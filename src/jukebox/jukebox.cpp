// nier - the Jukebox engine (jukebox.h): the catalog read from the game data, the decoders (ADX/AAX, MP3, WAV), the
// mixer thread and its SDL stream, the playlists' sequencer, the game's BGM held while ours plays, the saved library.

#include "jukebox.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <thread>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <toml++/toml.hpp>

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#if defined(NIER_JUKEBOX_MP3)
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

#include "../game_assets.h"
#include "../game_text.h"
#include "../pc_options.h"
#include "cri.h"

// The game's BGM player (jukebox.h): sub_82115620(player, cue, f1, f2) starts a BGM.
extern "C" void __imp__sub_82115620(PPCContext& ctx, uint8_t* base);

namespace nier::jukebox {
namespace {

using rex::X_STATUS;
constexpr int kRate = 48000;
constexpr uint32_t kBgmPlayerDefault = 0x8447C7ACu;  // the sound manager 0x8447BB90 + 3100
const char* kContainer = "game:\\media\\layer1.cpk";

// ---- Small things ----

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
bool EndsWith(const std::string& s, std::string_view e) {
  return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}
bool StartsWith(const std::string& s, std::string_view e) { return s.compare(0, e.size(), e) == 0; }
// A path as UTF-8 in a std::string (C++20's u8string is another type).
std::string U8(const std::filesystem::path& p) {
  const auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

// A file of the game data (the mounted ISO or folder), read by range.
class GameFile {
 public:
  GameFile() = default;
  ~GameFile() { Close(); }
  bool Open(std::string_view path) {
    Close();
    auto* fs = REX_KERNEL_FS();
    rex::filesystem::Entry* entry = fs ? fs->ResolvePath(path) : nullptr;
    if (entry && entry->Open(rex::filesystem::FileAccess::kGenericRead, &file_) != X_STATUS_SUCCESS) file_ = nullptr;
    return file_ != nullptr;
  }
  void Close() {
    if (file_) file_->Destroy();
    file_ = nullptr;
  }
  bool ok() const { return file_ != nullptr; }
  bool Read(uint64_t offset, size_t size, uint8_t* out) {
    size_t got = 0;
    return file_ && file_->ReadSync(std::span<uint8_t>(out, size), size_t(offset), &got) == X_STATUS_SUCCESS &&
           got == size;
  }
  bool Read(uint64_t offset, size_t size, std::vector<uint8_t>& out) {
    out.resize(size);
    return Read(offset, size, out.data());
  }

 private:
  rex::filesystem::File* file_ = nullptr;
};

// One AAX segment: an ADX stream in the container.
struct Segment {
  uint64_t frames_at = 0;  // the first frame, in the container
  uint32_t frames = 0;
  bool loop = false;
  cri::AdxInfo info;
};

bool ReadSegments(GameFile& f, uint64_t aax_at, uint64_t aax_size, std::vector<Segment>& out) {
  out.clear();
  std::vector<uint8_t> head;
  const size_t n = size_t(std::min<uint64_t>(aax_size, 4096));
  if (!f.Read(aax_at, n, head)) return false;
  cri::Table t;
  if (!cri::ParseUtf(head.data(), head.size(), 0, t) || t.name != "AAX") return false;
  for (size_t r = 0; r < t.rows.size(); ++r) {
    const cri::Cell* d = t.At(r, "data");
    if (!d) continue;
    uint8_t adx[64];
    if (!f.Read(aax_at + d->data_off, sizeof adx, adx)) return false;
    Segment s;
    if (!cri::ParseAdx(adx, sizeof adx, s.info)) return false;
    s.frames_at = aax_at + d->data_off + s.info.data_start;
    s.frames = uint32_t((uint64_t(s.info.samples) + 31) / 32);
    const uint64_t room = (d->data_size - s.info.data_start) / s.info.frame_bytes();
    s.frames = uint32_t(std::min<uint64_t>(s.frames, room));
    s.loop = t.Num(r, "lpflg") != 0;
    out.push_back(s);
  }
  return !out.empty();
}

// ---- State ----

struct Global {
  std::filesystem::path dir;  // next to nier.toml
  std::mutex catalog_mutex;
  Catalog catalog;
  std::atomic<bool> catalog_ready{false};
  std::mutex library_mutex;
  Library library;
  std::mutex user_mutex;
  std::vector<UserSong> user_songs;
  std::atomic<bool> bgm_seen{false};  // the game's BGM player exists (it started a BGM)
  std::atomic<uint32_t> bgm_player{kBgmPlayerDefault};  // as the game passes it (sub_82115620's r3)
  std::atomic<int32_t> last_cue{-1};
  // The page's Ligar / Desligar (2026-11-19): off at every start, and off the jukebox touches nothing of the game's
  // music (no hold, no replacement, no playback). A saved "Always play here" changed a place's music with the page
  // never opened.
  std::atomic<bool> enabled{false};
};
Global& G() {
  static Global g;
  return g;
}

// ---- Sources ----

// Converts what a decoder gives to 48 kHz (its own channels) and hands it out.
class Converter {
 public:
  ~Converter() {
    if (stream_) SDL_DestroyAudioStream(stream_);
  }
  bool Setup(int channels, int rate) {
    if (stream_ && channels == channels_ && rate == rate_) return true;
    if (stream_) SDL_DestroyAudioStream(stream_);
    SDL_AudioSpec src{SDL_AUDIO_F32, channels, rate}, dst{SDL_AUDIO_F32, channels, kRate};
    stream_ = SDL_CreateAudioStream(&src, &dst);
    channels_ = channels;
    rate_ = rate;
    return stream_ != nullptr;
  }
  void Put(const float* data, size_t frames) {
    if (stream_) SDL_PutAudioStreamData(stream_, data, int(frames * channels_ * sizeof(float)));
  }
  size_t Available() const { return stream_ ? size_t(SDL_GetAudioStreamAvailable(stream_)) / (channels_ * sizeof(float)) : 0; }
  size_t Get(float* out, size_t frames) {
    if (!stream_) return 0;
    const int got = SDL_GetAudioStreamData(stream_, out, int(frames * channels_ * sizeof(float)));
    return got > 0 ? size_t(got) / (channels_ * sizeof(float)) : 0;
  }
  void Flush() {
    if (stream_) SDL_FlushAudioStream(stream_);
  }
  void Clear() {
    if (stream_) SDL_ClearAudioStream(stream_);
  }
  int channels() const { return channels_; }

 private:
  SDL_AudioStream* stream_ = nullptr;
  int channels_ = 0, rate_ = 0;
};

// What a source hands out: per layer, 48 kHz stereo (mixed by the voice with the layers' gains).
class Source {
 public:
  virtual ~Source() = default;
  // Adds n frames of stereo, layer by layer: out[layer][2 * n]. Returns false once it is over.
  virtual bool Render(GameFile& f, std::array<std::vector<float>, kMaxLayers>& out, size_t n) = 0;
  virtual void Seek(GameFile& f, double seconds) = 0;
  virtual double Length() const = 0;  // one pass; 0 unknown
  virtual bool Loops() const { return false; }
  virtual int Layers() const { return 1; }
};

// One AAX of a game song.
class StemPlayer {
 public:
  bool Open(GameFile& f, const Stem& stem) {
    stem_ = stem;
    if (!ReadSegments(f, stem.offset, stem.size, segs_)) return false;
    return conv_.Setup(segs_[0].info.channels, segs_[0].info.rate);
  }
  // Adds n frames (stereo, 48 kHz) times gain into out; false once over.
  bool Render(GameFile& f, float* out, size_t n, float gain) {
    const int ch = conv_.channels();
    while (conv_.Available() < n && !ended_) Decode(f);
    tmp_.resize(n * size_t(ch));
    const size_t got = conv_.Get(tmp_.data(), n);
    const float g = gain * stem_.volume;
    const float gl = stem_.pan > 0 ? 0.0f : 1.0f, gr = stem_.pan < 0 ? 0.0f : 1.0f;
    for (size_t i = 0; i < got; ++i) {
      if (ch == 1) {
        out[2 * i] += tmp_[i] * g * gl;
        out[2 * i + 1] += tmp_[i] * g * gr;
      } else {
        out[2 * i] += tmp_[i * ch] * g;
        out[2 * i + 1] += tmp_[i * ch + 1] * g;
      }
    }
    return !(ended_ && got < n);
  }
  void Seek(GameFile& f, double seconds) {
    (void)f;
    conv_.Clear();
    ended_ = false;
    hist_ = {};
    uint64_t want = uint64_t(std::max(0.0, seconds) * segs_[0].info.rate);
    seg_ = 0;
    frame_ = 0;
    // Through the intro, then into the loop (as many times round as it takes).
    for (size_t i = 0; i < segs_.size(); ++i) {
      const uint64_t len = uint64_t(segs_[i].frames) * 32;
      if (want < len || (i + 1 == segs_.size() && segs_[i].loop)) {
        seg_ = i;
        if (segs_[i].loop && len) want %= len;
        frame_ = uint32_t(want / 32);
        return;
      }
      want -= len;
    }
    ended_ = true;
  }
  double Length() const {
    uint64_t s = 0;
    for (const Segment& g : segs_) s += uint64_t(g.frames) * 32;
    return segs_.empty() ? 0.0 : double(s) / segs_[0].info.rate;
  }
  bool Loops() const { return !segs_.empty() && segs_.back().loop; }
  const Stem& stem() const { return stem_; }

 private:
  void Decode(GameFile& f) {
    if (seg_ >= segs_.size()) {
      ended_ = true;
      conv_.Flush();
      return;
    }
    const Segment& s = segs_[seg_];
    if (frame_ >= s.frames) {
      if (s.loop) {
        frame_ = 0;
      } else {
        ++seg_;
        frame_ = 0;
        if (seg_ >= segs_.size()) {
          ended_ = true;
          conv_.Flush();
        }
      }
      return;
    }
    const uint32_t count = std::min<uint32_t>(64, s.frames - frame_);
    const uint32_t fb = s.info.frame_bytes();
    raw_.resize(size_t(count) * fb);
    if (!f.Read(s.frames_at + uint64_t(frame_) * fb, raw_.size(), raw_.data())) {
      ended_ = true;
      conv_.Flush();
      return;
    }
    pcm_.resize(size_t(count) * 32 * s.info.channels);
    for (uint32_t i = 0; i < count; ++i)
      cri::DecodeAdxFrame(raw_.data() + size_t(i) * fb, s.info, hist_, pcm_.data() + size_t(i) * 32 * s.info.channels);
    frame_ += count;
    conv_.Put(pcm_.data(), size_t(count) * 32);
  }

  Stem stem_;
  std::vector<Segment> segs_;
  size_t seg_ = 0;
  uint32_t frame_ = 0;
  cri::AdxHistory hist_;
  Converter conv_;
  bool ended_ = false;
  std::vector<uint8_t> raw_;
  std::vector<float> pcm_, tmp_;
};

class SongSource final : public Source {
 public:
  bool Open(GameFile& f, const Song& song) {
    song_ = song;
    for (const Stem& s : song.stems) {
      auto p = std::make_unique<StemPlayer>();
      if (p->Open(f, s)) stems_.push_back(std::move(p));
    }
    return !stems_.empty();
  }
  bool Render(GameFile& f, std::array<std::vector<float>, kMaxLayers>& out, size_t n) override {
    bool any = false;
    for (auto& s : stems_) {
      const int layer = std::clamp(s->stem().layer, 0, kMaxLayers - 1);
      any |= s->Render(f, out[size_t(layer)].data(), n, song_.volume);
    }
    return any;
  }
  void Seek(GameFile& f, double seconds) override {
    for (auto& s : stems_) s->Seek(f, seconds);
  }
  double Length() const override { return stems_.empty() ? 0.0 : stems_[0]->Length(); }
  bool Loops() const override { return !stems_.empty() && stems_[0]->Loops(); }
  int Layers() const override {
    int n = 1;
    for (auto& s : stems_) n = std::max(n, s->stem().layer + 1);
    return n;
  }

 private:
  Song song_;
  std::vector<std::unique_ptr<StemPlayer>> stems_;
};

// A WAV of the Music folder: decoded whole at the start (SDL).
class WavSource final : public Source {
 public:
  bool Open(const std::filesystem::path& path) {
    SDL_AudioSpec spec{};
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path.string().c_str(), &spec, &buf, &len)) return false;
    SDL_AudioSpec dst{SDL_AUDIO_F32, 2, kRate};
    Uint8* conv = nullptr;
    int conv_len = 0;
    const bool ok = SDL_ConvertAudioSamples(&spec, buf, int(len), &dst, &conv, &conv_len);
    SDL_free(buf);
    if (!ok) return false;
    pcm_.assign(reinterpret_cast<float*>(conv), reinterpret_cast<float*>(conv) + conv_len / sizeof(float));
    SDL_free(conv);
    return !pcm_.empty();
  }
  bool Render(GameFile&, std::array<std::vector<float>, kMaxLayers>& out, size_t n) override {
    const size_t frames = pcm_.size() / 2;
    size_t i = 0;
    for (; i < n && pos_ < frames; ++i, ++pos_) {
      out[0][2 * i] += pcm_[2 * pos_];
      out[0][2 * i + 1] += pcm_[2 * pos_ + 1];
    }
    return pos_ < frames;
  }
  void Seek(GameFile&, double seconds) override { pos_ = std::min(pcm_.size() / 2, size_t(std::max(0.0, seconds) * kRate)); }
  double Length() const override { return double(pcm_.size() / 2) / kRate; }

 private:
  std::vector<float> pcm_;
  size_t pos_ = 0;
};

#if defined(NIER_JUKEBOX_MP3)
// An MP3 of the Music folder, decoded as it plays (libavcodec's decoder and parser, from the SDK).
class Mp3Source final : public Source {
 public:
  ~Mp3Source() override { Close(); }
  bool Open(const std::filesystem::path& path) {
    path_ = path;
    file_.open(path, std::ios::binary);
    if (!file_) return false;
    file_.seekg(0, std::ios::end);
    file_size_ = uint64_t(file_.tellg());
    file_.seekg(0);
    data_start_ = SkipId3();
    file_.seekg(std::streamoff(data_start_));
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
    if (!codec) return false;
    ctx_ = avcodec_alloc_context3(codec);
    parser_ = av_parser_init(AV_CODEC_ID_MP3);
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    return ctx_ && parser_ && packet_ && frame_ && avcodec_open2(ctx_, codec, nullptr) == 0;
  }
  bool Render(GameFile&, std::array<std::vector<float>, kMaxLayers>& out, size_t n) override {
    while (conv_.Available() < n && !ended_) Decode();
    tmp_.resize(n * 2);
    size_t got = 0;
    if (conv_.channels() > 0) {
      const int ch = conv_.channels();
      tmp_.resize(n * size_t(ch));
      got = conv_.Get(tmp_.data(), n);
      for (size_t i = 0; i < got; ++i) {
        out[0][2 * i] += tmp_[i * ch];
        out[0][2 * i + 1] += tmp_[i * ch + (ch > 1 ? 1 : 0)];
      }
    }
    return !(ended_ && got < n);
  }
  void Seek(GameFile&, double seconds) override {
    // By the average bit rate (the decoder settles within a frame).
    conv_.Clear();
    avcodec_flush_buffers(ctx_);
    ended_ = false;
    pending_.clear();
    const double length = Length();
    const double frac = length > 0 ? std::clamp(seconds / length, 0.0, 1.0) : 0.0;
    file_.clear();
    file_.seekg(std::streamoff(data_start_ + uint64_t(frac * double(file_size_ - data_start_))));
  }
  double Length() const override {
    return bit_rate_ > 0 ? double(file_size_ - data_start_) * 8.0 / double(bit_rate_) : 0.0;
  }

 private:
  void Close() {
    if (frame_) av_frame_free(&frame_);
    if (packet_) av_packet_free(&packet_);
    if (parser_) av_parser_close(parser_);
    if (ctx_) avcodec_free_context(&ctx_);
  }
  uint64_t SkipId3() {
    char h[10] = {};
    file_.read(h, 10);
    if (file_.gcount() == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
      const uint32_t size = (uint32_t(h[6] & 0x7F) << 21) | (uint32_t(h[7] & 0x7F) << 14) | (uint32_t(h[8] & 0x7F) << 7) |
                            uint32_t(h[9] & 0x7F);
      return 10 + size + ((h[5] & 0x10) ? 10 : 0);
    }
    return 0;
  }
  void Decode() {
    if (pending_.size() < 4096 && file_) {
      char buf[16384];
      file_.read(buf, sizeof buf);
      pending_.insert(pending_.end(), buf, buf + file_.gcount());
    }
    if (pending_.empty()) {
      avcodec_send_packet(ctx_, nullptr);
      Drain();
      ended_ = true;
      conv_.Flush();
      return;
    }
    uint8_t* data = nullptr;
    int size = 0;
    const int used = av_parser_parse2(parser_, ctx_, &data, &size, pending_.data(), int(pending_.size()),
                                      AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (used < 0) {
      ended_ = true;
      return;
    }
    // The packet may point into pending_ itself (the parser hands out the input when a whole frame is in it):
    // decoded before pending_ moves. Erasing first fed the decoder shifted bytes, a crackle at every frame
    // (the player's own songs sounded "crunchy", 2026-11-17).
    // Measured (NIER_JUKEBOX_DUMP, 40 s of an MP3): 79 clicks before, none after.
    if (size > 0) {
      packet_->data = data;
      packet_->size = size;
      if (avcodec_send_packet(ctx_, packet_) == 0) Drain();
    }
    pending_.erase(pending_.begin(), pending_.begin() + used);
    if (size <= 0 && used == 0 && !file_) pending_.clear();
  }
  void Drain() {
    while (avcodec_receive_frame(ctx_, frame_) == 0) {
      const int ch = std::max(1, frame_->channels);
      if (ctx_->bit_rate > 0) bit_rate_ = ctx_->bit_rate;
      conv_.Setup(std::min(ch, 2), frame_->sample_rate);
      const int outch = std::min(ch, 2);
      inter_.resize(size_t(frame_->nb_samples) * outch);
      for (int i = 0; i < frame_->nb_samples; ++i) {
        for (int c = 0; c < outch; ++c) {
          float v = 0;
          switch (frame_->format) {
            case AV_SAMPLE_FMT_FLTP: v = reinterpret_cast<float*>(frame_->extended_data[c])[i]; break;
            case AV_SAMPLE_FMT_FLT: v = reinterpret_cast<float*>(frame_->extended_data[0])[i * ch + c]; break;
            case AV_SAMPLE_FMT_S16P: v = reinterpret_cast<int16_t*>(frame_->extended_data[c])[i] / 32768.0f; break;
            case AV_SAMPLE_FMT_S16: v = reinterpret_cast<int16_t*>(frame_->extended_data[0])[i * ch + c] / 32768.0f; break;
            default: break;
          }
          inter_[size_t(i) * outch + c] = v;
        }
      }
      conv_.Put(inter_.data(), size_t(frame_->nb_samples));
    }
  }

  std::filesystem::path path_;
  std::ifstream file_;
  uint64_t file_size_ = 0, data_start_ = 0;
  int64_t bit_rate_ = 0;
  AVCodecContext* ctx_ = nullptr;
  AVCodecParserContext* parser_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  std::vector<uint8_t> pending_;
  std::vector<float> inter_, tmp_;
  Converter conv_;
  bool ended_ = false;
};
#endif

std::unique_ptr<Source> OpenSource(GameFile& f, const Item& item) {
  if (item.file) {
    const std::filesystem::path path = MusicFolder() / std::filesystem::path(std::u8string(item.key.begin(), item.key.end()));
    const std::string ext = Lower(path.extension().string());
    if (ext == ".wav") {
      auto s = std::make_unique<WavSource>();
      if (s->Open(path)) return s;
    }
#if defined(NIER_JUKEBOX_MP3)
    if (ext == ".mp3") {
      auto s = std::make_unique<Mp3Source>();
      if (s->Open(path)) return s;
    }
#endif
    REXLOG_WARN("Jukebox: could not open {}", path.string());
    return nullptr;
  }
  std::lock_guard lock(G().catalog_mutex);
  const Song* song = G().catalog.Find(item.key);
  if (!song) return nullptr;
  auto s = std::make_unique<SongSource>();
  if (s->Open(f, *song)) return s;
  return nullptr;
}

// ---- The player ----

struct Ramp {
  float now = 1, target = 1, step = 0;
  void Set(float t, float seconds) {
    target = t;
    step = seconds > 0 ? std::fabs(t - now) / (seconds * kRate) : 1.0f;
  }
  float Next() {
    if (now < target) now = std::min(target, now + step);
    else if (now > target) now = std::max(target, now - step);
    return now;
  }
};

// The timeline section holding a time (the first; -1 none).
int SectionAt(const Item& item, double seconds) {
  for (size_t i = 0; i < item.sections.size(); ++i)
    if (seconds >= item.sections[i].from && seconds < item.sections[i].to) return int(i);
  return -1;
}

struct Voice {
  std::unique_ptr<Source> source;
  Item item;
  std::string title;
  std::vector<std::string> parts;
  std::array<Ramp, kMaxLayers> layers;
  Ramp fade;
  uint64_t frames = 0;  // played
  std::array<float, kMaxLayers> base{1, 1, 1, 1};  // the mix outside the timeline's sections
  int section = -1;  // the section applied (-1 none)
  double length = 0;
  bool loops = false;
  bool ended = false;
  std::array<float, kMaxLayers> levels{};
  bool official = false, official_live = false;  // PlayOfficial: the layers follow the game's (GameTick)
};

class Player {
 public:
  void Start() {
    thread_ = std::thread([this] { Run(); });
    thread_.detach();
  }

  void Command(std::function<void(Player&)> fn) {
    std::lock_guard lock(mutex_);
    commands_.push_back(std::move(fn));
  }

  // Under mutex_ (the commands run in the player thread, between blocks).
  void PlayLocked(const Item& item, int playlist, int index) {
    if (!G().enabled.load()) return;  // switched off: nothing plays
    auto voice = std::make_unique<Voice>();
    voice->source = OpenSource(file_, item);
    if (!voice->source) return;
    voice->item = item;
    voice->title = Title(item.key, item.file);
    voice->length = voice->source->Length();
    voice->loops = voice->source->Loops();
    if (!item.file) {
      std::lock_guard lock(G().catalog_mutex);
      if (const Song* s = G().catalog.Find(item.key)) {
        voice->parts.assign(kMaxLayers, std::string());
        for (const Stem& st : s->stems) {
          auto& p = voice->parts[size_t(std::clamp(st.layer, 0, kMaxLayers - 1))];
          if (p.empty()) p = PartName(st.part);
        }
        while (!voice->parts.empty() && voice->parts.back().empty()) voice->parts.pop_back();
      }
    }
    voice->base = item.layers;
    voice->section = SectionAt(item, 0.0);
    const auto& start = voice->section >= 0 ? item.sections[size_t(voice->section)].layers : item.layers;
    for (int i = 0; i < kMaxLayers; ++i) {
      voice->layers[size_t(i)].now = voice->layers[size_t(i)].target = start[size_t(i)];
    }
    float crossfade;
    {
      std::lock_guard lock(G().library_mutex);
      crossfade = G().library.crossfade;
    }
    voice->fade.now = current_ ? 0.0f : 1.0f;
    voice->fade.Set(1.0f, crossfade);
    if (current_) {
      current_->fade.Set(0.0f, crossfade);
      old_ = std::move(current_);
    }
    current_ = std::move(voice);
    playlist_ = playlist;
    index_ = index;
    paused_ = false;
  }

  void PlayPlaylistLocked(int playlist, int index) {
    Playlist pl;
    {
      std::lock_guard lock(G().library_mutex);
      if (playlist < 0 || playlist >= int(G().library.playlists.size())) return;
      pl = G().library.playlists[size_t(playlist)];
    }
    if (pl.items.empty()) return;
    order_.resize(pl.items.size());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = int(i);
    if (pl.shuffle) std::shuffle(order_.begin(), order_.end(), rng_);
    index = std::clamp(index, 0, int(pl.items.size()) - 1);
    // With shuffle the chosen one goes first.
    if (pl.shuffle) std::iter_swap(order_.begin(), std::find(order_.begin(), order_.end(), index));
    order_pos_ = pl.shuffle ? 0 : index;
    PlayLocked(pl.items[size_t(order_[size_t(order_pos_)])], playlist, order_[size_t(order_pos_)]);
  }

  void StepLocked(int d) {
    if (playlist_ < 0) {
      if (d > 0) StopLocked();
      else if (current_) current_->source->Seek(file_, 0), current_->frames = 0;
      return;
    }
    Playlist pl;
    {
      std::lock_guard lock(G().library_mutex);
      if (playlist_ >= int(G().library.playlists.size())) return StopLocked();
      pl = G().library.playlists[size_t(playlist_)];
    }
    if (pl.items.empty() || order_.size() != pl.items.size()) return StopLocked();
    int pos = order_pos_;
    if (pl.repeat != 2 || d < 0) pos += d;
    if (pos >= int(order_.size())) {
      if (pl.repeat == 0) return StopLocked();
      if (pl.shuffle) std::shuffle(order_.begin(), order_.end(), rng_);
      pos = 0;
    }
    if (pos < 0) pos = int(order_.size()) - 1;
    order_pos_ = pos;
    const int keep = playlist_;
    PlayLocked(pl.items[size_t(order_[size_t(pos)])], keep, order_[size_t(pos)]);
  }

  void StopLocked() {
    float crossfade;
    {
      std::lock_guard lock(G().library_mutex);
      crossfade = G().library.crossfade;
    }
    if (current_) {
      current_->fade.Set(0.0f, std::max(0.3f, crossfade * 0.5f));
      old_ = std::move(current_);
    }
    playlist_ = index_ = -1;
    auto_cue_ = -1;
  }

  NowPlaying Now() {
    std::lock_guard lock(mutex_);
    NowPlaying n;
    if (!current_) return n;
    n.playing = true;
    n.paused = paused_;
    n.item = current_->item;
    for (int i = 0; i < kMaxLayers; ++i) n.item.layers[size_t(i)] = current_->layers[size_t(i)].target;
    n.title = current_->title;
    n.position = double(current_->frames) / kRate;
    n.length = current_->length;
    if (current_->loops && current_->length > 0) n.position = std::fmod(n.position, current_->length);
    n.playlist = playlist_;
    n.playlist_index = index_;
    n.parts = current_->parts;
    n.levels = current_->levels;
    n.section = current_->section;
    n.elapsed = double(current_->frames) / kRate;
    n.official = current_->official;
    n.official_live = current_->official_live;
    return n;
  }
  GameMusic Game() {
    std::lock_guard lock(mutex_);
    return game_;
  }

  bool paused_ = false;
  std::unique_ptr<Voice> current_, old_;
  int playlist_ = -1, index_ = -1;
  int32_t auto_cue_ = -1;  // the game's cue whose replacement plays now
  bool was_enabled_ = false;  // GameTick: the switch as last seen
  GameFile file_;

 private:
  void Run() {
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
    SDL_AudioStream* out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!out) {
      REXLOG_ERROR("Jukebox: no audio stream ({})", SDL_GetError());
      return;
    }
    SDL_ResumeAudioStreamDevice(out);
    file_.Open(kContainer);
    constexpr size_t kBlock = 1024;
    std::array<std::vector<float>, kMaxLayers> layers;
    for (auto& l : layers) l.resize(kBlock * 2);
    std::vector<float> mix(kBlock * 2), voice_mix(kBlock * 2);
    auto last_game = std::chrono::steady_clock::now();
    REXLOG_INFO("Jukebox: player ready");
    // Tests (no page needed): NIER_JUKEBOX_AUTOPLAY plays a file of the Music folder (or "game:<synth>") once
    // the catalog is read; NIER_JUKEBOX_DUMP writes what the player sends out to a WAV (its first 40 s).
    std::string autoplay = std::getenv("NIER_JUKEBOX_AUTOPLAY") ? std::getenv("NIER_JUKEBOX_AUTOPLAY") : "";
    std::ofstream dump;
    uint64_t dumped = 0;
    if (const char* d = std::getenv("NIER_JUKEBOX_DUMP")) {
      dump.open(d, std::ios::binary | std::ios::trunc);
      const char header[44] = {};
      dump.write(header, 44);
    }
    for (;;) {
      {
        std::vector<std::function<void(Player&)>> cmds;
        {
          std::lock_guard lock(mutex_);
          cmds.swap(commands_);
        }
        for (auto& c : cmds) {
          std::lock_guard lock(mutex_);
          c(*this);
        }
      }
      if (!autoplay.empty() && G().catalog_ready) {
        Item it;
        it.file = !StartsWith(autoplay, "game:");
        it.key = it.file ? autoplay : autoplay.substr(5);
        // NIER_JUKEBOX_AUTOPLAY_SECTIONS "0-10:1,0,0,0;10-20:0,1,0,0" (a timeline to test).
        if (const char* secs = std::getenv("NIER_JUKEBOX_AUTOPLAY_SECTIONS")) {
          std::string s = secs;
          for (size_t p = 0; p < s.size();) {
            size_t e = s.find(';', p);
            if (e == std::string::npos) e = s.size();
            Section sec;
            float l[4] = {1, 1, 1, 1};
            if (std::sscanf(s.substr(p, e - p).c_str(), "%lf-%lf:%f,%f,%f,%f", &sec.from, &sec.to, &l[0], &l[1], &l[2], &l[3]) >= 3) {
              for (int i = 0; i < 4; ++i) sec.layers[size_t(i)] = l[i];
              it.sections.push_back(sec);
            }
            p = e + 1;
          }
        }
        REXLOG_INFO("Jukebox: autoplay {}", it.key);
        G().enabled = true;  // the test switches it on
        std::lock_guard lock(mutex_);
        PlayLocked(it, -1, -1);
        autoplay.clear();
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - last_game > std::chrono::milliseconds(30)) {
        last_game = now;
        std::lock_guard lock(mutex_);
        GameTick();
      }
      bool active;
      {
        std::lock_guard lock(mutex_);
        active = (current_ || old_) && !paused_;
      }
      const size_t queued = size_t(SDL_GetAudioStreamQueued(out)) / (2 * sizeof(float));
      if (!active || queued > size_t(kRate) / 6) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      float volume;
      {
        std::lock_guard lock(G().library_mutex);
        volume = G().library.volume;
      }
      std::fill(mix.begin(), mix.end(), 0.0f);
      {
        std::lock_guard lock(mutex_);
        // The timeline: a new section (or out of one) brings its mix in with a short fade.
        if (Voice* v = current_.get(); v && !v->item.sections.empty()) {
          const int sec = SectionAt(v->item, double(v->frames) / kRate);
          if (sec != v->section) {
            const float fade = v->section == -2 ? 0.15f : 0.6f;  // -2: edited just now (heard at once)
            v->section = sec;
            const auto& want = sec >= 0 ? v->item.sections[size_t(sec)].layers : v->base;
            for (int i = 0; i < kMaxLayers; ++i) v->layers[size_t(i)].Set(want[size_t(i)], fade);
          }
        }
        for (Voice* v : {old_.get(), current_.get()}) {
          if (!v) continue;
          for (auto& l : layers) std::fill(l.begin(), l.end(), 0.0f);
          if (!v->ended && !v->source->Render(file_, layers, kBlock)) v->ended = true;
          for (int li = 0; li < kMaxLayers; ++li) {
            float peak = 0;
            for (size_t i = 0; i < kBlock * 2; ++i) peak = std::max(peak, std::fabs(layers[size_t(li)][i]));
            v->levels[size_t(li)] = std::max(peak, v->levels[size_t(li)] * 0.85f);
          }
          for (size_t i = 0; i < kBlock; ++i) {
            const float fade = v->fade.Next();
            float l = 0, r = 0;
            for (int li = 0; li < kMaxLayers; ++li) {
              const float g = v->layers[size_t(li)].Next();
              l += layers[size_t(li)][2 * i] * g;
              r += layers[size_t(li)][2 * i + 1] * g;
            }
            mix[2 * i] += l * fade;
            mix[2 * i + 1] += r * fade;
          }
          v->frames += kBlock;
        }
        if (old_ && (old_->fade.now <= 0.0f || old_->ended)) old_.reset();
        if (current_ && current_->fade.target <= 0.0f && current_->fade.now <= 0.0f) current_.reset();
        Sequence();
      }
      for (float& s : mix) {
        s *= volume;
        if (s > 0.9f) s = 0.9f + 0.1f * std::tanh((s - 0.9f) / 0.1f);
        if (s < -0.9f) s = -0.9f - 0.1f * std::tanh((-s - 0.9f) / 0.1f);
      }
      SDL_PutAudioStreamData(out, mix.data(), int(mix.size() * sizeof(float)));
      if (dump.is_open()) {
        for (float s : mix) {
          const int16_t v = int16_t(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
          dump.write(reinterpret_cast<const char*>(&v), 2);
        }
        dumped += kBlock;
        if (dumped >= uint64_t(kRate) * 40) {
          auto put32 = [&](uint32_t at, uint32_t v) {
            dump.seekp(at);
            dump.write(reinterpret_cast<const char*>(&v), 4);
          };
          const uint32_t bytes = uint32_t(dumped * 4);
          dump.seekp(0);
          dump.write("RIFF\0\0\0\0WAVEfmt ", 16);
          put32(4, 36 + bytes);
          put32(16, 16);
          const uint16_t fmt[2] = {1, 2};
          dump.seekp(20);
          dump.write(reinterpret_cast<const char*>(fmt), 4);
          put32(24, kRate);
          put32(28, kRate * 4);
          const uint16_t align[2] = {4, 16};
          dump.seekp(32);
          dump.write(reinterpret_cast<const char*>(align), 4);
          dump.seekp(36);
          dump.write("data", 4);
          put32(40, bytes);
          dump.close();
          REXLOG_INFO("Jukebox: dump written");
        }
      }
    }
  }

  // A playlist item's time is up, or the song ended: the next one.
  void Sequence() {
    if (!current_ || current_->fade.target <= 0.0f) return;
    const double pos = double(current_->frames) / kRate;
    double limit = current_->item.seconds;
    if (limit <= 0 && playlist_ >= 0 && current_->loops)
      limit = std::max(current_->length > 0 ? current_->length : 0.0, 180.0);  // a looping song: once round, 3 min at least
    float crossfade;
    {
      std::lock_guard lock(G().library_mutex);
      crossfade = G().library.crossfade;
    }
    const bool time_up = limit > 0 && pos >= limit - crossfade;
    if (current_->ended || time_up) {
      if (playlist_ >= 0) StepLocked(1);
      else if (current_->ended) StopLocked();
    }
  }

  // Each ~30 ms: the game's BGM (what plays, its layers); held silent while ours plays; the replacements.
  void GameTick() {
    uint8_t* gb = pc_options_state().guest_base.load();
    if (!gb || !G().bgm_seen) return;
    auto rd = [&](uint32_t a) { return cri::Be32(gb + a); };
    auto rdf = [&](uint32_t a) {
      const uint32_t v = rd(a);
      float f;
      std::memcpy(&f, &v, 4);
      return f;
    };
    auto wrf = [&](uint32_t a, float f) {
      uint32_t v;
      std::memcpy(&v, &f, 4);
      gb[a] = uint8_t(v >> 24);
      gb[a + 1] = uint8_t(v >> 16);
      gb[a + 2] = uint8_t(v >> 8);
      gb[a + 3] = uint8_t(v);
    };
    auto valid = [](uint32_t p) { return p >= 0x40000000u && p < 0xA0000000u && (p & 3) == 0; };
    const uint32_t kBgmPlayer = G().bgm_player.load();
    const int32_t cue = int32_t(rd(kBgmPlayer + 8));
    // The tracks of the list (the playing one and those fading).
    uint32_t tracks[8];
    int count = 0;
    for (uint32_t t = rd(kBgmPlayer + 28); valid(t) && count < 8; t = rd(t + 156)) tracks[count++] = t;
    GameMusic g;
    g.cue = cue;
    for (int i = 0; i < count; ++i) {
      if (int32_t(rd(tracks[i] + 16)) != cue) continue;
      g.layer_count = std::clamp(int(rd(tracks[i] + 24)), 0, kMaxLayers);
      g.volume = rdf(tracks[i] + 32);
      for (int l = 0; l < g.layer_count; ++l) g.layers[size_t(l)] = rdf(tracks[i] + 56 + uint32_t(l) * 24);
    }
    const bool enabled = G().enabled.load();
    if (cue != game_.cue) {
      REXLOG_INFO("Jukebox: game BGM cue {}", cue);
      if (enabled) OnGameCue(cue);
    }
    // Just switched on: the place's replacement (if any) at once.
    if (enabled && !was_enabled_ && cue >= 0) OnGameCue(cue);
    was_enabled_ = enabled;
    // The official mode: while the game plays this song (its place), our layers take the game's values as its
    // scripts move them (each layer's current volume, already faded by the game). Elsewhere the last mix stays.
    if (current_ && current_->official) {
      bool live = false;
      if (cue >= 0 && g.layer_count > 0) {
        std::lock_guard lock(G().catalog_mutex);
        if (const Song* s = G().catalog.Find(current_->item.key))
          live = std::find(s->cues.begin(), s->cues.end(), uint32_t(cue)) != s->cues.end();
      }
      if (live && !current_->official_live) REXLOG_INFO("Jukebox: official mode follows cue {}", cue);
      current_->official_live = live;
      if (live) {
        for (int l = 0; l < g.layer_count; ++l) {
          const float v = std::clamp(g.layers[size_t(l)], 0.0f, 1.0f);
          current_->layers[size_t(l)].Set(v, 0.05f);
          current_->item.layers[size_t(l)] = current_->base[size_t(l)] = v;
        }
      }
    }
    bool hold;
    {
      std::lock_guard lock(G().library_mutex);
      hold = G().library.hold_game_music;
    }
    hold = hold && enabled && current_ && current_->fade.target > 0.0f && !paused_;
    if (hold) {
      for (int i = 0; i < count; ++i) {
        const uint32_t t = tracks[i];
        if (!holding_) saved_volume_ = std::max(saved_volume_, rdf(t + 40));
        wrf(t + 32, 0.0f);
        wrf(t + 36, 0.0f);
        wrf(t + 40, 0.0f);
        wrf(t + 44, 0.0f);
        wrf(t + 48, 0.0f);
        gb[t + 52] = 1;
      }
      holding_ = true;
    } else if (holding_) {
      // Back with a fade, as the game's own _SetBgmMasterVolume would.
      const float back = saved_volume_ > 0.01f ? saved_volume_ : 1.0f;
      for (int i = 0; i < count; ++i) {
        const uint32_t t = tracks[i];
        wrf(t + 36, rdf(t + 32));
        wrf(t + 40, back);
        wrf(t + 44, 2.0f);
        wrf(t + 48, 0.0f);
        gb[t + 52] = 1;
      }
      holding_ = false;
      saved_volume_ = 0;
    }
    g.held = holding_;
    game_ = g;
  }

  void OnGameCue(int32_t cue) {
    Item replacement;
    bool replace = false;
    {
      std::lock_guard lock(G().library_mutex);
      auto it = G().library.replace.find(uint32_t(cue));
      if (cue >= 0 && it != G().library.replace.end()) {
        replacement = it->second;
        replace = true;
      }
    }
    if (replace) {
      PlayLocked(replacement, -1, -1);
      auto_cue_ = cue;
    } else if (auto_cue_ >= 0 && current_ && playlist_ < 0) {
      // The place's replacement ends with it.
      StopLocked();
    }
  }

  std::mutex mutex_;
  std::thread thread_;
  std::vector<std::function<void(Player&)>> commands_;
  std::vector<int> order_;
  int order_pos_ = 0;
  std::mt19937 rng_{std::random_device{}()};
  GameMusic game_;
  bool holding_ = false;
  float saved_volume_ = 0;
};

Player& P() {
  static Player p;
  return p;
}

// ---- The catalog ----

std::string PartOf(const std::string& wave, const std::string& base) {
  std::string p = wave;
  for (std::string_view suffix : {"_wav.aax", ".aax"})
    if (EndsWith(p, suffix)) p.resize(p.size() - suffix.size());
  // Files of one song may start with different numbers (096_king_first_B_vocal, 097_king_first_B_oche).
  auto unnumbered = [](const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    return i > 0 && i < s.size() && s[i] == '_' ? s.substr(i + 1) : s;
  };
  if (StartsWith(p, base)) p = p.substr(base.size());
  else if (StartsWith(unnumbered(p), unnumbered(base))) p = unnumbered(p).substr(unnumbered(base).size());
  while (!p.empty() && p[0] == '_') p.erase(0, 1);
  for (bool again = true; again;) {
    again = false;
    for (std::string_view suffix : {"_cmp2", "_cmp", "_loop", "_lp", "cmp2", "cmp"})
      if (EndsWith(p, suffix) && p.size() > suffix.size()) {
        p.resize(p.size() - suffix.size());
        again = true;
      }
    if (p == "cmp" || p == "loop" || p == "cmp2") p.clear();
  }
  return p;
}

std::string CommonBase(const std::vector<std::string>& waves, bool numbered_retry = true) {
  if (waves.empty()) return {};
  std::string base = waves[0];
  for (std::string_view suffix : {"_wav.aax", ".aax"})
    if (EndsWith(base, suffix)) base.resize(base.size() - suffix.size());
  if (waves.size() == 1) {
    for (bool again = true; again;) {
      again = false;
      for (std::string_view suffix : {"_cmp2", "_cmp", "_loop", "_lp"})
        if (EndsWith(base, suffix)) {
          base.resize(base.size() - suffix.size());
          again = true;
        }
    }
    return base;
  }
  for (const std::string& w : waves) {
    size_t i = 0;
    while (i < base.size() && i < w.size() && base[i] == w[i]) ++i;
    base.resize(i);
  }
  while (!base.empty() && base.back() != '_') base.pop_back();
  while (!base.empty() && base.back() == '_') base.pop_back();
  if (base.size() < 4 && numbered_retry) {
    // Different leading numbers: the common part after them (096_king_first_B_x, 097_king_first_B_y -> 096_king_first_B).
    // Once only (a name with no number before its first '_' stays as it is).
    std::vector<std::string> rest;
    for (const std::string& w : waves) {
      const size_t u = w.find('_');
      if (u == 0 || u == std::string::npos || !std::all_of(w.begin(), w.begin() + u, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        return base;
      rest.push_back(w.substr(u + 1));
    }
    std::string tail = CommonBase(rest, false);
    const size_t u = waves[0].find('_');
    if (!tail.empty() && u != std::string::npos) return waves[0].substr(0, u + 1) + tail;
  }
  return base;
}

void LoadCatalog() {
  Catalog cat;
  const auto files = ListCpk(kContainer);
  const CpkFile *csb_f = nullptr, *cpk_f = nullptr;
  for (const CpkFile& f : files) {
    if (f.dir == "AUDIO" && f.name == "NIER_BGM.CSB") csb_f = &f;
    if (f.dir == "AUDIO" && f.name == "NIER_BGM.CPK") cpk_f = &f;
  }
  GameFile file;
  std::vector<uint8_t> csb;
  if (!csb_f || !cpk_f || !ReadCpkFile(kContainer, *csb_f, csb) || !file.Open(kContainer)) {
    cat.error = "AUDIO/NIER_BGM not found";
    REXLOG_WARN("Jukebox: {}", cat.error);
    std::lock_guard lock(G().catalog_mutex);
    G().catalog = std::move(cat);
    G().catalog_ready = true;
    return;
  }
  // The inner CPK's files (AAX by name).
  std::map<std::string, std::pair<uint64_t, uint64_t>> waves;
  {
    std::vector<uint8_t> head;
    cri::Table header, toc;
    if (file.Read(cpk_f->offset, 0x800, head) && std::memcmp(head.data(), "CPK ", 4) == 0 &&
        cri::ParseUtf(head.data(), head.size(), 16, header)) {
      const uint64_t toc_at = header.Num(0, "TocOffset"), content = header.Num(0, "ContentOffset", toc_at);
      const uint64_t base = std::min(toc_at, content);
      std::vector<uint8_t> th, table;
      if (toc_at && file.Read(cpk_f->offset + toc_at, 16, th)) {
        const uint64_t size = cri::ChunkSize(th.data());
        if (size && size < (16u << 20) && file.Read(cpk_f->offset + toc_at + 16, size_t(size), table) &&
            cri::ParseUtf(table.data(), table.size(), 0, toc)) {
          for (size_t r = 0; r < toc.rows.size(); ++r)
            waves[toc.Str(r, "FileName")] = {cpk_f->offset + base + toc.Num(r, "FileOffset"), toc.Num(r, "FileSize")};
        }
      }
    }
  }
  // The cue sheet: cues -> synths -> their waveforms and AISAC layers.
  cri::Table top;
  std::map<std::string, cri::Table> tabs;
  if (cri::ParseUtf(csb.data(), csb.size(), 0, top)) {
    for (size_t r = 0; r < top.rows.size(); ++r) {
      const cri::Cell* d = top.At(r, "utf");
      cri::Table t;
      if (d && d->data_size > 8 && cri::ParseUtf(csb.data(), csb.size(), d->data_off, t)) tabs[top.Str(r, "name")] = t;
    }
  }
  const cri::Table& syn = tabs["SYNTH"];
  const cri::Table& cue = tabs["CUE"];
  auto synth_row = [&](const std::string& name) -> int {
    for (size_t r = 0; r < syn.rows.size(); ++r)
      if (syn.Str(r, "synname") == name) return int(r);
    return -1;
  };
  std::map<std::string, size_t> by_key;
  for (size_t r = 0; r < syn.rows.size(); ++r) {
    const std::string name = syn.Str(r, "synname");
    if (syn.Num(r, "syntype") != 1 || name == "Synth/0000" || by_key.count(name)) continue;
    Song song;
    song.key = name;
    song.volume = float(syn.Num(r, "volume", 1000)) / 1000.0f;
    std::string links = syn.Str(r, "lnkname");
    std::vector<std::string> wave_names;
    size_t p = 0;
    while (p < links.size()) {
      size_t e = links.find('\n', p);
      if (e == std::string::npos) e = links.size();
      const std::string child = links.substr(p, e - p);
      p = e + 1;
      if (child.empty()) continue;
      const int cr = synth_row(child);
      if (cr < 0) continue;
      std::string wave = syn.Str(size_t(cr), "lnkname");
      while (!wave.empty() && (wave.back() == '\n' || wave.back() == '\r')) wave.pop_back();
      if (StartsWith(wave, "Synth/")) wave = wave.substr(6);
      auto w = waves.find(wave);
      if (w == waves.end()) continue;
      Stem st;
      st.wave = wave;
      st.offset = w->second.first;
      st.size = w->second.second;
      st.volume = float(syn.Num(size_t(cr), "volume", 1000)) / 1000.0f;
      const std::string is = syn.Str(size_t(cr), "issetname");
      const size_t lp = is.find("layer");
      st.layer = lp != std::string::npos ? std::clamp(std::atoi(is.c_str() + lp + 5), 0, kMaxLayers - 1) : 0;
      const std::string lw = Lower(wave);
      if (lw.find("_l_wav") != std::string::npos) st.pan = -1;
      if (lw.find("_r_wav") != std::string::npos) st.pan = 1;
      if (lw.find("_ls_wav") != std::string::npos) st.pan = -1, st.volume *= 0.7f;
      if (lw.find("_rs_wav") != std::string::npos) st.pan = 1, st.volume *= 0.7f;
      song.stems.push_back(st);
      wave_names.push_back(wave);
    }
    if (song.stems.empty()) continue;
    song.base = CommonBase(wave_names);
    for (Stem& st : song.stems) st.part = PartOf(st.wave, song.base);
    std::sort(song.stems.begin(), song.stems.end(), [](const Stem& a, const Stem& b) { return a.layer < b.layer; });
    std::vector<Segment> segs;
    if (ReadSegments(file, song.stems[0].offset, song.stems[0].size, segs)) {
      uint64_t samples = 0;
      for (const Segment& s : segs) samples += uint64_t(s.frames) * 32;
      song.sample_rate = segs[0].info.rate;
      song.seconds = double(samples) / segs[0].info.rate;
      song.loops = segs.back().loop;
    }
    by_key[name] = cat.songs.size();
    cat.songs.push_back(std::move(song));
  }
  for (size_t r = 0; r < cue.rows.size(); ++r) {
    auto it = by_key.find(cue.Str(r, "synth"));
    if (it != by_key.end()) cat.songs[it->second].cues.push_back(uint32_t(cue.Num(r, "id")));
  }
  std::sort(cat.songs.begin(), cat.songs.end(), [](const Song& a, const Song& b) { return a.base < b.base; });
  cat.ready = true;
  REXLOG_INFO("Jukebox: {} songs ({} waveforms) in the game's music", cat.songs.size(), waves.size());
  std::lock_guard lock(G().catalog_mutex);
  G().catalog = std::move(cat);
  G().catalog_ready = true;
}

// ---- The library file ----

std::filesystem::path LibraryPath() {
  if (const char* p = std::getenv("NIER_JUKEBOX_LIBRARY")) return p;  // tests: a library of their own
  return G().dir / "jukebox.toml";
}

toml::array Layers(const std::array<float, kMaxLayers>& l) {
  toml::array a;
  for (float v : l) a.push_back(double(v));
  return a;
}
std::array<float, kMaxLayers> Layers(const toml::node* n) {
  std::array<float, kMaxLayers> l{1, 1, 1, 1};
  if (const toml::array* a = n ? n->as_array() : nullptr)
    for (size_t i = 0; i < a->size() && i < size_t(kMaxLayers); ++i) l[i] = float((*a)[i].value_or(1.0));
  return l;
}
toml::table ItemTable(const Item& it) {
  toml::table t{{"file", it.file}, {"key", it.key}, {"layers", Layers(it.layers)}, {"seconds", it.seconds}};
  if (!it.sections.empty()) {
    toml::array secs;
    for (const Section& s : it.sections) secs.push_back(toml::table{{"from", s.from}, {"to", s.to}, {"layers", Layers(s.layers)}});
    t.insert("sections", secs);
  }
  return t;
}
Item ItemFrom(const toml::table& t) {
  Item it;
  it.file = t["file"].value_or(false);
  it.key = t["key"].value_or(std::string());
  it.layers = Layers(t.get("layers"));
  it.seconds = t["seconds"].value_or(0.0);
  if (auto* secs = t["sections"].as_array())
    for (auto& n : *secs)
      if (auto* st = n.as_table()) {
        Section s;
        s.from = (*st)["from"].value_or(0.0);
        s.to = (*st)["to"].value_or(s.from + 60.0);
        s.layers = Layers(st->get("layers"));
        it.sections.push_back(s);
      }
  return it;
}

void LoadLibrary() {
  Library lib;
  try {
    if (std::filesystem::exists(LibraryPath())) {
      toml::table t = toml::parse_file(LibraryPath().string());
      lib.volume = float(t["volume"].value_or(0.8));
      lib.crossfade = float(t["crossfade"].value_or(2.0));
      lib.hold_game_music = t["hold_game_music"].value_or(true);
      lib.keep_playing = t["keep_playing"].value_or(true);
      if (auto* titles = t["titles"].as_table())
        for (auto& [k, v] : *titles) lib.titles[std::string(k.str())] = v.value_or(std::string());
      if (auto* a = t["albums"].as_array())
        for (auto& n : *a)
          if (auto* at = n.as_table()) {
            Album al;
            al.name = (*at)["name"].value_or(std::string());
            al.cover = (*at)["cover"].value_or(std::string());
            if (auto* keys = (*at)["keys"].as_array())
              for (auto& k : *keys) al.keys.push_back(k.value_or(std::string()));
            lib.albums.push_back(std::move(al));
          }
      if (auto* a = t["playlists"].as_array())
        for (auto& n : *a)
          if (auto* pt = n.as_table()) {
            Playlist pl;
            pl.name = (*pt)["name"].value_or(std::string());
            pl.shuffle = (*pt)["shuffle"].value_or(false);
            pl.repeat = int((*pt)["repeat"].value_or(int64_t(1)));
            if (auto* items = (*pt)["items"].as_array())
              for (auto& i : *items)
                if (auto* it = i.as_table()) pl.items.push_back(ItemFrom(*it));
            lib.playlists.push_back(std::move(pl));
          }
      if (auto* a = t["presets"].as_array())
        for (auto& n : *a)
          if (auto* pt = n.as_table()) {
            Preset p;
            p.name = (*pt)["name"].value_or(std::string());
            p.key = (*pt)["key"].value_or(std::string());
            p.layers = Layers(pt->get("layers"));
            lib.presets.push_back(std::move(p));
          }
      if (auto* r = t["replace"].as_table())
        for (auto& [k, v] : *r)
          if (auto* it = v.as_table()) lib.replace[uint32_t(std::strtoul(std::string(k.str()).c_str(), nullptr, 10))] = ItemFrom(*it);
    }
  } catch (const std::exception& e) {
    REXLOG_WARN("Jukebox: {} unreadable ({})", LibraryPath().string(), e.what());
  }
  std::lock_guard lock(G().library_mutex);
  G().library = std::move(lib);
}

// ---- The Music folder ----

void ScanMusic() {
  std::vector<UserSong> songs;
  std::error_code ec;
  const std::filesystem::path root = MusicFolder();
  std::filesystem::create_directories(root, ec);
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string ext = Lower(it->path().extension().string());
    if (ext != ".wav"
#if defined(NIER_JUKEBOX_MP3)
        && ext != ".mp3"
#endif
    )
      continue;
    UserSong s;
    s.path = U8(std::filesystem::relative(it->path(), root, ec));
    const auto parent = std::filesystem::relative(it->path().parent_path(), root, ec);
    s.folder = parent.empty() || parent == "." ? std::string() : U8(parent);
    s.title = U8(it->path().stem());
    songs.push_back(std::move(s));
  }
  std::sort(songs.begin(), songs.end(), [](const UserSong& a, const UserSong& b) { return a.path < b.path; });
  REXLOG_INFO("Jukebox: {} songs of the player's in {}", songs.size(), root.string());
  std::lock_guard lock(G().user_mutex);
  G().user_songs = std::move(songs);
}

}  // namespace

// ---- The API ----

void Init(const std::filesystem::path& settings_dir) {
  static bool started = false;
  if (started) return;
  started = true;
  G().dir = settings_dir;
  LoadLibrary();
  std::thread([] {
    LoadCatalog();
    ScanMusic();
  }).detach();
  P().Start();
}

const Catalog& GetCatalog() {
  static Catalog empty;
  return G().catalog_ready ? G().catalog : empty;
}

std::vector<UserSong> UserSongs() {
  std::lock_guard lock(G().user_mutex);
  return G().user_songs;
}

std::filesystem::path MusicFolder() { return G().dir / "Music"; }

void RescanMusicFolder() { std::thread(ScanMusic).detach(); }

std::mutex& LibraryMutex() { return G().library_mutex; }
Library& GetLibrary() { return G().library; }

void Save() {
  toml::table t;
  {
    std::lock_guard lock(G().library_mutex);
    const Library& lib = G().library;
    t.insert("volume", double(lib.volume));
    t.insert("crossfade", double(lib.crossfade));
    t.insert("hold_game_music", lib.hold_game_music);
    t.insert("keep_playing", lib.keep_playing);
    toml::table titles;
    for (auto& [k, v] : lib.titles) titles.insert(k, v);
    t.insert("titles", titles);
    toml::array albums;
    for (const Album& a : lib.albums) {
      toml::array keys;
      for (auto& k : a.keys) keys.push_back(k);
      albums.push_back(toml::table{{"name", a.name}, {"cover", a.cover}, {"keys", keys}});
    }
    t.insert("albums", albums);
    toml::array playlists;
    for (const Playlist& p : lib.playlists) {
      toml::array items;
      for (const Item& it : p.items) items.push_back(ItemTable(it));
      playlists.push_back(toml::table{{"name", p.name}, {"shuffle", p.shuffle}, {"repeat", int64_t(p.repeat)}, {"items", items}});
    }
    t.insert("playlists", playlists);
    toml::array presets;
    for (const Preset& p : lib.presets) presets.push_back(toml::table{{"name", p.name}, {"key", p.key}, {"layers", Layers(p.layers)}});
    t.insert("presets", presets);
    toml::table replace;
    for (auto& [cue, it] : lib.replace) replace.insert(std::to_string(cue), ItemTable(it));
    t.insert("replace", replace);
  }
  std::error_code ec;
  std::filesystem::create_directories(G().dir, ec);
  std::ofstream f(LibraryPath(), std::ios::binary | std::ios::trunc);
  f << "# NieR Jukebox (the game's Jukebox page writes this file)\n" << t << "\n";
}

std::string PartName(const std::string& part) {
  static const std::map<std::string, std::pair<const char*, const char*>> kNames = {
      {"oche", {"Orquestra", "Orchestra"}},  {"ochel", {"Orquestra", "Orchestra"}}, {"rythm", {"Ritmo", "Rhythm"}},
      {"vocal", {"Voz", "Vocals"}},          {"vocal1", {"Voz 1", "Vocals 1"}},     {"vocal2", {"Voz 2", "Vocals 2"}},
      {"voa", {"Voz A", "Vocals A"}},        {"vob", {"Voz B", "Vocals B"}},        {"piano", {"Piano", "Piano"}},
      {"strings", {"Cordas", "Strings"}},    {"strings2", {"Cordas", "Strings"}},  {"strrings", {"Cordas", "Strings"}},
      {"guitar", {"Violão", "Guitar"}},      {"organ", {"Órgão", "Organ"}},         {"mbox", {"Caixinha de música", "Music box"}},
      {"musicbox", {"Caixinha de música", "Music box"}}, {"pad", {"Fundo", "Pad"}}, {"seq", {"Sequência", "Sequence"}},
      {"choir", {"Coral", "Choir"}},         {"amb", {"Ambiente", "Ambience"}},     {"fst", {"Rápida", "Fast"}},
      {"slw", {"Lenta", "Slow"}},            {"room", {"Interior", "Indoors"}},     {"pfhp", {"Piano e harpa", "Piano and harp"}},
      {"boss", {"Chefe", "Boss"}},           {"l", {"Esquerda", "Left"}},           {"r", {"Direita", "Right"}},
      {"ls", {"Esquerda (fundo)", "Left (rear)"}}, {"rs", {"Direita (fundo)", "Right (rear)"}},
  };
  const bool pt = text::CurrentName() == "Portugues";
  auto it = kNames.find(Lower(part));
  if (it != kNames.end()) return pt ? it->second.first : it->second.second;
  if (part.empty()) return pt ? "Música" : "Music";
  std::string s = part;
  std::replace(s.begin(), s.end(), '_', ' ');
  s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

std::string Title(const std::string& key, bool file) {
  {
    std::lock_guard lock(G().library_mutex);
    auto it = G().library.titles.find(key);
    if (it != G().library.titles.end() && !it->second.empty()) return it->second;
  }
  if (file) return U8(std::filesystem::path(std::u8string(key.begin(), key.end())).stem());
  std::string base;
  {
    std::lock_guard lock(G().catalog_mutex);
    if (const Song* s = G().catalog.Find(key)) base = s->base;
  }
  if (base.empty()) return key;
  // The soundtrack's names (the player's list, 2026-11-15), by the game's synth.
  static const std::map<std::string, const char*> kOfficial = {
      {"Synth/100020", "Snow in Summer"},
      {"Synth/100023", "Temple of Drifting Sands"},
      {"Synth/100022", "Gods Bound by Rules"},
      {"Synth/100025", "Hills of Radiant Winds"},
      {"Synth/100024", "City of Commerce"},
      {"Synth/100026", "Song of the Ancients (Combined)"},
      {"Synth/100027", "The Prestigious Mask"},
      {"Synth/100028", "The Incomplete Stone"},
      {"Synth/100029", "Grandma"},
      {"Synth/100030", "Cold Steel Coffin"},
      {"Synth/100031", "The Ultimate Weapon"},
      {"Synth/100032", "Deep Crimson Foe"},
      {"Synth/100033", "The Wretched Automatons"},
      {"Synth/100035", "Repose"},
      {"Synth/100036", "The Lost Forest"},
      {"Synth/100037", "Shadowlord's Castle"},
      {"Synth/100039", "Shadowlord (Extra Part)"},
      {"Synth/100075", "Shadowlord (Stage 1)"},
      {"Synth/100076", "Shadowlord (Stage 1, Part 2)"},
      {"Synth/100077", "Shadowlord (Stage 2)"},
      {"Synth/100078", "Shadowlord (Stage 3)"},
      {"Synth/100040", "Song of the Ancients (Fate)"},
      {"Synth/100041", "Song of the Ancients (Hollow Dreams)"},
      {"Synth/100042", "Kain\xC3\xA9 (Escape)"},
      {"Synth/100043", "Kain\xC3\xA9 (Salvation)"},
      {"Synth/100044", "Yonah (Piano Version)"},
      {"Synth/100069", "Yonah (Strings Version)"},
      {"Synth/100064", "Yonah (Pluck Version 1)"},
      {"Synth/100065", "Yonah (Pluck Version 2)"},
      {"Synth/100045", "Emil (Karma)"},
      {"Synth/100046", "Emil (Sacrifice)"},
      {"Synth/100047", "The Dark Colossus Destroys All"},
      {"Synth/100048", "Blu-Bird"},
      {"Synth/100063", "Blu-Bird (Stage 2)"},
      {"Synth/100066", "Dispossession (Piano Version)"},
      {"Synth/100049", "Dispossession (Strings Version)"},
      {"Synth/100067", "Dispossession (Pluck Version)"},
      {"Synth/100068", "Dispossession (Music Box Version)"},
      {"Synth/100061", "Dance of Evanescence"},
      {"Synth/120019", "Shadowlord's Castle (Feeling)"},
      {"Synth/100059", "His Dream"},
      {"Synth/100060", "This Dream"},
  };
  if (auto it = kOfficial.find(key); it != kOfficial.end()) return it->second;
  return base;
}

std::vector<std::string> PartsOf(const Item& item) {
  std::vector<std::string> parts;
  if (!item.file) {
    std::lock_guard lock(G().catalog_mutex);
    if (const Song* s = G().catalog.Find(item.key)) {
      parts.assign(kMaxLayers, std::string());
      for (const Stem& st : s->stems) {
        auto& p = parts[size_t(std::clamp(st.layer, 0, kMaxLayers - 1))];
        if (p.empty()) p = PartName(st.part);
      }
      while (!parts.empty() && parts.back().empty()) parts.pop_back();
      for (auto& p : parts)
        if (p.empty()) p = PartName("");
    }
  }
  if (parts.empty()) parts.push_back("Volume");
  return parts;
}

// The cue sheet's short pieces that are no song (ambience, stingers: the player's list, 2026-11-15): out of the list.
bool Hidden(const std::string& key) { return StartsWith(key, "Synth/1200") && key != "Synth/120019"; }

void Play(const Item& item) {
  P().Command([item](Player& p) { p.PlayLocked(item, -1, -1); });
}
void PlayOfficial(const std::string& key) {
  P().Command([key](Player& p) {
    Item item;
    item.key = key;
    p.PlayLocked(item, -1, -1);
    if (p.current_ && !p.current_->item.file && p.current_->item.key == key) p.current_->official = true;
  });
}
void PlayPlaylist(int playlist, int index) {
  P().Command([playlist, index](Player& p) { p.PlayPlaylistLocked(playlist, index); });
}
void Next() {
  P().Command([](Player& p) { p.StepLocked(1); });
}
void Previous() {
  P().Command([](Player& p) {
    if (p.current_ && p.current_->frames > uint64_t(kRate) * 4) {
      p.current_->source->Seek(p.file_, 0);
      p.current_->frames = 0;
    } else {
      p.StepLocked(-1);
    }
  });
}
void Pause(bool paused) {
  P().Command([paused](Player& p) { p.paused_ = paused; });
}
void Stop() {
  P().Command([](Player& p) { p.StopLocked(); });
}
void SetLayers(const std::array<float, kMaxLayers>& layers, float fade_seconds) {
  P().Command([layers, fade_seconds](Player& p) {
    if (!p.current_) return;
    p.current_->official = p.current_->official_live = false;  // the player's own mix from now on
    for (int i = 0; i < kMaxLayers; ++i) p.current_->layers[size_t(i)].Set(layers[size_t(i)], fade_seconds);
    // In a timeline section the change is that moment's only (the next section brings its own mix).
    if (p.current_->section < 0) {
      p.current_->item.layers = layers;
      p.current_->base = layers;
    }
  });
}
void SetTimeline(int playlist, int index, const Item& item) {
  P().Command([playlist, index, item](Player& p) {
    Voice* v = p.current_.get();
    if (!v || p.playlist_ != playlist || p.index_ != index || v->item.key != item.key) return;
    v->item.sections = item.sections;
    v->item.seconds = item.seconds;
    if (!item.sections.empty()) {
      v->section = -2;
    } else if (v->section != -1) {
      v->section = -1;
      for (int i = 0; i < kMaxLayers; ++i) v->layers[size_t(i)].Set(v->base[size_t(i)], 0.15f);
    }
  });
}
void Seek(double seconds) {
  P().Command([seconds](Player& p) {
    if (!p.current_) return;
    p.current_->source->Seek(p.file_, seconds);
    p.current_->frames = uint64_t(std::max(0.0, seconds) * kRate);
    p.current_->ended = false;
  });
}
NowPlaying Now() { return P().Now(); }
GameMusic Game() { return P().Game(); }
bool Enabled() { return G().enabled.load(); }
void SetEnabled(bool on) {
  if (G().enabled.exchange(on) == on) return;
  REXLOG_INFO("Jukebox: {}", on ? "on" : "off");
  // Off: ours stops, and GameTick brings the game's music back (its hold ends).
  if (!on) P().Command([](Player& p) { p.StopLocked(); });
}

}  // namespace nier::jukebox

// The game starts a BGM (its player exists from now on: GameTick may read it).
REX_HOOK_RAW(sub_82115620) {
  nier::jukebox::G().bgm_seen = true;
  nier::jukebox::G().last_cue = int32_t(ctx.r4.u32);
  if (ctx.r3.u32 >= 0x40000000u && ctx.r3.u32 < 0xA0000000u) nier::jukebox::G().bgm_player = ctx.r3.u32;
  __imp__sub_82115620(ctx, base);
}
