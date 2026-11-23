// nier - "Achievements" as a sixth category of Grimoire Weiss's Notes page.
//
// Notes lists five categories (Status, Word List, Documents, Tutorials, Fishing Records) laid out in
// the menu_memo layout (MENU.2DV > spdpack > menu_memo.spd, "SPD0"). Two parts:
//
// 1. The layout. When the game binds menu_memo.spd (sub_823B7628, before relocating its offsets in
//    place), it gets an enlarged copy built here from the player's own file: a sixth row (icon
//    "=_com[icn_memo]_00_other", text "=_txt_06_memo", line "=_com[menu_com]_05_line") one row below
//    Fishing Records, the row in every animation that dims or shows the Fishing Records row, and
//    ACH_SELECT_IN / ACH_SELECT_OUT (copies of FISH_SELECT_*, which highlight the new row).
//
// 2. The list. The game's list code (sub_8240DDA0: A, up, down) knows five rows; it is replaced by
//    the same steps over six. The sixth row is the fifth one (index 4) with the fifth entries of the
//    game's row tables (text node, icon, select-in / select-out animation, help id) pointed at ours
//    while it is selected, so every other part of the game (cursor, help, the page-change
//    animations) handles it unchanged. A on it opens the achievements page (achievements_ui.h);
//    B there comes back to the list.
//
// SPD0 (big-endian): +4 name, +0x0C/+0x10 sprites (32 B: id, name, parent, -1, x, y, w, h),
// +0x14/+0x18 texts (52 B: id, name, parent, label, x, y, w, h, ...), +0x1C/+0x20 animations (24 B:
// name, length, loop, -1, tracks, count; track 56 B: node name, ..., kind byte at +0x34),
// +0x24/+0x28 initial states (16 B: name, entries, count; entry 72 B: node name, ...), +0x2C tracks,
// +0x30 entries, +0x34 strings. sub_823B7408 turns the offsets into pointers once (+0x0C == 0x38
// before it).

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "game_text.h"
#include "pc_options.h"
#include "pc_text.h"

extern "C" void __imp__sub_823B7628(PPCContext& ctx, uint8_t* base);  // bind a layout (.spd)
extern "C" void __imp__sub_823BEC18(PPCContext& ctx, uint8_t* base);  // bind its layer list (.pli)
extern "C" void __imp__sub_8240DDA0(PPCContext& ctx, uint8_t* base);  // Notes list: A / up / down
extern "C" void __imp__sub_8240FC78(PPCContext& ctx, uint8_t* base);  // Notes list: entered
extern "C" void __imp__sub_8240FB80(PPCContext& ctx, uint8_t* base);  // Notes list: back from a category
extern "C" void sub_82103CF0(PPCContext& ctx, uint8_t* base);         // pad: confirm held
extern "C" void sub_82103E00(PPCContext& ctx, uint8_t* base);         // pad: pressed (with repeat)
extern "C" void sub_8240DC40(PPCContext& ctx, uint8_t* base);         // Notes: cursor + help of the row
extern "C" void sub_824053C0(PPCContext& ctx, uint8_t* base);         // menu sound
extern "C" void sub_823BC8F0(PPCContext& ctx, uint8_t* base);         // layout: node by name

namespace {

// ---- guest memory -------------------------------------------------------------------------------

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  uint8_t* p = base + addr;
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
float LoadFloat(const uint8_t* base, uint32_t addr) {
  const uint32_t bits = Load32(base, addr);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint32_t GuestAlloc(uint32_t size) { return REX_KERNEL_MEMORY()->SystemHeapAlloc(size); }

uint32_t GuestText(const std::string& text) {
  static std::mutex mutex;
  static std::map<std::string, uint32_t> copies;
  std::lock_guard lock(mutex);
  uint32_t& addr = copies[text];
  if (!addr) {
    addr = GuestAlloc(uint32_t(text.size()) + 1);
    if (addr) std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(addr), text.c_str(), text.size() + 1);
  }
  return addr;
}

// A recompiled function by its guest address (as the generated indirect calls find it).
void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t addr) {
  constexpr uint64_t kImageBase = 0x82000000ull, kImageSize = 0x4090000ull, kCodeBase = 0x82100000ull;
  if (addr < kCodeBase || addr >= kImageBase + kImageSize) return;
  PPCFunc* f = *reinterpret_cast<PPCFunc**>(base + kImageBase + kImageSize + (uint64_t(addr) - kCodeBase) * 2);
  if (f) f(ctx, base);
}

// ---- the layout ---------------------------------------------------------------------------------

constexpr const char* kFishIcon = "=_com[icn_item]_00_fish";
constexpr const char* kFishText = "=_txt_05_memo";
constexpr const char* kFishLine = "=_com[menu_com]_04_line";
constexpr const char* kOurIcon = "=_com[icn_memo]_00_other";
constexpr const char* kOurText = "=_txt_06_memo";
constexpr const char* kOurLine = "=_com[menu_com]_05_line";
constexpr const char* kOurIn = "ACH_SELECT_IN";
constexpr const char* kOurOut = "ACH_SELECT_OUT";
constexpr float kRowStep = 39.0f;      // the rows are 39 units apart
constexpr uint32_t kOurLabelMessage = 9004000, kOurHelpMessage = 9004060;  // pc_text ids

struct Rec {
  std::vector<uint8_t> raw;
  std::map<int, std::string> strings;  // offset in the record -> text it points to
  std::vector<Rec> children;           // tracks of an animation, entries of an initial state
  const std::string& Name() const { return strings.at(0x00); }
};

struct Spd {
  std::vector<uint8_t> header;  // 0x38 bytes
  std::string name;
  std::vector<Rec> sprites, texts, anims, inits;
};

uint32_t Be(const std::vector<uint8_t>& d, size_t o) {
  return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}
void SetBe(std::vector<uint8_t>& d, size_t o, uint32_t v) {
  d[o] = uint8_t(v >> 24); d[o + 1] = uint8_t(v >> 16); d[o + 2] = uint8_t(v >> 8); d[o + 3] = uint8_t(v);
}
float BeFloat(const std::vector<uint8_t>& d, size_t o) {
  const uint32_t bits = Be(d, o);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void SetBeFloat(std::vector<uint8_t>& d, size_t o, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  SetBe(d, o, bits);
}

bool ReadString(const std::vector<uint8_t>& d, uint32_t at, std::string& out) {
  if (at >= d.size()) return false;
  const auto end = std::find(d.begin() + at, d.end(), uint8_t(0));
  if (end == d.end()) return false;
  out.assign(d.begin() + at, end);
  return true;
}

bool ReadRec(const std::vector<uint8_t>& d, uint32_t at, uint32_t size, std::initializer_list<int> string_fields,
             Rec& rec) {
  if (uint64_t(at) + size > d.size()) return false;
  rec.raw.assign(d.begin() + at, d.begin() + at + size);
  for (int f : string_fields) {
    if (!ReadString(d, Be(rec.raw, f), rec.strings[f])) return false;
  }
  return true;
}

bool Parse(const std::vector<uint8_t>& d, Spd& spd) {
  if (d.size() < 0x38 || Be(d, 0) != 0x30445053 || Be(d, 0x0C) != 0x38) return false;
  spd.header.assign(d.begin(), d.begin() + 0x38);
  if (!ReadString(d, Be(d, 4), spd.name)) return false;
  auto table = [&](uint32_t off_field, uint32_t size, std::initializer_list<int> strings, std::vector<Rec>& out) {
    const uint32_t at = Be(d, off_field), count = Be(d, off_field + 4);
    if (count > 4096) return false;
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      if (!ReadRec(d, at + i * size, size, strings, out[i])) return false;
    }
    return true;
  };
  if (!table(0x0C, 32, {4, 8}, spd.sprites) || !table(0x14, 52, {4, 8, 12}, spd.texts) ||
      !table(0x1C, 24, {0}, spd.anims) || !table(0x24, 16, {0}, spd.inits)) {
    return false;
  }
  for (Rec& a : spd.anims) {
    const uint32_t at = Be(a.raw, 16), count = Be(a.raw, 20);
    if (count > 4096) return false;
    a.children.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      if (!ReadRec(d, at + i * 56, 56, {0}, a.children[i])) return false;
    }
  }
  for (Rec& s : spd.inits) {
    const uint32_t at = Be(s.raw, 4), count = Be(s.raw, 8);
    if (count > 4096) return false;
    s.children.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      if (!ReadRec(d, at + i * 72, 72, {0}, s.children[i])) return false;
    }
  }
  return true;
}

std::vector<uint8_t> Write(const Spd& spd) {
  size_t tracks = 0, entries = 0;
  for (const Rec& a : spd.anims) tracks += a.children.size();
  for (const Rec& s : spd.inits) entries += s.children.size();
  const uint32_t o_sprites = 0x38;
  const uint32_t o_texts = o_sprites + uint32_t(spd.sprites.size()) * 32;
  const uint32_t o_anims = o_texts + uint32_t(spd.texts.size()) * 52;
  const uint32_t o_inits = o_anims + uint32_t(spd.anims.size()) * 24;
  const uint32_t o_tracks = o_inits + uint32_t(spd.inits.size()) * 16;
  const uint32_t o_entries = o_tracks + uint32_t(tracks) * 56;
  const uint32_t o_strings = o_entries + uint32_t(entries) * 72;

  std::vector<uint8_t> out(o_strings, 0);
  std::map<std::string, uint32_t> pool;
  std::vector<uint8_t> strings(1, 0);  // starts with the empty string, as the game's files
  pool[""] = o_strings;
  auto str = [&](const std::string& s) {
    auto it = pool.find(s);
    if (it != pool.end()) return it->second;
    const uint32_t at = o_strings + uint32_t(strings.size());
    strings.insert(strings.end(), s.begin(), s.end());
    strings.push_back(0);
    pool[s] = at;
    return at;
  };
  auto put = [&](uint32_t at, const Rec& r) {
    std::copy(r.raw.begin(), r.raw.end(), out.begin() + at);
    for (const auto& [field, s] : r.strings) SetBe(out, at + field, str(s));
  };
  // Strings in the original order of first use: names, then labels, then animations.
  str(spd.name);
  std::copy(spd.header.begin(), spd.header.end(), out.begin());
  for (size_t i = 0; i < spd.sprites.size(); ++i) put(o_sprites + uint32_t(i) * 32, spd.sprites[i]);
  for (size_t i = 0; i < spd.texts.size(); ++i) put(o_texts + uint32_t(i) * 52, spd.texts[i]);
  uint32_t t = o_tracks;
  for (size_t i = 0; i < spd.anims.size(); ++i) {
    const uint32_t at = o_anims + uint32_t(i) * 24;
    put(at, spd.anims[i]);
    SetBe(out, at + 16, t);
    SetBe(out, at + 20, uint32_t(spd.anims[i].children.size()));
    for (const Rec& tr : spd.anims[i].children) {
      put(t, tr);
      t += 56;
    }
  }
  uint32_t e = o_entries;
  for (size_t i = 0; i < spd.inits.size(); ++i) {
    const uint32_t at = o_inits + uint32_t(i) * 16;
    put(at, spd.inits[i]);
    SetBe(out, at + 4, e);
    SetBe(out, at + 8, uint32_t(spd.inits[i].children.size()));
    for (const Rec& en : spd.inits[i].children) {
      put(e, en);
      e += 72;
    }
  }
  SetBe(out, 0x04, str(spd.name));
  SetBe(out, 0x0C, o_sprites);
  SetBe(out, 0x10, uint32_t(spd.sprites.size()));
  SetBe(out, 0x14, o_texts);
  SetBe(out, 0x18, uint32_t(spd.texts.size()));
  SetBe(out, 0x1C, o_anims);
  SetBe(out, 0x20, uint32_t(spd.anims.size()));
  SetBe(out, 0x24, o_inits);
  SetBe(out, 0x28, uint32_t(spd.inits.size()));
  SetBe(out, 0x2C, o_tracks);
  SetBe(out, 0x30, o_entries);
  SetBe(out, 0x34, o_strings);
  out.insert(out.end(), strings.begin(), strings.end());
  while (out.size() % 16) out.push_back(0);
  return out;
}

Rec* Find(std::vector<Rec>& list, const std::string& name, int field = 0) {
  for (Rec& r : list) {
    auto it = r.strings.find(field);
    if (it != r.strings.end() && it->second == name) return &r;
  }
  return nullptr;
}

// A copy of `from` (a node record) one row lower, with a new id and name.
Rec RowBelow(const Rec& from, const char* name, uint32_t id) {
  Rec r = from;
  SetBe(r.raw, 0, id);
  r.strings[4] = name;
  SetBeFloat(r.raw, 0x14, BeFloat(r.raw, 0x14) + kRowStep);
  return r;
}

bool AddSixthRow(Spd& spd) {
  Rec* icon = Find(spd.sprites, kFishIcon, 4);
  Rec* line = Find(spd.sprites, kFishLine, 4);
  Rec* text = Find(spd.texts, kFishText, 4);
  Rec* fish_in = Find(spd.anims, "FISH_SELECT_IN");
  Rec* fish_out = Find(spd.anims, "FISH_SELECT_OUT");
  Rec* status_in = Find(spd.anims, "STATUS_SELECT_IN");
  if (!icon || !line || !text || !fish_in || !fish_out || !status_in) return false;
  if (Find(spd.texts, kOurText, 4)) return false;  // already there
  const Rec* dim = nullptr;  // "this row is not selected": the icon at half alpha
  for (const Rec& tr : status_in->children) {
    if (tr.Name() == kFishIcon) dim = &tr;
  }
  if (!dim) return false;
  const Rec dim_track = *dim;

  uint32_t next_id = 0;
  for (const Rec& r : spd.sprites) next_id = std::max(next_id, Be(r.raw, 0) + 1);
  for (const Rec& r : spd.texts) next_id = std::max(next_id, Be(r.raw, 0) + 1);
  Rec new_icon = RowBelow(*icon, kOurIcon, next_id);
  Rec new_line = RowBelow(*line, kOurLine, next_id + 1);
  Rec new_text = RowBelow(*text, kOurText, next_id + 2);
  new_text.strings[12] = std::to_string(kOurLabelMessage / 100);
  spd.sprites.push_back(new_icon);
  spd.sprites.push_back(new_line);
  spd.texts.push_back(new_text);

  auto retarget = [](Rec r, const char* node) {
    r.strings[0] = node;
    return r;
  };
  // Every animation: our row does what the Fishing Records row does; selecting Fishing Records
  // dims ours. Deleting fishing records leaves it alone.
  Rec ach_in = *fish_in, ach_out = *fish_out;
  for (Rec& a : spd.anims) {
    const std::string& name = a.Name();
    if (name == "DELETE_FISH" || name == "ALL_DELETE_FISH") continue;
    if (name == "FISH_SELECT_IN" || name == "FISH_SELECT_OUT") {
      a.children.push_back(retarget(dim_track, kOurIcon));
      continue;
    }
    std::vector<Rec> extra;
    for (const Rec& tr : a.children) {
      if (tr.Name() == kFishIcon) extra.push_back(retarget(tr, kOurIcon));
      else if (tr.Name() == kFishLine) extra.push_back(retarget(tr, kOurLine));
      else if (tr.Name() == kFishText) extra.push_back(retarget(tr, kOurText));
    }
    a.children.insert(a.children.end(), extra.begin(), extra.end());
  }
  for (Rec* a : {&ach_in, &ach_out}) {
    for (Rec& tr : a->children) {
      if (tr.Name() == kFishIcon) tr.strings[0] = kOurIcon;
    }
    a->children.push_back(retarget(dim_track, kFishIcon));
  }
  ach_in.strings[0] = kOurIn;
  ach_out.strings[0] = kOurOut;
  spd.anims.push_back(ach_in);
  spd.anims.push_back(ach_out);

  for (Rec& s : spd.inits) {
    std::vector<Rec> extra;
    for (const Rec& en : s.children) {
      if (en.Name() == kFishIcon) extra.push_back(retarget(en, kOurIcon));
      else if (en.Name() == kFishLine) extra.push_back(retarget(en, kOurLine));
      else if (en.Name() == kFishText) extra.push_back(retarget(en, kOurText));
    }
    s.children.insert(s.children.end(), extra.begin(), extra.end());
  }
  return true;
}

struct Layout {
  std::mutex mutex;
  std::vector<uint8_t> built;  // the enlarged menu_memo.spd, unrelocated
  bool failed = false;
  uint32_t copies[2] = {};     // guest buffers, used in turn (the previous one may still be bound)
  int next = 0;
};
Layout g_layout, g_layers;

// menu_memo.pli, the layer list bound with the layout (sub_823BEC18): "PLI ", +0x0C count, +0x10
// records (64 B: "LYER", x0, y0, x1, y1 in the 1280-wide sheet, kind, order, w << 16 | h, name[32]).
// A node is drawn only with its layer; ours are copies of the Fishing Records row's, one row lower.
bool AddSixthRowLayers(const std::vector<uint8_t>& d, std::vector<uint8_t>& out) {
  if (d.size() < 0x20 || Be(d, 0) != 0x504C4920) return false;
  const uint32_t count = Be(d, 0x0C), at = Be(d, 0x10);
  if (count > 4096 || uint64_t(at) + uint64_t(count) * 64 > d.size()) return false;
  auto name_of = [&](uint32_t i) {
    const char* p = reinterpret_cast<const char*>(d.data() + at + i * 64 + 32);
    return std::string(p, strnlen(p, 32));
  };
  int icon = -1, line = -1, text = -1;
  uint32_t order = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const std::string n = name_of(i);
    if (n == kFishIcon) icon = int(i);
    if (n == kFishLine) line = int(i);
    if (n == kFishText) text = int(i);
    if (n == kOurText) return false;
    order = std::max(order, Be(d, at + i * 64 + 0x18));
  }
  if (icon < 0 || line < 0 || text < 0) return false;
  out.assign(d.begin(), d.begin() + at + count * 64);
  const int sources[] = {icon, line, text};
  const char* names[] = {kOurIcon, kOurLine, kOurText};
  for (int k = 0; k < 3; ++k) {
    std::vector<uint8_t> r(d.begin() + at + sources[k] * 64, d.begin() + at + sources[k] * 64 + 64);
    SetBe(r, 0x08, Be(r, 0x08) + uint32_t(kRowStep));
    SetBe(r, 0x10, Be(r, 0x10) + uint32_t(kRowStep));
    if (Be(r, 0x18) != 0) SetBe(r, 0x18, ++order);
    std::fill(r.begin() + 32, r.end(), uint8_t(0));
    std::memcpy(r.data() + 32, names[k], std::strlen(names[k]));
    out.insert(out.end(), r.begin(), r.end());
  }
  SetBe(out, 0x0C, count + 3);
  return true;
}

uint32_t PlaceCopy(Layout& lay, uint8_t* base) {
  uint32_t& copy = lay.copies[lay.next];
  if (!copy) copy = GuestAlloc(uint32_t(lay.built.size()));
  if (!copy) return 0;
  lay.next ^= 1;
  std::memcpy(base + copy, lay.built.data(), lay.built.size());
  return copy;
}

// ---- "NOW LOADING" ------------------------------------------------------------------------------
// The loading screen's letters are the layout "load" of SYSTEM.2DV: ten text nodes of one letter
// each (labels "N", "O", "W", "L", "O", "A", "D", "I", "N", "G"), 25 units apart, a wider gap for
// the space, the last at x 1124, each with its own fade-in. The disc's languages all show NOW LOADING
// (as the console does); with a translation, its message 11800000 is spelled there instead, more
// nodes cloned from the last one when it has more letters.

constexpr const char* kLoadNodes[] = {"=_txt_n", "=_txt_o", "=_txt_w", "=_txt_l", "=_txt_o2",
                                      "=_txt_a", "=_txt_d", "=_txt_i", "=_txt_n2", "=_txt_g"};
constexpr float kLetterStep = 25.0f, kSpaceStep = 15.0f;

Layout g_load, g_load_layers;
std::vector<std::pair<std::string, float>> g_load_added;  // cloned nodes and their x offset from the last

std::string LoadingText() {
  const auto dir = nier::text::CurrentDir();
  if (dir.empty()) return {};
  std::ifstream in(dir / "NIER_TEXT.TXD", std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("11800000\t", 0) != 0) continue;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> cols;
    size_t at = 0;
    for (size_t tab; (tab = line.find('\t', at)) != std::string::npos; at = tab + 1) cols.push_back(line.substr(at, tab - at));
    cols.push_back(line.substr(at));
    return cols.size() > 2 ? cols[2] : std::string();
  }
  return {};
}

bool SpellLoading(Spd& spd, const std::string& text) {
  // UTF-8 letters; a space widens the gap before the next one.
  std::vector<std::string> letters;
  std::vector<bool> gap_before;
  bool gap = false;
  for (size_t i = 0; i < text.size();) {
    size_t n = 1;
    const uint8_t c = uint8_t(text[i]);
    if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
    const std::string ch = text.substr(i, n);
    i += n;
    if (ch == " ") { gap = !letters.empty(); continue; }
    letters.push_back(ch);
    gap_before.push_back(gap);
    gap = false;
  }
  if (letters.empty() || letters.size() > 40) return false;
  std::vector<Rec*> nodes;
  for (const char* name : kLoadNodes) {
    Rec* r = Find(spd.texts, name, 4);
    if (!r) return false;
    nodes.push_back(r);
  }
  const Rec last = *nodes.back();
  const float right = BeFloat(last.raw, 0x10);
  uint32_t next_id = 0;
  for (const Rec& r : spd.sprites) next_id = std::max(next_id, Be(r.raw, 0) + 1);
  for (const Rec& r : spd.texts) next_id = std::max(next_id, Be(r.raw, 0) + 1);
  // x of each letter, the last one where the game's last is.
  std::vector<float> xs(letters.size());
  float x = right;
  for (size_t i = letters.size(); i-- > 0;) {
    xs[i] = x;
    x -= kLetterStep + (gap_before[i] ? kSpaceStep : 0.0f);
  }
  g_load_added.clear();
  std::vector<std::string> names;
  for (size_t i = 0; i < letters.size(); ++i) {
    if (i < nodes.size()) {
      names.push_back(kLoadNodes[i]);
      continue;
    }
    std::string name = "=_txt_x" + std::to_string(i);
    Rec r = last;
    SetBe(r.raw, 0, next_id++);
    r.strings[4] = name;
    spd.texts.push_back(r);
    names.push_back(name);
    g_load_added.push_back({name, xs[i] - right});
  }
  // Clones do what the last letter does.
  for (Rec& a : spd.anims) {
    std::vector<Rec> extra;
    for (const Rec& tr : a.children) {
      if (tr.Name() != kLoadNodes[9]) continue;
      for (const auto& [name, dx] : g_load_added) {
        Rec c = tr;
        c.strings[0] = name;
        extra.push_back(c);
      }
    }
    a.children.insert(a.children.end(), extra.begin(), extra.end());
  }
  for (Rec& s : spd.inits) {
    std::vector<Rec> extra;
    for (const Rec& en : s.children) {
      if (en.Name() != kLoadNodes[9]) continue;
      for (const auto& [name, dx] : g_load_added) {
        Rec c = en;
        c.strings[0] = name;
        extra.push_back(c);
      }
    }
    s.children.insert(s.children.end(), extra.begin(), extra.end());
  }
  // Labels and places (unused game nodes keep no letter).
  for (size_t i = 0; i < nodes.size() || i < names.size(); ++i) {
    Rec* r = Find(spd.texts, i < names.size() ? names[i] : std::string(kLoadNodes[i]), 4);
    if (!r) continue;
    if (i < letters.size()) {
      r->strings[12] = letters[i];
      SetBeFloat(r->raw, 0x10, xs[i]);
    } else {
      r->strings[12] = "";
    }
  }
  return true;
}

bool AddLoadingLayers(const std::vector<uint8_t>& d, std::vector<uint8_t>& out) {
  if (g_load_added.empty() || d.size() < 0x20) return false;
  const uint32_t count = Be(d, 0x0C), at = Be(d, 0x10);
  if (count > 4096 || uint64_t(at) + uint64_t(count) * 64 > d.size()) return false;
  int last = -1;
  for (uint32_t i = 0; i < count; ++i) {
    if (std::strncmp(reinterpret_cast<const char*>(d.data() + at + i * 64 + 32), kLoadNodes[9], 32) == 0) last = int(i);
  }
  if (last < 0) return false;
  out.assign(d.begin(), d.begin() + at + count * 64);
  for (const auto& [name, dx] : g_load_added) {
    std::vector<uint8_t> r(d.begin() + at + last * 64, d.begin() + at + last * 64 + 64);
    SetBe(r, 0x04, uint32_t(int32_t(Be(r, 0x04)) + int32_t(dx)));
    SetBe(r, 0x0C, uint32_t(int32_t(Be(r, 0x0C)) + int32_t(dx)));
    std::fill(r.begin() + 32, r.end(), uint8_t(0));
    std::memcpy(r.data() + 32, name.data(), std::min<size_t>(name.size(), 31));
    out.insert(out.end(), r.begin(), r.end());
  }
  SetBe(out, 0x0C, count + uint32_t(g_load_added.size()));
  return true;
}

// ---- the list -----------------------------------------------------------------------------------

// The game's row tables (5 entries each) and help message ids.
constexpr uint32_t kRowText = 0x8275D9FC, kRowIcon = 0x8275DA10, kRowIn = 0x8275DA24, kRowOut = 0x8275DA38;
constexpr uint32_t kRowHelp = 0x82048394;
constexpr uint32_t kSlot = 4 * 4;  // the fifth entry
constexpr uint32_t kOffRow = 3556;
constexpr uint32_t kPadState = 0x827CD42C;
constexpr uint32_t kSoundA = 0x820007A4, kSoundB = 0x820454E0;

struct List {
  bool ready = false;  // our layout is bound (the sixth row exists)
  bool ours = false;   // the sixth row is selected (the fifth entries are ours)
  uint32_t fish[5] = {};
  uint32_t mine[5] = {};
};
List g_list;

void SetHelp(uint8_t* base, uint32_t id) {
  rex::memory::PageAccess old_access;
  if (!rex::memory::Protect(base + kRowHelp + kSlot, 4, rex::memory::PageAccess::kReadWrite, &old_access)) return;
  Store32(base, kRowHelp + kSlot, id);
  rex::memory::Protect(base + kRowHelp + kSlot, 4, old_access);
}

void UseSixthRow(uint8_t* base, bool ours) {
  List& l = g_list;
  if (!l.fish[0]) {
    l.fish[0] = Load32(base, kRowText + kSlot);
    l.fish[1] = Load32(base, kRowIcon + kSlot);
    l.fish[2] = Load32(base, kRowIn + kSlot);
    l.fish[3] = Load32(base, kRowOut + kSlot);
    l.fish[4] = Load32(base, kRowHelp + kSlot);
    l.mine[0] = GuestText(kOurText);
    l.mine[1] = GuestText(kOurIcon);
    l.mine[2] = GuestText(kOurIn);
    l.mine[3] = GuestText(kOurOut);
    l.mine[4] = kOurHelpMessage;
  }
  const uint32_t* v = ours ? l.mine : l.fish;
  Store32(base, kRowText + kSlot, v[0]);
  Store32(base, kRowIcon + kSlot, v[1]);
  Store32(base, kRowIn + kSlot, v[2]);
  Store32(base, kRowOut + kSlot, v[3]);
  if (Load32(base, kRowHelp + kSlot) != v[4]) SetHelp(base, v[4]);
  l.ours = ours;
}

// The list was (re)entered: the fifth entries follow the row the game has.
void SyncRow(uint8_t* base, uint32_t obj) {
  if (!g_list.ready) return;
  const bool ours = g_list.ours && Load32(base, obj + kOffRow) == 4;
  UseSixthRow(base, ours);
}

// Our row's text: the layout's label is a message id the game may not look up for added nodes.
void SetRowText(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  if (!g_list.ready) return;
  rex::CallFrame f(ctx);
  f.ctx.r3.u64 = obj + 48;
  f.ctx.r4.u64 = GuestText(kOurText);
  sub_823BC8F0(f, base);
  const uint32_t node = f.ctx.r3.u32;
  if (!node) return;
  const uint32_t set_text = Load32(base, Load32(base, node) + 72);
  f.ctx.r3.u64 = node;
  f.ctx.r4.u64 = GuestText(nier::pc_text::T("Achievements"));
  CallGuest(f, base, set_text);
}

void Sound(PPCContext& ctx, uint8_t* base, int which) {
  rex::CallFrame f(ctx);
  f.ctx.r3.u64 = uint64_t(which);
  f.ctx.f1.f64 = LoadFloat(base, kSoundA);
  f.ctx.f2.f64 = LoadFloat(base, kSoundB);
  sub_824053C0(f, base);
}

bool Pad(PPCContext& ctx, uint8_t* base, bool held, uint32_t mask) {
  rex::CallFrame f(ctx);
  f.ctx.r3.u64 = kPadState;
  f.ctx.r4.u64 = mask;
  f.ctx.r5.u64 = 0;
  f.ctx.r6.s64 = -1;
  if (held) sub_82103CF0(f, base);
  else sub_82103E00(f, base);
  return (f.ctx.r3.u32 & 0xFF) != 0;
}

// Plays the animation named by the pointer in `table` (fifth entries as they are now) at `row`.
void PlayRowAnimation(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t table, uint32_t row) {
  rex::CallFrame f(ctx);
  const uint32_t player = obj + 16;
  f.ctx.r3.u64 = player;
  f.ctx.r4.u64 = Load32(base, table + row * 4);
  f.ctx.r5.u64 = 0;
  CallGuest(f, base, Load32(base, Load32(base, player) + 32));
}

}  // namespace

// Bind a layout: menu_memo.spd gets our enlarged copy.
REX_HOOK_RAW(sub_823B7628) {
  const uint32_t spd = ctx.r4.u32;
  if (spd && Load32(base, spd) == 0x30445053 && Load32(base, spd + 0x0C) == 0x38) {
    const uint32_t name = spd + Load32(base, spd + 4);
    if (std::strcmp(reinterpret_cast<const char*>(base + name), "load") == 0) {
      Layout& lay = g_load;
      std::lock_guard lock(lay.mutex);
      if (lay.built.empty() && !lay.failed) {
        lay.failed = true;
        const std::string text = LoadingText();
        if (!text.empty() && text != "NOW LOADING") {
          const uint32_t size = std::min<uint32_t>(Load32(base, spd + 0x34), 0x100000) + 0x4000;
          std::vector<uint8_t> original(base + spd, base + spd + size);
          Spd parsed;
          if (Parse(original, parsed) && SpellLoading(parsed, text)) {
            lay.built = Write(parsed);
            lay.failed = false;
            REXLOG_INFO("Loading screen: \"{}\" ({} letters cloned)", text, g_load_added.size());
          }
        }
      }
      if (!lay.built.empty()) {
        if (const uint32_t copy = PlaceCopy(lay, base)) ctx.r4.u64 = copy;
      }
    } else if (std::strcmp(reinterpret_cast<const char*>(base + name), "menu_memo") == 0) {
      Layout& lay = g_layout;
      std::lock_guard lock(lay.mutex);
      if (lay.built.empty() && !lay.failed) {
        // The strings end the file and are a few KB; the parser stops at each string's end.
        const uint32_t size = std::min<uint32_t>(Load32(base, spd + 0x34), 0x100000) + 0x4000;
        std::vector<uint8_t> original(base + spd, base + spd + size);
        Spd parsed;
        if (Parse(original, parsed) && AddSixthRow(parsed)) {
          lay.built = Write(parsed);
          REXLOG_INFO("Notes: sixth row (Achievements) added to menu_memo ({} bytes)", lay.built.size());
          if (const char* dump = std::getenv("NIER_SPD_DUMP")) {
            std::ofstream(dump, std::ios::binary).write(reinterpret_cast<const char*>(lay.built.data()),
                                                        std::streamsize(lay.built.size()));
          }
        } else {
          lay.failed = true;
          REXLOG_WARN("Notes: menu_memo layout not as expected; no Achievements row");
        }
      }
      if (!lay.built.empty()) {
        if (const uint32_t copy = PlaceCopy(lay, base)) {
          ctx.r4.u64 = copy;
          g_list.ready = true;
          g_list.ours = false;
        }
      }
    }
  }
  __imp__sub_823B7628(ctx, base);
}

// Bind a layer list: menu_memo.pli gets our copy with the sixth row's layers.
REX_HOOK_RAW(sub_823BEC18) {
  const uint32_t pli = ctx.r4.u32;
  if (pli && Load32(base, pli) == 0x504C4920) {  // not bound yet
    const uint32_t count = Load32(base, pli + 0x0C), at = Load32(base, pli + 0x10);
    bool load = false;
    for (uint32_t i = 0; i < count && i < 4096 && at < 0x100 && !load; ++i) {
      load = std::strncmp(reinterpret_cast<const char*>(base + pli + at + i * 64 + 32), kLoadNodes[8], 32) == 0;
    }
    if (load && !g_load_added.empty()) {
      Layout& lay = g_load_layers;
      std::lock_guard lock(lay.mutex);
      std::vector<uint8_t> built;
      const std::vector<uint8_t> original(base + pli, base + pli + at + count * 64);
      if (lay.built.empty() && AddLoadingLayers(original, built)) lay.built = std::move(built);
      if (!lay.built.empty()) {
        if (const uint32_t copy = PlaceCopy(lay, base)) ctx.r4.u64 = copy;
      }
    } else if (g_layout.copies[0] && count && count < 4096 && at < 0x100) {  // after our Notes layout
      Layout& lay = g_layers;
      std::lock_guard lock(lay.mutex);
      std::vector<uint8_t> built;
      const std::vector<uint8_t> original(base + pli, base + pli + at + count * 64);
      if (lay.built.empty() && !lay.failed && AddSixthRowLayers(original, built)) {
        lay.built = std::move(built);
        REXLOG_INFO("Notes: sixth row layers added to menu_memo.pli ({} bytes)", lay.built.size());
      }
      // Ours replaces only menu_memo's list (the one with the Fishing Records row).
      bool memo = false;
      for (uint32_t i = 0; i < count && !memo; ++i) {
        memo = std::strncmp(reinterpret_cast<const char*>(base + pli + at + i * 64 + 32), kFishText, 32) == 0;
      }
      if (memo && !lay.built.empty()) {
        if (const uint32_t copy = PlaceCopy(lay, base)) ctx.r4.u64 = copy;
      }
    }
  }
  __imp__sub_823BEC18(ctx, base);
}

// The Notes list's input: A, up, down over six rows.
REX_HOOK_RAW(sub_8240DDA0) {
  if (!g_list.ready) {
    __imp__sub_8240DDA0(ctx, base);
    return;
  }
  const uint32_t obj = ctx.r3.u32;
  if (Pad(ctx, base, true, 0x02000007)) {
    if (g_list.ours) {
      nier::pc_options_state().ach_open = true;  // achievements_ui.h; B comes back here
      Sound(ctx, base, 2);
      return;
    }
    __imp__sub_8240DDA0(ctx, base);  // the game opens the category (it reads the same press)
    return;
  }
  int step = 0;
  if (Pad(ctx, base, false, 0x00010001)) step = -1;
  else if (Pad(ctx, base, false, 0x00020002)) step = +1;
  // The mouse: the row under it, by the same steps (menu_mouse.h).
  int target = -1;
  {  // the mouse on the PC, the finger on Android
    bool act = false;
    const int r = nier::menu_mouse::BookRowUnderMouse(6, act);
    if (act && r >= 0) target = r;
  }
  const uint32_t row = std::min<uint32_t>(Load32(base, obj + kOffRow), 4);
  const int visible = g_list.ours ? 5 : int(row);
  if (target == visible) target = -1;
  if (!step && target < 0) return;
  PlayRowAnimation(ctx, base, obj, kRowOut, row);
  const int next = target >= 0 ? target : (visible + step + 6) % 6;
  UseSixthRow(base, next == 5);
  const uint32_t game_row = uint32_t(std::min(next, 4));
  Store32(base, obj + kOffRow, game_row);
  PlayRowAnimation(ctx, base, obj, kRowIn, game_row);
  {
    rex::CallFrame f(ctx);
    f.ctx.r3.u64 = obj;
    sub_8240DC40(f, base);
  }
  Sound(ctx, base, 5);
}

// The list is shown (opening Notes, or back from a category): the fifth entries follow the row.
REX_HOOK_RAW(sub_8240FC78) {
  const uint32_t obj = ctx.r3.u32;
  SyncRow(base, obj);
  __imp__sub_8240FC78(ctx, base);
  SetRowText(ctx, base, obj);
}

REX_HOOK_RAW(sub_8240FB80) {
  const uint32_t obj = ctx.r3.u32;
  SyncRow(base, obj);
  __imp__sub_8240FB80(ctx, base);
  SetRowText(ctx, base, obj);
}
