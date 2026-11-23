// nier - the Jukebox: NieR's own music played from the player's copy (AUDIO/NIER_BGM in layer1.cpk), each song's
// layers (orchestra, rhythm, voice...) mixed at will, plus the player's own songs (a Music folder: MP3, WAV).
//
// What the game does (2026-11-15, NOTAS.md "Jukebox"): its script commands _PlayBgm / _SetBgmLayerVolume /
// _SetBgmMasterVolume reach a BGM player at 0x8447C7AC (the sound manager 0x8447BB90 + 3100): +8 the BGM playing
// (a cue id), +28 the list of loaded tracks (next at +156, cue id at +16). A track: +24 layer count, +32 its volume
// (current, from, target, fade time, elapsed, active byte at +52), then 24 bytes per layer from +56 the same way.
// The jukebox plays through its own SDL stream; while it plays the game's BGM is held at volume 0 (and brought back
// with a fade after).
//
// Saved with the settings (jukebox.toml): song titles the player gave, albums, playlists (with times: "song X for 5
// minutes, then Y for 3"), mix presets, which song replaces the game's in each place.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace nier::jukebox {

constexpr int kMaxLayers = 4;

// A song's layer: one AAX in the game data.
struct Stem {
  std::string wave;     // "016_village_center_vocal_loop_wav.aax"
  std::string part;     // "vocal"
  int layer = 0;        // the game's AISAC layer (0..3)
  float volume = 1.0f;  // the cue sheet's volume for it
  uint64_t offset = 0;  // the AAX in layer1.cpk
  uint64_t size = 0;
  int pan = 0;          // mono files of a 4-channel song: -1 left, 1 right, 0 both
};

struct Song {
  std::string key;  // the synth, "Synth/100026"
  std::string base;  // the files' common name, "016_village_center"
  std::vector<uint32_t> cues;  // the game's cue ids that play it
  float volume = 1.0f;
  std::vector<Stem> stems;
  double seconds = 0;  // one pass (intro + loop)
  bool loops = false;
  int sample_rate = 48000;
};

// A stretch of a playlist item's time with its own mix (the timeline: "from 0:00 to 1:00 the orchestra at 80%
// and the rhythm at 30%, from 1:00 to 2:00 the voice alone"). Outside every section the item's own layers play.
struct Section {
  double from = 0, to = 60;  // seconds into the item
  std::array<float, kMaxLayers> layers{1, 1, 1, 1};
};

// A thing to play: a game song (key) or a file of the Music folder (path relative to it).
struct Item {
  bool file = false;
  std::string key;
  std::array<float, kMaxLayers> layers{1, 1, 1, 1};  // per layer, 0..1
  double seconds = 0;  // how long (0: the song's own length, once through; looping ones a few minutes)
  std::vector<Section> sections;  // the timeline (the first that holds the time wins)
};

struct Playlist {
  std::string name;
  std::vector<Item> items;
  bool shuffle = false;
  int repeat = 1;  // 0 once, 1 the whole list, 2 one item
};

struct Album {
  std::string name;
  std::string cover;  // a song key whose disc art it uses, or an image file in the Music folder
  std::vector<std::string> keys;
};

struct Preset {
  std::string name;
  std::string key;  // the song (empty: any; the layers by part name)
  std::array<float, kMaxLayers> layers{1, 1, 1, 1};
};

struct Library {
  std::map<std::string, std::string> titles;  // song key / file -> the name the player gave
  std::vector<Album> albums;
  std::vector<Playlist> playlists;
  std::vector<Preset> presets;
  std::map<uint32_t, Item> replace;  // the game's cue -> what plays instead
  float volume = 0.8f;
  float crossfade = 2.0f;  // seconds
  bool hold_game_music = true;  // the game's BGM silent while the jukebox plays
  bool keep_playing = true;     // goes on after the page is closed
};

struct Catalog {
  bool ready = false;
  std::string error;
  std::vector<Song> songs;
  const Song* Find(const std::string& key) const {
    for (const Song& s : songs)
      if (s.key == key) return &s;
    return nullptr;
  }
  const Song* ByCue(uint32_t cue) const {
    for (const Song& s : songs)
      for (uint32_t c : s.cues)
        if (c == cue) return &s;
    return nullptr;
  }
};

// A file of the Music folder.
struct UserSong {
  std::string path;  // relative to the folder, "/" separated
  std::string folder;  // its subfolder (shown as an album)
  std::string title;
};

// What plays now (a copy for the page).
struct NowPlaying {
  bool playing = false, paused = false;
  Item item;
  std::string title;
  double position = 0, length = 0;  // seconds; length 0 = unknown / loops
  int playlist = -1, playlist_index = -1;
  std::vector<std::string> parts;  // the layers' part names
  std::array<float, kMaxLayers> levels{};  // the layers' loudness now (for the meters)
  int section = -1;  // the item's timeline section playing now (-1: none)
  double elapsed = 0;  // seconds since the item started (the timeline's clock; a looping song goes past its length)
  // The official mode (PlayOfficial): the layers follow the game's own for this song, live. `official_live` while
  // the game plays this song now (its place); else the last mix stays.
  bool official = false, official_live = false;
};

// The game's own music now.
struct GameMusic {
  int32_t cue = -1;
  std::array<float, kMaxLayers> layers{};
  int layer_count = 0;
  float volume = 0;
  bool held = false;  // silenced by the jukebox
};

// Started once (NierApp): the catalog and the Music folder in the background, the player thread.
void Init(const std::filesystem::path& settings_dir);

const Catalog& GetCatalog();
std::vector<UserSong> UserSongs();
std::filesystem::path MusicFolder();
void RescanMusicFolder();

// The library (lock it while reading/changing; Save() after a change).
std::mutex& LibraryMutex();
Library& GetLibrary();
void Save();

std::string Title(const std::string& key, bool file);  // the player's name, else a readable one
std::string PartName(const std::string& part);         // "oche" -> "Orquestra"
bool Hidden(const std::string& key);                   // a cue sheet piece that is no song (not listed)
std::vector<std::string> PartsOf(const Item& item);    // its layers' names (one, "Volume", for a file)

// Playback.
void Play(const Item& item);                 // now, with a crossfade
void PlayPlaylist(int playlist, int index);  // a saved playlist from an item
// A game song in the official mode: its layers move as the game moves them in that song's place (the voice coming
// in, the instruments dropping out), read from the game's BGM player each tick. A layer slider ends it.
void PlayOfficial(const std::string& key);
void Next();
void Previous();
void Pause(bool paused);
void Stop();
void SetLayers(const std::array<float, kMaxLayers>& layers, float fade_seconds = 0.4f);
void Seek(double seconds);
// A playlist item's timeline / time was edited: the playing one (that playlist's item) follows at once.
void SetTimeline(int playlist, int index, const Item& item);
NowPlaying Now();
GameMusic Game();
// The page's switch: off at every start; off, the jukebox plays nothing and leaves the game's music alone.
bool Enabled();
void SetEnabled(bool on);

}  // namespace nier::jukebox
