// nier - "Video Options" inside the game's own Options menu.
//
// NieR's Options screen (list menu type 1) holds four 56-byte items: Game
// Settings, Difficulty, an unused blank row (kind 0x08, id 0x25 - the console's
// hard-disk install entry, which has no text on this version) and Return. We
// turn that blank row into a normal item with our own message ids, answer
// those ids from the game's message lookup, and when it is confirmed open the
// PC options panel instead of letting the game act on it.

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <SDL3/SDL_events.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "achievements.h"
#include "game_text.h"
#include "key_capture.h"
#include "menu_mouse.h"
#include "mods.h"
#include "../mods/sdk/nier_mod.h"
#include "pc_options.h"
#include "pad_remap.h"
#include "weapon_switch.h"
#include "pc_text.h"
#include "title_carousel.h"
#include "touch_controls.h"

// The menus' pointer (menu_mouse.h): the mouse on the PC, the finger on Android (touch_controls.h).
#define NIER_MENU_POINTER 1

extern "C" void __imp__sub_82419518(PPCContext& ctx, uint8_t* base);  // list menu draw
extern "C" void __imp__sub_823CB8C8(PPCContext& ctx, uint8_t* base);  // message id -> text
extern "C" void __imp__sub_82417798(PPCContext& ctx, uint8_t* base);  // list screen transition
extern "C" void __imp__sub_8241B908(PPCContext& ctx, uint8_t* base);  // change the menu state (with its transition)
// The game's menu sound: r3 = which, f1 / f2 = the floats its menus pass (0x820007A4, 0x820454E0).
// Notes' list plays 2 (moving) and 5 (A).
// It asks sub_823FFC80 first and may play nothing (an unlock in a menu page was silent): the sound
// player itself, sub_82112190(r3 = 0x8447BB90, r4 = which, f1, f2), is called instead.
extern "C" void __imp__sub_82112190(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8241A188(PPCContext& ctx, uint8_t* base);  // builds the list of a state
extern "C" void __imp__sub_824197A8(PPCContext& ctx, uint8_t* base);  // draws the settings list
extern "C" void __imp__sub_823BC8F0(PPCContext& ctx, uint8_t* base);  // 2D layout node by name
extern "C" void __imp__sub_82416C20(PPCContext& ctx, uint8_t* base);  // item -> its value text
extern "C" void __imp__sub_82404458(PPCContext& ctx, uint8_t* base);  // page's scroll bar thumb -> first row shown

namespace {

constexpr uint32_t kOptionsMenuType = 1;
constexpr uint32_t kOffMenuType = 3552, kOffScroll = 3564, kOffSelected = 3568;
constexpr uint32_t kOffItems = 3580, kItemSize = 56, kOffCount = 5260;
constexpr uint32_t kItemKind = 0, kItemId = 40, kItemLabel = 44, kItemHelp = 48;
constexpr uint8_t kKindBlank = 0x08, kKindEntry = 0x09;
constexpr uint32_t kBlankInstallId = 0x25;
constexpr uint32_t kLabelMessage = 0x7E000001, kHelpMessage = 0x7E000002;  // unused by the game
// "Control Options" and "Language", rows of our own inserted after Video Options.
constexpr uint32_t kControlsId = 0x7E;
constexpr uint32_t kControlsLabel = 0x7E000003, kControlsHelp = 0x7E000004;
constexpr uint32_t kLanguageId = 0x7D;
constexpr uint32_t kLanguageLabel = 0x7E000005, kLanguageHelp = 0x7E000006;
// "Quit to Desktop", in the pause menu's Options and in place of Return on the title's Options.
constexpr uint32_t kQuitId = 0x7C;
constexpr uint32_t kQuitLabel = 0x7E000007, kQuitHelp = 0x7E000008;
// "Achievements" on the title's Options (a seventh row; its texts are Notes' sixth row's).
constexpr uint32_t kAchievementsId = 0x7B, kAchievementsLabel = 9004000, kAchievementsHelp = 9004060;
constexpr uint32_t kMaxItems = 6;  // rows the list holds
// Nier Plus (2026-11-15): "Mods", the code mods' settings (mods.cpp): a row of the pause's Options, and the last row
// of the title's Video Options page (the title's list is full).
constexpr uint32_t kModsLabel = 0x7E000009, kModsHelp = 0x7E00000A;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void SetBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// Guest copies of our strings (the game only reads guest memory), one per
// distinct text (UTF-8, as the game's own text files).
uint32_t GuestString(const std::string& text) {
  static std::mutex mutex;
  static std::map<std::string, uint32_t> copies;
  std::lock_guard lock(mutex);
  uint32_t& addr = copies[text];
  if (!addr) {
    uint32_t size = uint32_t(text.size()) + 1;
    addr = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
    if (addr) std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(addr), text.c_str(), size);
  }
  return addr;
}
uint32_t GuestString(const char* text) { return GuestString(std::string(text)); }

// Rows of the pages that open inside the list (below). The game asks for their
// text through the message lookup, by these ids.
constexpr uint32_t kRowLabelBase = 0x7E100000, kRowHelpBase = 0x7E200000, kRowIdBase = 0x7000;
constexpr uint32_t kMaxRows = 29;  // the list holds 30 items

struct Row {
  bool back = false;                    // the game's own Return row
  std::function<std::string()> text;    // label
  std::function<std::string()> value;   // value column; none = an action row
  std::string help;
  std::function<void(int)> act;         // 0 = A, -1 / +1 = left / right
  // A slider: drawn as the game's own bar row (the sound volumes' kind 5: bytes 1-3 = value, lowest,
  // highest; the volumes are 5 of 1..10). {value index, number of values}.
  std::function<std::pair<int, int>()> level;
};

struct NativePages {
  std::mutex mutex;  // rows are read by the message lookup
  int page = 0;      // 0 = none open
  int pending = 0;   // asked for; the next build of the settings list takes it
  uint32_t state = 4;  // the menu state whose settings list holds the page (4 title, 7 pause)
  std::vector<Row> rows;
  int capturing = -1;
  bool entered = false;  // the game's state really is the settings one (after its transition)
  int waiting = 0;       // frames spent waiting for that
  // The game's own settings list, as it was before a page of ours replaced its rows. Put back when
  // the page closes: Start (Enter) on the title's list goes back to the title screen without
  // rebuilding it, and the next list build over our rows read 0x834 (crash, 2026-11-12).
  int screen = -1;  // pause page: the pause menu's screen byte (0x82757AC5) when it opened
  // The pause list's row the page was opened from: Return puts the selection back there (the game's
  // cursor stays drawn on it; selecting row 0 left the cursor drawn on one row and acting on another).
  uint32_t return_scroll = 0, return_selected = 0;
  uint32_t saved_obj = 0;
  uint32_t saved_count = 0, saved_scroll = 0, saved_selected = 0;
  std::vector<uint8_t> saved_items;
  // Row kinds of the game's list (copied when a page opens): ours are made from them.
  uint8_t t_choice[56] = {}, t_action[56] = {}, t_back[56] = {}, t_bar[56] = {}, t_plain[56] = {};
  bool have_bar = false, have_plain = false;
  int ach_part = 0;      // achievements page: which half of the list
  bool refill = false;   // the page's rows change (a half turned): write them again
  int mod = -1;             // Nier Plus: the mod whose page (6) is open
  int capture_hotkey = -1;  // Nier Plus: the mod panel whose key is being captured
  int pad_capturing = -1;   // Controller page: the action waiting for its button (pad_remap.h)
  uint64_t pad_capture_ms = 0;
};
NativePages g_pages;

// Another page of ours in the same list (the Mods pages): written over it at the end of this input's handling.
void GoToPage(int page) {
  {
    std::lock_guard lock(g_pages.mutex);
    g_pages.page = page;
  }
  nier::pc_options_state().native_page = page;
  g_pages.refill = true;
}

// The rows we add speak the game's current language, or the translation's (pc_text.h).
uint32_t Say(const char* en) { return GuestString(nier::pc_text::T(en)); }

// Inserts a copy of row `after` right below it, with our id and messages.
uint32_t InsertRow(uint8_t* obj, uint32_t count, uint32_t after, uint32_t id, uint32_t label,
                   uint32_t help) {
  uint8_t* items = obj + kOffItems;
  std::memmove(items + (after + 2) * kItemSize, items + (after + 1) * kItemSize,
               (count - after - 1) * kItemSize);
  uint8_t* item = items + (after + 1) * kItemSize;
  std::memcpy(item, items + after * kItemSize, kItemSize);
  SetBe32(item + kItemId, id);
  SetBe32(item + kItemLabel, label);
  SetBe32(item + kItemHelp, help);
  SetBe32(obj + kOffCount, count + 1);
  return after + 1;
}

int FindRow(const uint8_t* obj, uint32_t count, uint32_t id) {
  for (uint32_t i = 0; i < count && i < 8; ++i) {
    if (Be32(obj + kOffItems + i * kItemSize + kItemId) == id) return int(i);
  }
  return -1;
}

// The 2D layout nodes of the list rows (names at 0x82761360, list A, and 0x827612EC, list B; the tables the game's
// own row drawing uses), for pointing at them with the mouse (menu_mouse.h).
void CacheMenuNodes(PPCContext& ctx, uint8_t* base, uint32_t obj_addr) {
  auto& n = nier::menu_mouse::nodes();
  PPCContext saved = ctx;
  auto lookup = [&](uint32_t table, uint32_t i) {
    ctx.r3.u64 = obj_addr + 48;
    ctx.r4.u64 = Be32(base + table + i * 4);
    __imp__sub_823BC8F0(ctx, base);
    return ctx.r3.u32;
  };
  for (int i = 0; i < nier::menu_mouse::kRowsA; ++i) n.a[i] = lookup(0x82761360, i);
  for (int i = 0; i < nier::menu_mouse::kRowsB; ++i) n.b[i] = lookup(0x827612EC, i);
  ctx = saved;
  n.obj = obj_addr;
}

}  // namespace

// TEMP (MENUDBG): last list menu object built, for NIER_MSGDBG's item dump.
std::atomic<uint8_t*> g_menudbg_obj{nullptr};

REX_HOOK_RAW(sub_82419518) {
  uint8_t* obj = base + ctx.r3.u32;
  g_menudbg_obj = obj;
  nier::menu_mouse::nodes().drawn_ms = nier::menu_mouse::NowMs();
  if (Be32(obj + kOffMenuType) == kOptionsMenuType) {
    uint32_t count = Be32(obj + kOffCount);
    int ours = -1;
    for (uint32_t i = 0; i < count && i < 8; ++i) {
      uint8_t* item = obj + kOffItems + i * kItemSize;
      if (Be32(item + kItemId) != kBlankInstallId) continue;
      if (item[kItemKind] == kKindBlank) {
        item[kItemKind] = kKindEntry;
        SetBe32(item + kItemLabel, kLabelMessage);
        SetBe32(item + kItemHelp, kHelpMessage);
      }
      ours = int(i);
    }
    // Below Video Options (Return moves down): Control Options, then Language.
    int controls = FindRow(obj, count, kControlsId);
    if (ours >= 0 && controls < 0 && count < kMaxItems) {
      controls = int(InsertRow(obj, count, uint32_t(ours), kControlsId, kControlsLabel, kControlsHelp));
      count = Be32(obj + kOffCount);
    }
    int language = FindRow(obj, count, kLanguageId);
    if (ours >= 0 && controls >= 0 && language < 0 && count < kMaxItems) {
      language = int(InsertRow(obj, count, uint32_t(controls), kLanguageId, kLanguageLabel, kLanguageHelp));
      count = Be32(obj + kOffCount);
    }
    // The list is full (its 6 items end where the settings list's begin): the game's Return row,
    // the last one, becomes Achievements (B still goes back, as on every screen of the game). A
    // seventh row (in the settings list's memory) showed twice and acted as the row below it
    // (2026-11-12); Quit to Desktop stays in the pause menu's Options.
    if (ours >= 0 && language >= 0 && count == kMaxItems && uint32_t(language) == count - 2 &&
        FindRow(obj, count, kAchievementsId) < 0) {
      uint8_t* last = obj + kOffItems + (count - 1) * kItemSize;
      SetBe32(last + kItemId, kAchievementsId);
      SetBe32(last + kItemLabel, kAchievementsLabel);
      SetBe32(last + kItemHelp, kAchievementsHelp);
    }
    if (ours >= 0) {
      auto& s = nier::pc_options_state();
      s.game_options_menu = obj;
      s.game_item_index = ours;
      s.game_controls_index = controls;
      s.game_language_index = language;
      s.game_options_on_screen = true;
    }
  }
  __imp__sub_82419518(ctx, base);
  CacheMenuNodes(ctx, base, uint32_t(obj - base));
}

// Screen transition of the list menus (r4 = kind). Measured order when the
// Options screen opens: list built, then kind 1 (its own appear animation)
// ~0.4 s later; leaving it is kind 4 (into a submenu) or 5 (back). Backing
// out with B to the title menu makes no call here (handled in GateDriver).
REX_HOOK_RAW(sub_82417798) {
  if (ctx.r4.u32 != 1) {
    nier::pc_options_state().game_options_on_screen = false;
  }
  __imp__sub_82417798(ctx, base);
}

REX_HOOK_RAW(sub_823CB8C8) {
  uint32_t id = ctx.r4.u32;
  // The heading Grimoire Weiss's book looks up (from 0x82404CE4) when it shows Items, Quests,
  // Weapons or Word Edit: corrects the page count the gate keeps (input_recorder.h). Maps and Notes
  // look up nothing of their own, and the book looks up the next pages' texts ahead.
  if (uint32_t(ctx.lr) == 0x82404CE4) {
    switch (id) {
      case 0x632EA0: nier::pc_options_state().book_tab = 1; break;  // Items
      case 0x5E9AC0: nier::pc_options_state().book_tab = 2; break;  // Quests
      case 0x602160: nier::pc_options_state().book_tab = 3; break;  // Weapons
      case 0x5D1420: nier::pc_options_state().book_tab = 4; break;  // Word Edit
      default: break;
    }
  }
  {  // TEMP (MSGDBG): NIER_MSGDBG=1 logs who looks up which message, per 2 s.
    static const bool dbg = std::getenv("NIER_MSGDBG") != nullptr;
    if (dbg) {
      static std::mutex m;
      static std::map<std::pair<uint32_t, uint32_t>, uint32_t> counts;
      static auto last = std::chrono::steady_clock::now();
      std::lock_guard lock(m);
      ++counts[{ctx.lr, id}];
      auto now = std::chrono::steady_clock::now();
      if (now - last > std::chrono::seconds(2)) {
        last = now;
        std::string line;
        for (auto& [k, n] : counts) line += fmt::format(" {:08X}:{:X}x{}", k.first, k.second, n);
        REXLOG_INFO("MSGDBG{}", line);
        counts.clear();
      }
    }
  }
  if ((id >> 20) == (kRowLabelBase >> 20) || (id >> 20) == (kRowHelpBase >> 20)) {
    std::string text;
    bool found = false;
    {
      std::lock_guard lock(g_pages.mutex);
      const size_t index = (id & 0xFFFFF);
      if (index < g_pages.rows.size() && !g_pages.rows[index].back) {
        const Row& row = g_pages.rows[index];
        text = (id >> 20) == (kRowLabelBase >> 20) ? (row.text ? row.text() : std::string()) : row.help;
        found = true;
      }
    }
    if (found) {
      ctx.r3.u64 = GuestString(text);
      return;
    }
  }
  switch (id) {
    case kLabelMessage: ctx.r3.u64 = Say("Video Options"); return;
    case kHelpMessage: ctx.r3.u64 = Say("Adjust resolution, image quality and display settings."); return;
    case kControlsLabel: ctx.r3.u64 = Say("Control Options"); return;
    case kControlsHelp: ctx.r3.u64 = Say("Touch, keyboard and mouse, and controller."); return;
    case kLanguageLabel: ctx.r3.u64 = Say("Language"); return;
    case kLanguageHelp: ctx.r3.u64 = Say("Change the language of the game text."); return;
    case kQuitLabel: ctx.r3.u64 = Say("Quit to Desktop"); return;
    case kQuitHelp: ctx.r3.u64 = Say("Close the game. Progress since the last save is lost."); return;
    case kModsLabel: ctx.r3.u64 = Say("Mods"); return;
    case kModsHelp: ctx.r3.u64 = Say("Settings of the installed mods."); return;
    case 0x7E00000B: ctx.r3.u64 = GuestString("Jukebox"); return;  // the pause's key hint (RB)
    case 9004000: ctx.r3.u64 = Say("Achievements"); return;            // Notes' sixth row (memo_achievements.cpp)
    case 9004060: ctx.r3.u64 = Say("See your achievements."); return;  // its help
    default: break;
  }
  __imp__sub_823CB8C8(ctx, base);
}

// The key hints bar's whole set (sub_823F1690: r3 the bar, then up to 8 pairs icon, message: r5..r10, the rest in the
// caller's parameter words from r1 + 84, 8 apart; a negative one ends the list).
extern "C" void __imp__sub_823F1690(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_823F1690) {
  // The pause's cross (its call from 0x823F199C: Select, Confirm, Cancel): a fourth hint, RB Jukebox, which the
  // game draws with its own icons (the controller's style, the keyboard's key; game_text_hooks.cpp). RB then
  // opens the page (input_recorder.h, JukeboxHintShown). Icons are bits: 1 A, 2 B, 16 LT, 32 RT, 64 LB, 128 RB.
  const uint32_t sp = ctx.r1.u32;
  // The save list and other screens share the call: the cross is the one Start just opened, or the one shown again
  // when one of its arms closes (it was on top moments ago).
  const auto& st = nier::pc_options_state();
  const uint64_t now = nier::menu_mouse::NowMs();
  const bool pause = now - st.start_press_ms.load() < 1500 || now - st.cross_seen_ms.load() < 4000;
  if (pause && uint32_t(ctx.lr) == 0x823F199C && ctx.r6.u32 == 0xA95F60 && ctx.r8.u32 == 0xA95FC4 && ctx.r10.u32 == 0xA96028 &&
      int32_t(Be32(base + sp + 84)) < 0) {
    SetBe32(base + sp + 84, 128);
    SetBe32(base + sp + 92, 0x7E00000B);
    SetBe32(base + sp + 100, 0xFFFFFFFFu);
  }
  __imp__sub_823F1690(ctx, base);
}

// TEMP (MENUDBG): NIER_MSGDBG also dumps the last list menu every 2 s.
void MenuDebugTick() {
  static const bool dbg = std::getenv("NIER_MSGDBG") != nullptr;
  if (!dbg) return;
  static auto last = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(2)) return;
  last = now;
  uint8_t* obj = g_menudbg_obj.load();
  if (!obj) return;
  uint32_t count = Be32(obj + kOffCount);
  REXLOG_INFO("MENUDBG type={} count={} scroll={} sel={}", Be32(obj + kOffMenuType), count,
              Be32(obj + kOffScroll), Be32(obj + kOffSelected));
  {
    std::string head;
    for (uint32_t o = 3552; o < 3580; o += 4) head += fmt::format(" {:08X}", Be32(obj + o));
    REXLOG_INFO("MENUDBG head{} | B: scroll={} sel={} count={}", head, Be32(obj + 3572),
                Be32(obj + 3576), Be32(obj + 5264));
    for (uint32_t i = 0; i < Be32(obj + 5264) && i < 24; ++i) {
      std::string hex;
      for (uint32_t b = 0; b < kItemSize; b += 4) hex += fmt::format(" {:08X}", Be32(obj + 3916 + i * kItemSize + b));
      REXLOG_INFO("MENUDBG  B{}{}", i, hex);
    }
  }
  for (uint32_t i = 0; i < count && i < 10; ++i) {
    const uint8_t* item = obj + kOffItems + i * kItemSize;
    std::string hex;
    for (uint32_t b = 0; b < kItemSize; b += 4) hex += fmt::format(" {:08X}", Be32(item + b));
    REXLOG_INFO("MENUDBG  item{}{}", i, hex);
  }
}


// ---------------------------------------------------------------------------
// Pages in the game's own settings list.
//
// Video Options, Control Options and Language are not screens of ours. The
// Options screen is one object with two lists: the 6-row menu (Game Settings,
// Difficulty, ...) and the settings list (9 rows with scrolling, a scroll bar,
// values on the right and arrows), which Game Settings itself uses (state 4).
// A on one of our rows asks the game for that same state - with its own page
// transition - and when it builds the list we put our rows in it. The game then
// scrolls, moves the cursor, plays sounds and shows Return as it does for its
// own settings. Value text comes from our hook of the item-to-text function;
// the input thread (GateDriver) only reports A / left / right, which we take
// from the game so it does not change anything by itself. Leaving the state
// (B or Return, native) saves nier.toml.

namespace {

constexpr uint32_t kOffState = 3556;  // 1 = the Options menu, 4 = Game Settings
constexpr uint32_t kOffBScroll = 3572, kOffBSelected = 3576, kOffBItems = 3916, kOffBCount = 5264;
constexpr uint32_t kSettingsState = 4, kMaxBItems = 24;
constexpr uint8_t kKindChoice = 2, kKindAction = 0x0A;
// The pause menu's Options: list menu type 7, its settings list built in state 7. 24 items (the
// most it holds): Return (0x16), blank, Quit Game (0x18, to the title), Load (0x19), blank, the
// settings, blank, Restore Defaults (0x1A), Select Storage Device (0x1B).
constexpr uint32_t kPauseMenuType = 7, kPauseState = 7;
constexpr uint32_t kReturnId = 0x16, kQuitGameId = 0x18, kSelectDeviceId = 0x1B;

// Return, Video Options, Control Options, Quit Game, Quit to Desktop, Load, the settings,
// Restore Defaults: the blanks and Select Storage Device (nothing to pick on a PC) make the room.
void RewritePauseList(uint8_t* obj) {
  const uint32_t count = Be32(obj + kOffBCount);
  if (count < 2 || count > kMaxBItems) return;
  uint8_t* items = obj + kOffBItems;
  const uint8_t* action = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* item = items + i * kItemSize;
    // Already done (our first row is there; Quit to Desktop may be Quit Game while its dialog is up).
    if (Be32(item + kItemId) == nier::PcOptionsState::kRowQuit ||
        (Be32(item + kItemId) == nier::PcOptionsState::kRowVideo && Be32(item + kItemLabel) == kLabelMessage)) {
      return;
    }
    if (!action && item[kItemKind] == kKindAction && Be32(item + kItemId) == kQuitGameId) action = item;
  }
  if (!action || Be32(items + kItemId) != kReturnId) {
    REXLOG_WARN("Menu: opcoes do pause inesperadas; linhas do PC nao postas");
    return;
  }
  std::vector<std::array<uint8_t, kItemSize>> rows;
  auto ours = [&](uint32_t id, uint32_t label, uint32_t help) {
    std::array<uint8_t, kItemSize> row;
    std::memcpy(row.data(), action, kItemSize);
    SetBe32(row.data() + kItemId, id);
    SetBe32(row.data() + kItemLabel, label);
    SetBe32(row.data() + kItemHelp, help);
    rows.push_back(row);
  };
  auto keep = [&](const uint8_t* item) {
    std::array<uint8_t, kItemSize> row;
    std::memcpy(row.data(), item, kItemSize);
    rows.push_back(row);
  };
  keep(items);  // Return
  ours(nier::PcOptionsState::kRowVideo, kLabelMessage, kHelpMessage);
  ours(nier::PcOptionsState::kRowControls, kControlsLabel, kControlsHelp);
  if (!nier::mods::Mods().empty()) ours(nier::PcOptionsState::kRowMods, kModsLabel, kModsHelp);
  // No Language here (2026-11-14, the user): mid-game the loaded dialogue stays in the old language and the rest
  // breaks; it is chosen in the title's Options.
  for (uint32_t i = 1; i < count; ++i) {
    const uint8_t* item = items + i * kItemSize;
    const uint32_t id = Be32(item + kItemId);
    if (item[kItemKind] == kKindBlank || id == kSelectDeviceId) continue;
    keep(item);
    if (id == kQuitGameId) ours(nier::PcOptionsState::kRowQuit, kQuitLabel, kQuitHelp);
  }
  if (rows.size() > kMaxBItems) rows.resize(kMaxBItems);
  for (size_t i = 0; i < rows.size(); ++i) std::memcpy(items + i * kItemSize, rows[i].data(), kItemSize);
  SetBe32(obj + kOffBCount, uint32_t(rows.size()));
}

// Removes the Select Storage Device row (action 0x1B) from a settings list; true if it was there.
bool DropSelectDevice(uint8_t* obj) {
  const uint32_t count = Be32(obj + kOffBCount);
  if (count < 2 || count > kMaxBItems) return false;
  uint8_t* items = obj + kOffBItems;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* item = items + i * kItemSize;
    if (item[kItemKind] != kKindAction || Be32(item + kItemId) != kSelectDeviceId) continue;
    std::memmove(item, item + kItemSize, (count - 1 - i) * kItemSize);
    SetBe32(obj + kOffBCount, count - 1);
    const uint32_t selected = Be32(obj + kOffBSelected);
    if (selected >= count - 1) SetBe32(obj + kOffBSelected, count - 2);
    return true;
  }
  return false;
}

struct EnOption {
  const char* cvar;
  const char* label;
  std::vector<const char*> values;  // same order as the option's values
  const char* help;
};

const std::vector<EnOption>& EnOptions() {
  static const std::vector<EnOption> options = {
      {"perf_overlay", "Performance Meter", {"Off", "On"},
       "Shows real fps, frame time, GPU and CPU use. Shortcut: F2."},
      {"fullscreen", "Window Mode", {"Fullscreen", "Windowed"}, "Fullscreen or windowed."},
      {"resolution", "Resolution", {"720p", "1080p", "1440p", "4K"},
       "Window size and how many pixels are drawn."},
      {"render_scale_percent", "Resolution Scale", {"50%", "67%", "100%", "150%", "200%", "300%"},
       "Below 100% draws fewer pixels and scales up (use AMD FSR): much lighter. Above: sharper but heavier."},
      {"dynamic_resolution_level", "3D Sharpness", {"Auto", "Lowest", "Medium", "Highest"},
       "The game's inner 3D resolution. Auto stays Highest while the frame rate limit is reached and "
       "lowers it only as much as needed."},
      {"min_3d_resolution_percent", "Lowest 3D Sharpness", {"768x432 (original)", "640x360", "512x288"},
       "Size of the Lowest level (and of Auto when the game struggles). Smaller is lighter."},
      {"fps_60", "Frame Rate", {"30 fps", "60 fps"},
       "60 fps in gameplay. Heavier: at 1080p or more the GPU may not keep up."},
      {"vsync", "V-Sync", {"On", "Off (unstable)"}, "Off, the game loses its frame pacing. Recommended: on."},
      {"renut_vsync", "V-Sync", {"On", "Off"}, "Off, frames do not wait for the display (may tear) and can pass its rate."},
      {"frame_rate_limit", "Frame Rate Limit", {},
       "Frames per second, up to Unlimited. With V-Sync on the display also limits it. More fps is heavier."},
      {"sa2_renut_msaa", "Anti-Aliasing (MSAA)", {"Off", "2x", "4x", "8x"},
       "Smooths jagged edges. Each step costs more GPU time."},
      {"renut_anisotropic_filtering", "Anisotropic Filter", {"Game's", "2x", "4x", "8x", "16x"},
       "Sharp textures at slanted angles (floors, walls)."},
      {"nier_shadows", "Shadows", {"On", "Off"},
       "Shadows cast by characters and objects. Off is lighter: more fps on weak devices."},
      {"swap_post_effect", "Anti-Aliasing", {"Off", "FXAA", "FXAA Extreme"}, "Smooths jagged edges."},
      {"native_2x_msaa", "MSAA 2x", {"On", "Off"}, "The Xbox 360's own anti-aliasing."},
      {"present_effect", "Upscaling", {"Bilinear", "AMD CAS (sharpness)", "AMD FSR"},
       "How the image is scaled to the screen. AMD FSR upscales with sharp edges, but only gains speed with "
       "Resolution Scale below 100%: 75% = Quality, 67% = Balanced, 50% = Performance."},
      {"anisotropic_override", "Anisotropic Filter", {"Off", "2x", "4x", "8x", "16x"},
       "Sharp textures at slanted angles (floors, walls)."},
      {"present_letterbox", "Black Bars", {"On", "Off"}, "Keeps 16:9 on screens of another shape."},
      {"nier_button_icons", "Button Icons", {"Xbox 360", "PlayStation 3"},
       "The controller's icons in hints and menus. With the keyboard and mouse the keys show."},
      {"nier_screen_fit", "Screen Fit", {"Fill (crops)", "Black Bars", "Stretch", "Widescreen", "Real Widescreen"},
       "The game is 16:9. On wider screens: Fill zooms in, cropping a little off the top and bottom; Black Bars "
       "shows the whole picture; Stretch fills the screen, widening the picture; Widescreen fills the screen with "
       "the HUD in shape in the middle; Real Widescreen opens the camera to the sides (more view, nothing "
       "stretched) and takes the HUD to the edges."},
      {"nier_brightness", "Brightness", {"-2", "-1", "Game's", "+1", "+2", "+3"},
       "Lightens or darkens the picture's mid tones (black and white stay)."},
      {"nier_near_blur", "Near Blur", {"On (game's)", "Off"},
       "The blur on characters when the camera comes close. Off: they stay sharp (better at 4K). The far blur stays."},
      {"execute_unclipped_draw_vs_on_cpu", "Full Effects", {"On", "Off"},
       "Fog and other full-screen effects. Recommended: on."},
      {"mnk_mouse", "Mouse Camera", {"On", "Off (arrows)"},
       "The mouse turns the camera (the cursor is hidden in game). Off: camera on the arrows."},
      {"mnk_sensitivity", "Mouse Sensitivity", {"1", "2", "3", "4", "5", "6", "8", "10"},
       "Camera speed with the mouse."},
  };
  return options;
}

const char* const kEnBindLabels[] = {
    "Move: Forward", "Move: Back", "Move: Left", "Move: Right", "Jump", "Action", "Attack",
    "Heavy Attack", "Recenter Camera", "Dodge", "Magic 1", "Guard", "Magic 2", "Camera: Up",
    "Camera: Down", "Camera: Left", "Camera: Right", "Walk", "Main Menu", "Large Map",
    "Item Shortcut", "One-Handed Sword", "Two-Handed Sword", "Spear"};

std::string KeyEn(const std::string& key) {
  static const std::pair<const char*, const char*> kNames[] = {
      {"Return", "Enter"}, {"Escape", "Esc"}, {"Control", "Ctrl"}, {"LMB", "Left Click"},
      {"RMB", "Right Click"}, {"MMB", "Middle Click"}, {"Semicolon", ";"}, {"Quote", "'"},
      {"Comma", ","}, {"Period", "."}, {"Slash", "/"}, {"Backslash", "\\"}, {"LBracket", "["},
      {"RBracket", "]"}, {"Minus", "-"}, {"Plus", "="}, {"PageUp", "Page Up"},
      {"PageDown", "Page Down"}};
  for (auto [name, shown] : kNames) {
    if (key == name) return nier::pc_text::T(shown);
  }
  return key.empty() ? "-" : key;
}

std::string AsciiOnly(std::string s) {
  for (char& c : s) {
    if (static_cast<unsigned char>(c) >= 0x80) c = '?';
  }
  return s;
}

int OptionIndex(const nier::PcOption& o) { return nier::OptionValueIndex(o); }

Row OptionRow(const nier::PcOption& o) {
  const EnOption* en = nullptr;
  for (const EnOption& e : EnOptions()) {
    if (std::strcmp(e.cvar, o.cvar) == 0) en = &e;
  }
  Row row;
  const std::string label = en ? nier::pc_text::T(en->label) : AsciiOnly(o.label);
  row.text = [label] { return label; };
  row.value = [o, en] {
    const size_t i = size_t(OptionIndex(o));
    if (o.slider) {  // the bar is the game's own (Row::level); the value beside it
      const std::string& v = o.values[i].second;
      return v == "Ilimitado" ? nier::pc_text::T("Unlimited") : AsciiOnly(v);
    }
    // The English names only when they match the list (Renut's and Xenos's lists differ).
    return en && en->values.size() == o.values.size() ? nier::pc_text::T(en->values[i]) : AsciiOnly(o.values[i].second);
  };
  const bool restart = o.needs_restart && !(o.live_on_vulkan && rex::cvar::GetFlagByName("renderer") == "vulkan");
  row.help = (en ? nier::pc_text::T(en->help) : std::string()) +
             (restart ? nier::pc_text::T(" (Takes effect after restarting the game.)") : std::string());
  row.act = [o](int dir) {
    const int n = int(o.values.size());
    const int step = dir == 0 ? 1 : dir;
    // A slider stops at its ends (A steps forward and comes back round from the last).
    const int i = o.slider ? (dir == 0 && OptionIndex(o) == n - 1 ? 0 : std::clamp(OptionIndex(o) + step, 0, n - 1))
                           : (OptionIndex(o) + step + n) % n;
    if (!rex::cvar::SetFlagByName(o.cvar, o.values[i].first)) {
      REXLOG_WARN("Opcoes de PC: {} = {} rejeitado", o.cvar, o.values[i].first);
    } else {
      nier::AfterOptionChange(o);
    }
  };
  // The bar only where a level reads well (fps); the resolution scale is a number alone.
  // The value goes beside the label (left aligned, room before the arrow): the value column is
  // centred on the bar and no padding kept every value clear of it.
  if (o.slider && std::strcmp(o.cvar, "render_scale_percent") != 0) {
    row.level = [o] { return std::pair{OptionIndex(o), int(o.values.size())}; };
    row.text = [label, o] {
      const std::string& v = o.values[size_t(OptionIndex(o))].second;
      return label + "   " + (v == "Ilimitado" ? nier::pc_text::T("Unlimited") : AsciiOnly(v));
    };
    row.value = [] { return std::string(); };
  }
  return row;
}

void AssignKey(size_t index, const std::string& key) {
  const auto& binds = nier::KeyBindRows();
  const std::string old = nier::PrimaryKey(rex::cvar::GetFlagByName(binds[index].cvar));
  for (size_t i = 0; i < binds.size(); ++i) {
    const std::string value = rex::cvar::GetFlagByName(binds[i].cvar);
    if (i != index && nier::PrimaryKey(value) == key) {
      rex::cvar::SetFlagByName(binds[i].cvar, nier::WithPrimaryKey(value, old));  // swap
    }
  }
  rex::cvar::SetFlagByName(binds[index].cvar,
                           nier::WithPrimaryKey(rex::cvar::GetFlagByName(binds[index].cvar), key));
}

// The page's rows (without the Return row, which is the game's own) and the
// row the cursor starts on.
std::vector<Row> BuildPage(int page, uint32_t* selected) {
  std::vector<Row> rows;
  const auto options = nier::MakePcOptions();
  auto find = [&](const char* cvar) -> const nier::PcOption* {
    for (const auto& o : options) {
      if (std::strcmp(o.cvar, cvar) == 0) return &o;
    }
    return nullptr;
  };
  if (page == 1) {
    for (const auto& o : options) {
      if (std::strcmp(o.cvar, "mnk_mouse") != 0 && std::strcmp(o.cvar, "mnk_sensitivity") != 0) {
        rows.push_back(OptionRow(o));
      }
    }
    // Nier Plus: the title's way to the mods' settings (its Options list has no room for a row).
    if (g_pages.state == kSettingsState && !nier::mods::Mods().empty()) {
      Row row;
      row.text = [] { return nier::pc_text::T("Mods"); };
      row.help = nier::pc_text::T("Settings of the installed mods.");
      row.act = [](int dir) {
        if (dir == 0) GoToPage(5);
      };
      rows.push_back(std::move(row));
    }
  } else if (page == 5) {
    // Nier Plus: the installed code mods (mods.cpp), one row each; A opens its own page (6).
    for (const nier::mods::ModInfo& m : nier::mods::Mods()) {
      Row row;
      const int index = m.index;
      const std::string title = m.title;
      row.text = [index, title] {
        const auto mods = nier::mods::Mods();
        const bool on = size_t(index) < mods.size() && mods[size_t(index)].enabled;
        return on ? title : title + "  " + nier::pc_text::T("(off)");
      };
      row.help = nier::pc_text::T("Opens this mod's settings.");
      row.act = [index](int dir) {
        if (dir != 0) return;
        g_pages.mod = index;
        GoToPage(6);
      };
      rows.push_back(std::move(row));
    }
  } else if (page == 6) {
    // Nier Plus: one mod's page: on / off, its panels' keys (PC), its settings, back to the list.
    const int mod = g_pages.mod;
    {
      Row row;
      row.text = [] { return nier::pc_text::T("Mod Active"); };
      row.value = [mod] {
        const auto mods = nier::mods::Mods();
        const bool on = size_t(mod) < mods.size() && mods[size_t(mod)].enabled;
        return nier::pc_text::T(on ? "On" : "Off");
      };
      row.help = nier::pc_text::T("Turns this mod on or off. Off, it does nothing.");
      row.act = [mod](int) {
        const auto mods = nier::mods::Mods();
        if (size_t(mod) < mods.size()) nier::mods::SetModEnabled(mod, !mods[size_t(mod)].enabled);
      };
      rows.push_back(std::move(row));
    }
#if !defined(__ANDROID__)
    for (const nier::mods::HotkeyInfo& h : nier::mods::Hotkeys(mod)) {
      Row row;
      const int panel = h.panel;
      const std::string label = nier::pc_text::T("Shortcut") + ": " + h.title;
      row.text = [label] { return label; };
      row.value = [mod, panel] {
        if (g_pages.capture_hotkey == panel) return nier::pc_text::T("Press a key...");
        for (const auto& k : nier::mods::Hotkeys(mod)) {
          if (k.panel == panel) return KeyEn(k.key);
        }
        return std::string("-");
      };
      row.help = nier::pc_text::T("Key that opens this mod's window. Press A, then the new key. Esc cancels.");
      row.act = [panel](int dir) {
        if (dir != 0) return;
        g_pages.capture_hotkey = panel;
        nier::BeginKeyCapture();
        nier::pc_options_state().native_capturing = true;
      };
      rows.push_back(std::move(row));
    }
#endif
    for (nier::mods::MenuRow& m : nier::mods::ModRows(mod)) {
      Row row;
      const std::string label = m.label;
      row.text = [label] { return label; };
      row.value = m.value;
      row.act = m.act;
      row.help = nier::pc_text::T(m.value ? "Changes this mod's setting." : "Runs this mod's action.");
      rows.push_back(std::move(row));
    }
    {
      Row row;
      row.text = [] { return nier::pc_text::T("Back to Mods"); };
      row.help = nier::pc_text::T("Back to the list of installed mods.");
      row.act = [](int dir) {
        if (dir == 0) GoToPage(5);
      };
      rows.push_back(std::move(row));
    }
  } else if (page == 2) {
    // Control Options (2026-11-17): the three ways to play, on the PC and the phone alike (PCs with touch screens,
    // phones with a keyboard and mouse): each its own page.
    auto sub = [&](const char* label, const char* help, int to) {
      Row row;
      row.text = [label] { return nier::pc_text::T(label); };
      row.help = nier::pc_text::T(help);
      row.act = [to](int dir) {
        if (dir == 0) GoToPage(to);
      };
      rows.push_back(std::move(row));
    };
    sub("Touch Controls", "The on-screen buttons: when they show, their size and places.", 7);
    sub("Mouse and Keyboard", "The keys of each action and the mouse camera.", 8);
    sub("Controller", "Which controller button does each action.", 9);
    {  // The quick weapon switch (weapon_switch.h): for every device.
      Row row;
      row.text = [] { return nier::pc_text::T("Weapon Switch"); };
      row.value = [] { return nier::pc_text::T(nier::weapon_switch::HoldMode() ? "Hold" : "Tap"); };
      row.help = nier::pc_text::T(
          "Tap: a tap on a weapon button equips that kind's weapon at once, holding opens the menu. Hold: the other way "
          "round. Once Nier has every kind of weapon.");
      row.act = [](int) {
        rex::cvar::SetFlagByName("nier_weapon_switch", nier::weapon_switch::HoldMode() ? "tap" : "hold");
      };
      rows.push_back(std::move(row));
    }
  } else if (page == 7 || page == 8 || page == 9) {
    auto back = [&] {
      Row row;
      row.text = [] { return nier::pc_text::T("Back to Control Options"); };
      row.help = nier::pc_text::T("Back to the list of controls.");
      row.act = [](int dir) {
        if (dir == 0) GoToPage(2);
      };
      rows.push_back(std::move(row));
    };
    if (page == 9) {
      // The controller (pad_remap.h): each action's physical button; A, then the new button.
      namespace remap = nier::pad_remap;
      for (int i = 0; i < remap::kCount; ++i) {
        Row row;
        row.text = [i] { return nier::pc_text::T(remap::ActionName(i)); };
        row.value = [i] {
          if (g_pages.pad_capturing == i) return nier::pc_text::T("Press a button...");
          return remap::ButtonName(remap::Map()[size_t(i)]);
        };
        row.help = nier::pc_text::T("Press A, then the new button. Two actions trade buttons.");
        row.act = [i](int dir) {
          if (dir != 0) return;
          g_pages.pad_capturing = i;
          g_pages.pad_capture_ms = nier::menu_mouse::NowMs();
          remap::BeginCapture();
          nier::pc_options_state().native_capturing = true;
        };
        rows.push_back(std::move(row));
      }
      Row defaults;
      defaults.text = [] { return nier::pc_text::T("Restore Defaults"); };
      defaults.help = nier::pc_text::T("Every button back to its own action.");
      defaults.act = [](int dir) {
        if (dir == 0) remap::RestoreDefaults();
      };
      rows.push_back(std::move(defaults));
      back();
    } else if (page == 7) {
    // The on-screen buttons (touch_controls.h).
    namespace touch = nier::touch;
    auto percent = [](float v) { return std::to_string(int(std::lround(v * 100))) + "%"; };
    // Steps a value along a list, to the nearest one first.
    auto step = [](const std::vector<float>& steps, float now, int dir) {
      size_t at = 0;
      for (size_t i = 0; i < steps.size(); ++i) {
        if (std::fabs(steps[i] - now) < std::fabs(steps[at] - now)) at = i;
      }
      const int next = std::clamp(int(at) + (dir == 0 ? 1 : dir), 0, int(steps.size()) - 1);
      return steps[size_t(next)];
    };
    {
      Row row;
      row.text = [] { return nier::pc_text::T("On-Screen Buttons"); };
      row.value = [] {
        const int m = touch::Current().mode;
        return nier::pc_text::T(m == 0 ? "Off" : m == 2 ? "Always" : "Automatic");
      };
      row.help = nier::pc_text::T("Automatic: shown while no controller is connected.");
      row.act = [](int dir) { touch::Change([dir](touch::Layout& l) { l.mode = (l.mode + (dir < 0 ? 2 : 1)) % 3; }); };
      rows.push_back(std::move(row));
    }
    {
      Row row;
      row.text = [] { return nier::pc_text::T("Button Size"); };
      row.value = [percent] { return percent(touch::Current().scale); };
      row.help = nier::pc_text::T("Size of all the on-screen buttons.");
      row.act = [step](int dir) {
        touch::Change([&](touch::Layout& l) {
          l.scale = step({0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.75f, 2.0f}, l.scale, dir);
        });
      };
      rows.push_back(std::move(row));
    }
    {
      Row row;
      row.text = [] { return nier::pc_text::T("Button Opacity"); };
      row.value = [percent] { return percent(touch::Current().opacity); };
      row.help = nier::pc_text::T("How visible the on-screen buttons are.");
      row.act = [step](int dir) {
        touch::Change([&](touch::Layout& l) {
          l.opacity = step({0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f}, l.opacity, dir);
        });
      };
      rows.push_back(std::move(row));
    }
    {
      Row row;
      row.text = [] { return nier::pc_text::T("Touch Camera Speed"); };
      row.value = [percent] { return percent(touch::Current().camera); };
      row.help = nier::pc_text::T("How fast the camera turns when you drag on the right side of the screen.");
      row.act = [step](int dir) {
        touch::Change([&](touch::Layout& l) {
          l.camera = step({0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f}, l.camera, dir);
        });
      };
      rows.push_back(std::move(row));
    }
    {
      Row row;
      row.text = [] { return nier::pc_text::T("Edit Button Layout"); };
      row.help = nier::pc_text::T("Drag the buttons to new places and change their size.");
      row.act = [](int dir) {
        if (dir == 0) touch::state().editing = true;
      };
      rows.push_back(std::move(row));
    }
    {
      Row row;
      row.text = [] { return nier::pc_text::T("Restore Defaults"); };
      row.help = nier::pc_text::T("Puts the on-screen buttons back as they were.");
      row.act = [](int dir) {
        if (dir == 0) touch::RestoreDefaults();
      };
      rows.push_back(std::move(row));
    }
    back();
    } else {
    // The keyboard and mouse (keyboard_binds.h).
    for (const auto& o : nier::MouseOptions()) rows.push_back(OptionRow(o));
    const auto& binds = nier::KeyBindRows();
    for (size_t i = 0; i < binds.size(); ++i) {
      const std::string label = i < std::size(kEnBindLabels) ? nier::pc_text::T(kEnBindLabels[i]) : AsciiOnly(binds[i].label);
      const std::string cvar = binds[i].cvar;
      Row row;
      row.text = [label] { return label; };
      row.value = [i, cvar] {
        if (g_pages.capturing == int(i)) return nier::pc_text::T("Press a key...");
        return KeyEn(nier::PrimaryKey(rex::cvar::GetFlagByName(cvar)));
      };
      row.help = nier::pc_text::T("Press A, then the new key. Esc cancels.");
      row.act = [i](int dir) {
        if (dir != 0) return;
        g_pages.capturing = int(i);
        nier::BeginKeyCapture();
        nier::pc_options_state().native_capturing = true;
      };
      rows.push_back(std::move(row));
    }
    Row defaults;
    defaults.text = [] { return nier::pc_text::T("Restore Defaults"); };
    defaults.help = nier::pc_text::T("Puts every key back to the default.");
    defaults.act = [](int dir) {
      if (dir != 0) return;
      for (const auto& b : nier::KeyBindRows()) rex::cvar::SetFlagByName(b.cvar, b.default_keys);
    };
    rows.push_back(std::move(defaults));
    back();
    }
  } else if (page == 3) {
    for (const nier::text::Language& l : nier::text::Languages()) {
      const std::string name = l.name;
      if (name == nier::text::CurrentName()) *selected = uint32_t(rows.size());
      Row row;
      row.text = [name] {
        return AsciiOnly(name) + (nier::text::CurrentName() == name ? nier::pc_text::T("  (in use)") : std::string());
      };
      row.help = nier::pc_text::T("Changes the language of the game text, right now.");
      row.act = [name](int dir) {
        if (dir == 0) nier::text::Select(name);
      };
      rows.push_back(std::move(row));
    }
  } else if (page == 4) {
    // Achievements: the score, then each one (name, points or Locked; how it is earned in the help).
    // The list holds 23 rows, so the 41 come in two halves, turned by the last row.
    const auto list = nier::achievements::List();
    int got = 0, points = 0, total = 0;
    for (const auto& a : list) {
      total += int(a.points);
      if (a.unlocked) ++got, points += int(a.points);
    }
    constexpr int kPerPart = 21;
    const int parts = std::max(1, (int(list.size()) + kPerPart - 1) / kPerPart);
    const int part = std::clamp(g_pages.ach_part, 0, parts - 1);
    // Action rows (no value column: a value row has the arrows of a setting); the status is in the
    // row's own text.
    {
      Row row;
      const std::string head = fmt::format("{} {}/{}   {} {}/{}", nier::pc_text::T("Unlocked"), got, list.size(),
                                           nier::pc_text::T("Points"), points, total);
      row.text = [head] { return head; };
      row.help = nier::pc_text::T("Achievements unlocked on this PC, in every save.");
      rows.push_back(std::move(row));
    }
    for (int i = part * kPerPart; i < std::min(int(list.size()), (part + 1) * kPerPart); ++i) {
      const auto& a = list[size_t(i)];
      Row row;
      const std::string name = fmt::format("{}   ({})", nier::pc_text::Achievement(a.id, 0, a.name),
                                           a.unlocked ? fmt::format("{} {}", a.points, nier::pc_text::T("pts"))
                                                      : nier::pc_text::T("Locked"));
      row.text = [name] { return name; };
      row.help = a.unlocked ? fmt::format("{} ({} {})", nier::pc_text::Achievement(a.id, 1, a.description),
                                          nier::pc_text::T("unlocked on"), a.when)
                            : nier::pc_text::Achievement(a.id, 2, a.locked);
      rows.push_back(std::move(row));
    }
    if (parts > 1) {
      Row row;
      const bool last = part == parts - 1;
      row.text = [last] { return nier::pc_text::T(last ? "Previous Page" : "Next Page"); };
      row.help = nier::pc_text::T("Shows the other achievements.");
      row.act = [parts](int dir) {
        g_pages.ach_part = (g_pages.ach_part + (dir < 0 ? parts - 1 : 1)) % parts;
        g_pages.refill = true;
      };
      rows.push_back(std::move(row));
    }
  }
  if (rows.size() > kMaxBItems - 1) rows.resize(kMaxBItems - 1);
  return rows;
}

// A bar row's value, lowest and highest (bytes 1-3): {index, count} of the option's values.
// (The bar is drawn the same size whatever the range: 11 marks, filled in proportion.)
void SetBarLevel(uint8_t* item, std::pair<int, int> level) {
  item[1] = uint8_t(level.first);
  item[2] = 0;
  item[3] = uint8_t(std::max(level.second - 1, 1));
}

// Has the game draw the settings list again from the items in its memory.
void RedrawSettings(PPCContext& ctx, uint8_t* base, uint32_t obj_addr) {
  PPCContext saved = ctx;
  ctx.r3.u64 = obj_addr;
  ctx.r4.u64 = g_pages.state;
  __imp__sub_824197A8(ctx, base);
  ctx = saved;
}

// Writes the page's rows (BuildPage) and the Return row into the list; returns the rows written
// without Return. The cursor goes to the page's own row (BuildPage's selected).
size_t WritePage(uint8_t* obj, int page) {
  uint32_t selected = 0;
  std::vector<Row> rows = BuildPage(page, &selected);
  const size_t n = std::min(rows.size(), size_t(kMaxBItems - 1));
  rows.resize(n);
  uint8_t* items = obj + kOffBItems;
  std::memset(items, 0, kMaxBItems * kItemSize);
  for (size_t i = 0; i < n; ++i) {
    uint8_t* item = items + i * kItemSize;
    const bool bar = rows[i].level && g_pages.have_bar;
    const bool plain = rows[i].value && !rows[i].act && g_pages.have_plain;
    std::memcpy(item, bar ? g_pages.t_bar : plain ? g_pages.t_plain : rows[i].value ? g_pages.t_choice : g_pages.t_action,
                kItemSize);
    if (bar) SetBarLevel(item, rows[i].level());
    SetBe32(item + kItemId, kRowIdBase + uint32_t(i));
    SetBe32(item + kItemLabel, kRowLabelBase + uint32_t(i));
    SetBe32(item + kItemHelp, kRowHelpBase + uint32_t(i));
  }
  std::memcpy(items + n * kItemSize, g_pages.t_back, kItemSize);
  rows.push_back(Row{.back = true});
  SetBe32(obj + kOffBCount, uint32_t(n + 1));
  SetBe32(obj + kOffBScroll, selected < 9 ? 0 : selected - 8);
  SetBe32(obj + kOffBSelected, selected < 9 ? selected : 8);
  std::lock_guard lock(g_pages.mutex);
  g_pages.rows = std::move(rows);
  return n;
}

// The game has just built its Game Settings list: put the pending page in it.
void FillSettingsList(PPCContext& ctx, uint8_t* base, uint32_t obj_addr) {
  uint8_t* obj = base + obj_addr;
  const uint32_t count = Be32(obj + kOffBCount);
  if (count < 2 || count > kMaxBItems) return;
  const int page = g_pages.pending;
  g_pages.pending = 0;
  uint8_t choice[kItemSize], action[kItemSize], back[kItemSize], bar[kItemSize];
  bool have_choice = false, have_action = false, have_bar = false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* item = obj + kOffBItems + i * kItemSize;
    if ((item[kItemKind] == 5 || item[kItemKind] == 6) && !have_bar) {  // a volume's bar
      std::memcpy(bar, item, kItemSize);
      have_bar = true;
    }
    if (item[kItemKind] == kKindChoice && !have_choice) {
      std::memcpy(choice, item, kItemSize);
      have_choice = true;
    } else if (item[kItemKind] == kKindAction && Be32(item + kItemId) != 0x16 && !have_action) {
      std::memcpy(action, item, kItemSize);
      have_action = true;
    }
  }
  // Return: the last row on the title's settings list, the first one on the pause menu's.
  uint32_t back_at = count - 1;
  for (uint32_t i = 0; i < count; ++i) {
    if (Be32(obj + kOffBItems + i * kItemSize + kItemId) == kReturnId) back_at = i;
  }
  std::memcpy(back, obj + kOffBItems + back_at * kItemSize, kItemSize);
  if (!have_choice || !have_action || back[kItemKind] != kKindAction) {
    REXLOG_WARN("Menu: lista de ajustes inesperada; pagina {} nao aberta", page);
    return;
  }

  uint8_t* items = obj + kOffBItems;
  g_pages.saved_obj = obj_addr;
  g_pages.saved_count = count;
  g_pages.saved_scroll = Be32(obj + kOffBScroll);
  g_pages.saved_selected = Be32(obj + kOffBSelected);
  g_pages.saved_items.assign(items, items + kMaxBItems * kItemSize);
  // The row kinds, kept for refilling the page (the achievements' two halves).
  std::memcpy(g_pages.t_choice, choice, kItemSize);
  std::memcpy(g_pages.t_action, action, kItemSize);
  std::memcpy(g_pages.t_back, back, kItemSize);
  std::memcpy(g_pages.t_bar, bar, kItemSize);
  g_pages.have_bar = have_bar;
  g_pages.have_plain = false;
  for (uint32_t i = 0; i < count; ++i) {  // a value without arrows (the game's kind 3)
    const uint8_t* item = items + i * kItemSize;
    if (item[kItemKind] == 3) {
      std::memcpy(g_pages.t_plain, item, kItemSize);
      g_pages.have_plain = true;
      break;
    }
  }
  g_pages.ach_part = 0;
  const size_t n = WritePage(obj, page);
  {
    std::lock_guard lock(g_pages.mutex);
    g_pages.page = page;
  }
  g_pages.capturing = -1;
  g_pages.entered = false;
  g_pages.waiting = 0;
  g_pages.screen = base[0x82757AC5];
  nier::pc_options_state().native_page_pause = g_pages.state == kPauseState;
  nier::pc_options_state().native_page = page;
  REXLOG_INFO("Menu: pagina {} aberta na lista de ajustes do jogo ({} itens)", page, n + 1);
  RedrawSettings(ctx, base, obj_addr);
}

void ClosePage() {
  auto& state = nier::pc_options_state();
  if (!state.config_path.empty()) nier::SavePcSettings(state.config_path);
  {
    std::lock_guard lock(g_pages.mutex);
    g_pages.page = 0;
    g_pages.rows.clear();
  }
  g_pages.capturing = -1;
  g_pages.capture_hotkey = -1;
  g_pages.pad_capturing = -1;
  nier::pad_remap::CancelCapture();
  state.native_capturing = false;
  state.native_page = 0;
  state.native_page_pause = false;
  state.native_events = 0;
}

// Puts the game's own rows back into its settings list (g_pages.saved_*), if that object is still
// the Options one.
void RestoreGameList(uint8_t* base, uint8_t* obj) {
  const uint32_t at = g_pages.saved_obj;
  g_pages.saved_obj = 0;
  if (!at || !obj || obj != base + at || g_pages.saved_items.size() != kMaxBItems * kItemSize) return;
  std::memcpy(obj + kOffBItems, g_pages.saved_items.data(), g_pages.saved_items.size());
  SetBe32(obj + kOffBCount, g_pages.saved_count);
  SetBe32(obj + kOffBScroll, g_pages.saved_scroll);
  SetBe32(obj + kOffBSelected, g_pages.saved_selected);
  REXLOG_INFO("Menu: lista de ajustes do jogo devolvida ({} itens)", g_pages.saved_count);
}

// From a page of ours in the pause menu's Options back to that list (Return, or B there).
void BackToPauseList(PPCContext& ctx, uint8_t* base, uint32_t obj_addr) {
  uint8_t* obj = base + obj_addr;
  ClosePage();
  // The pause list as it was when the page opened (FillSettingsList kept it): put back as it is.
  // Building it again made the game place its drawn cursor on the original list, where row 1 is a
  // blank it skips: back from Video (row 1) the cursor showed on Controls while A acted on Video.
  const bool kept = g_pages.saved_obj == obj_addr && g_pages.saved_items.size() == kMaxBItems * kItemSize;
  g_pages.saved_obj = 0;
  if (kept) {
    std::memcpy(obj + kOffBItems, g_pages.saved_items.data(), g_pages.saved_items.size());
    SetBe32(obj + kOffBCount, g_pages.saved_count);
    RewritePauseList(obj);  // the game had built its own rows again just before the page opened
  } else {
    PPCContext saved = ctx;
    ctx.r3.u64 = obj_addr;
    ctx.r4.u64 = kPauseState;
    __imp__sub_8241A188(ctx, base);
    ctx = saved;
    RewritePauseList(obj);
  }
  const uint32_t count = Be32(obj + kOffBCount);
  const bool fits = g_pages.return_scroll + g_pages.return_selected < count;
  SetBe32(obj + kOffBScroll, fits ? g_pages.return_scroll : 0);
  SetBe32(obj + kOffBSelected, fits ? g_pages.return_selected : 0);
  RedrawSettings(ctx, base, obj_addr);
  nier::pc_options_state().pause_on_screen = true;
  nier::pc_options_state().pause_dialog = 0;
}

}  // namespace

// The game builds the list of a state (our page rides on the Game Settings one).
REX_HOOK_RAW(sub_8241A188) {
  const uint32_t obj_addr = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_8241A188(ctx, base);
  {  // TEMP (MENUDBG): NIER_MSGDBG logs every list build.
    static const bool dbg = std::getenv("NIER_MSGDBG") != nullptr;
    if (dbg) {
      const uint8_t* obj = base + obj_addr;
      const uint32_t n = Be32(obj + kOffBCount);
      std::string items;
      for (uint32_t i = 0; i < n && i < kMaxBItems; ++i) {
        const uint8_t* it = obj + kOffBItems + i * kItemSize;
        items += fmt::format(" [{:02X} id={:X} l={:X}]", it[kItemKind], Be32(it + kItemId), Be32(it + kItemLabel));
      }
      REXLOG_INFO("MENUDBG build obj={:08X} state={} type={} curstate={} count={}{}", obj_addr, state,
                  Be32(obj + kOffMenuType), Be32(obj + kOffState), n, items);
    }
  }
  CacheMenuNodes(ctx, base, obj_addr);
  if (g_pages.pending && state == g_pages.state) {
    FillSettingsList(ctx, base, obj_addr);
  } else if (state == kSettingsState && DropSelectDevice(base + obj_addr)) {
    // Title Options > Game Settings: Select Storage Device does nothing on a PC.
    PPCContext saved = ctx;
    ctx.r3.u64 = obj_addr;
    ctx.r4.u64 = kSettingsState;
    __imp__sub_824197A8(ctx, base);  // the game laid the old rows out while building
    ctx = saved;
    REXLOG_INFO("Menu: Selecionar Dispositivo removido de Game Settings");
  } else if (state == kPauseState && Be32(base + obj_addr + kOffMenuType) == kPauseMenuType) {
    RewritePauseList(base + obj_addr);
    {
      PPCContext saved = ctx;
      ctx.r3.u64 = obj_addr;
      ctx.r4.u64 = kPauseState;
      __imp__sub_824197A8(ctx, base);  // the game laid the old rows out while building
      ctx = saved;
    }
    REXLOG_INFO("Menu: opcoes do pause com as linhas do PC ({} itens)", Be32(base + obj_addr + kOffBCount));
    auto& s = nier::pc_options_state();
    s.game_options_menu = base + obj_addr;
    s.game_options_on_screen = true;
    s.pause_on_screen = true;
    s.pause_dialog = 0;
  }
}

// Value text of an item; rows of ours answer with their current value.
REX_HOOK_RAW(sub_82416C20) {
  const uint32_t id = Be32(base + ctx.r3.u32 + kItemId);
  if (id >= kRowIdBase && id < kRowIdBase + 0x100) {
    std::string text;
    bool found = false;
    {
      std::lock_guard lock(g_pages.mutex);
      const size_t index = id - kRowIdBase;
      if (index < g_pages.rows.size() && g_pages.rows[index].value) {
        text = g_pages.rows[index].value();
        found = true;
      }
    }
    if (found) {
      ctx.r3.u64 = GuestString(text);
      return;
    }
  }
  __imp__sub_82416C20(ctx, base);
}

// Once per frame on the game thread (the pad-read hook in game_text_hooks.cpp).
void SetQuitDialogText(uint8_t* base, bool ours);  // game_text_hooks.cpp

void PlayMenuSound(PPCContext& ctx, uint8_t* base, uint32_t which) {
  auto be_float = [base](uint32_t a) {
    const uint32_t v = Be32(base + a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };
  PPCContext saved = ctx;
  ctx.r3.u64 = 0x8447BB90;
  ctx.r4.u64 = which;
  ctx.f1.f64 = be_float(0x820007A4);
  ctx.f2.f64 = be_float(0x820454E0);
  __imp__sub_82112190(ctx, base);
  ctx = saved;
  REXLOG_INFO("Conquista: som {} tocado", which);
}

// Widescreen (2026-11-13), Screen Fit (nier_screen_fit, present.cpp):
//   "wide" ("Widescreen"): the 16:9 frame is stretched over the wider screen; the HUD and the menus are squeezed back
//     to their shape in its middle 16:9 (SqueezeHud). The 3D is stretched a little.
//   "widereal" ("Widescreen real"): the cameras also take the screen's shape (Hor+: the same height, more to the
//     sides, nothing stretched) and the HUD's pieces keep to the screen's edges (left, middle, right thirds).
// A camera's aspect is [camera + 328], copied from the 16:9 at 0x820007E4 when it is made (sub_821026B0); the HUD's
// layouts read it too, so only the projection's making sees the screen's (sub_82102598 below).
namespace renut::gpu {
float OutputAspect();  // present.cpp
}
namespace {
constexpr uint32_t k16x9 = 0x3FE38E39u;
float ScreenAspect() {
  const float a = renut::gpu::OutputAspect();
  return a > 1.78f ? std::min(a, 3.6f) : 0.0f;  // wider than 16:9 only (up to 32:9)
}
// The screen's aspect with a widescreen fit on, else 0.
float WideAspect() {
  const std::string mode = rex::cvar::GetFlagByName("nier_screen_fit");
  return mode == "wide" || mode == "widereal" ? ScreenAspect() : 0.0f;
}
bool WideReal() { return rex::cvar::GetFlagByName("nier_screen_fit") == "widereal"; }

}  // namespace
// sub_82102598(camera r3): the camera's projection from its field of view (+324), aspect (+328) and planes; each
// frame for the view (0x827B8890, from sub_821A66B0), made from cameras holding the 16:9 copied at their making.
// "widereal": the screen's aspect while it is made, then the 16:9 back (the HUD and menus read the field too and
// must keep 16:9). A camera of another aspect (not 16:9) is left alone.
extern "C" void __imp__sub_82102598(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_82102598) {
  const uint32_t field = ctx.r3.u32 + 328;
  const float a = WideReal() ? ScreenAspect() : 0.0f;
  const bool swap = a > 1.78f && Be32(base + field) == k16x9;
  if (swap) {
    uint32_t bits;
    std::memcpy(&bits, &a, 4);
    SetBe32(base + field, bits);
  }
  __imp__sub_82102598(ctx, base);
  if (swap) SetBe32(base + field, k16x9);
}

// sub_82590CC0(list r3, context r4): the 2D batch drawer (the HUD's sprites, a quad a call). Its list (first node at
// +24, end marker at +32; node: next at +0, entry at +8) holds entries of type [e+28]: 1 = vertices from [e+16] to
// [e+24], 24 bytes each (x, y, z in clip space first), copied into a BeginVertices rect list; 2 and 3 set state.
// Widescreen: the present stretches the 16:9 frame over the wider screen, so the HUD's x is squeezed by k = 16:9 / the
// screen's aspect while the vertices are copied, then put back. "wide": around the centre (the HUD keeps its shape in
// the middle 16:9 of the screen). "widereal", in gameplay: the pieces are gathered in groups (a row's pieces close
// together); a small group near an edge goes to that edge, the rest keeps to the centre. Quads across the whole
// width (fades, filters) are left whole.
static void SqueezeHud(uint8_t* base, uint32_t list, float k, bool anchored, uint32_t stride, bool typed,
                       std::vector<std::pair<uint32_t, uint32_t>>& saved) {
  const uint32_t end = Be32(base + list + 32);
  auto x_at = [&](uint32_t v) {
    const uint32_t w = Be32(base + v);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
  };
  // A quad list (D3DPT_QUADLIST, 4 vertices a quad): each quad on its own, as an entry can hold a backdrop with
  // the boxes over it. Quads across the whole width (fades, filters) are left whole.
  struct Piece {
    uint32_t v0, v1, group;
    float lo = 1e9f, hi = -1e9f, ylo = 1e9f, yhi = -1e9f;
  };
  thread_local std::vector<Piece> pieces;
  pieces.clear();
  auto whole_width = [&](uint32_t q, uint32_t group) {
    float lo = 1e9f, hi = -1e9f;
    for (uint32_t v = q; v < q + stride * group; v += stride) lo = std::min(lo, x_at(v)), hi = std::max(hi, x_at(v));
    return lo <= -0.98f && hi >= 0.98f;
  };
  int guard = 0;
  for (uint32_t node = Be32(base + list + 24); node && guard < 4096; node = Be32(base + node), ++guard) {
    const uint32_t e = Be32(base + node + 8);
    if (e == end) break;
    if (typed && Be32(base + e + 28) != 1) continue;
    const uint32_t v0 = Be32(base + e + 16), v1 = Be32(base + e + 24);
    if (v1 <= v0 || (v1 - v0) % stride || v1 - v0 > stride * 8192) continue;
    const uint32_t count = (v1 - v0) / stride;
    pieces.push_back({v0, v1, count % 4 == 0 ? 4u : count});
  }
  if (pieces.empty()) return;
  // Each piece's box (its quads that are not whole-width).
  for (Piece& p : pieces) {
    for (uint32_t q = p.v0; q < p.v1; q += stride * p.group) {
      float lo = 1e9f, hi = -1e9f;
      for (uint32_t v = q; v < q + stride * p.group; v += stride) lo = std::min(lo, x_at(v)), hi = std::max(hi, x_at(v));
      if (lo <= -0.98f && hi >= 0.98f) continue;
      for (uint32_t v = q; v < q + stride * p.group; v += stride) {
        const uint32_t yw = Be32(base + v + 4);
        float y;
        std::memcpy(&y, &yw, 4);
        p.ylo = std::min(p.ylo, y), p.yhi = std::max(p.yhi, y);
      }
      p.lo = std::min(p.lo, lo), p.hi = std::max(p.hi, hi);
    }
  }
  // 2026-11-19 (the Poco, 20:9): pieces anchored one by one parted what belongs together: a line of text drawn in
  // runs (Yonah's diary: half at each edge), a boss's bar in caps and a middle, a subtitle and its plate. Pieces of
  // a row (their heights overlapping) closer than a gap are one group, and the group is placed as a whole: a wide
  // one (a line, a bar) around the centre, a small one near an edge to that edge.
  const size_t n = pieces.size();
  std::vector<size_t> root(n);
  for (size_t i = 0; i < n; ++i) root[i] = i;
  auto find = [&](size_t i) {
    while (root[i] != i) i = root[i] = root[root[i]];
    return i;
  };
  for (size_t i = 0; i < n; ++i) {
    const Piece& a = pieces[i];
    if (a.lo > a.hi) continue;
    for (size_t j = i + 1; j < n; ++j) {
      const Piece& b = pieces[j];
      if (b.lo > b.hi) continue;
      const float pad = 0.02f;
      const bool rows = a.ylo <= b.yhi + pad && b.ylo <= a.yhi + pad;
      const bool close_x = a.lo <= b.hi + 0.12f && b.lo <= a.hi + 0.12f;
      if (rows && close_x) root[find(i)] = find(j);
    }
  }
  std::vector<float> glo(n, 1e9f), ghi(n, -1e9f), gylo(n, 1e9f), gyhi(n, -1e9f);
  for (size_t i = 0; i < n; ++i) {
    const size_t r = find(i);
    glo[r] = std::min(glo[r], pieces[i].lo), ghi[r] = std::max(ghi[r], pieces[i].hi);
    if (pieces[i].lo <= pieces[i].hi)
      gylo[r] = std::min(gylo[r], pieces[i].ylo), gyhi[r] = std::max(gyhi[r], pieces[i].yhi);
  }
  // 2026-11-20 (the Poco: the fishing / boss bar's caps parted from it): an object drawn in several calls (caps, middle)
  // was grouped per call only. The groups of all the calls of the last frame, joined by the same rule, give each group
  // of this call the bounds of the whole thing it belongs to (the HUD stays from frame to frame).
  if (anchored) {
    struct Box {
      float lo, hi, ylo, yhi;
    };
    static std::mutex mutex;
    static std::vector<Box> cur, prev;
    static uint32_t frame = 0;
    std::lock_guard lock(mutex);
    const uint32_t now = nier::menu_mouse::game_frame().load();
    if (now != frame) {
      frame = now;
      // Join last frame's boxes (rows overlapping, closer than the gap), as the groups above.
      std::vector<size_t> rt(cur.size());
      for (size_t i = 0; i < rt.size(); ++i) rt[i] = i;
      auto fr = [&](size_t i) {
        while (rt[i] != i) i = rt[i] = rt[rt[i]];
        return i;
      };
      for (size_t i = 0; i < cur.size(); ++i)
        for (size_t j = i + 1; j < cur.size(); ++j) {
          const Box& a = cur[i];
          const Box& b = cur[j];
          if (a.ylo <= b.yhi + 0.02f && b.ylo <= a.yhi + 0.02f && a.lo <= b.hi + 0.12f && b.lo <= a.hi + 0.12f)
            rt[fr(i)] = fr(j);
        }
      std::vector<Box> merged(cur.size(), Box{1e9f, -1e9f, 1e9f, -1e9f});
      for (size_t i = 0; i < cur.size(); ++i) {
        Box& m = merged[fr(i)];
        m.lo = std::min(m.lo, cur[i].lo), m.hi = std::max(m.hi, cur[i].hi);
        m.ylo = std::min(m.ylo, cur[i].ylo), m.yhi = std::max(m.yhi, cur[i].yhi);
      }
      prev.clear();
      for (const Box& m : merged)
        if (m.lo <= m.hi) prev.push_back(m);
      cur.clear();
    }
    for (size_t r = 0; r < n; ++r) {
      if (find(r) != r || glo[r] > ghi[r]) continue;
      if (cur.size() < 4096) cur.push_back({glo[r], ghi[r], gylo[r], gyhi[r]});
      for (const Box& m : prev)
        if (m.ylo <= gyhi[r] + 0.02f && gylo[r] <= m.yhi + 0.02f && m.lo <= ghi[r] + 0.001f && glo[r] <= m.hi + 0.001f) {
          glo[r] = std::min(glo[r], m.lo), ghi[r] = std::max(ghi[r], m.hi);
        }
    }
  }
  for (size_t i = 0; i < n; ++i) {
    const Piece& p = pieces[i];
    const size_t r = find(i);
    // x' = x k + shift. Not anchored (menus): around the centre (shift 0).
    float shift = 0.0f;
    if (anchored && glo[r] <= ghi[r]) {
      const float lo = glo[r], hi = ghi[r], cx = (lo + hi) * 0.5f;
      // 2026-11-19 (2nd try): centred groups (the whole line around the middle) stay centred; a small group at the
      // right edge (the map, the buttons) keeps its right edge; everything else keeps its LEFT edge where the
      // stretched frame puts it (lines of text start where they did over the stretched page, a bar over an enemy
      // stays over it, the left HUD stays at the left edge).
      if (std::fabs(cx) < 0.06f) shift = 0.0f;
      // The right's groups all keep to the screen's right edge (x 1), not each to its own: the life / magic bars are
      // drawn in pieces of other lengths (a fill shorter than its frame) and parted, each kept its own end
      // (2026-11-19, the Poco).
      else if (cx > 0.45f && hi - lo < 0.6f) shift = 1.0f - k;  // keeps the right edge's distance
      else shift = lo * (1.0f - k);                                      // keeps lo
    }
    for (uint32_t q = p.v0; q < p.v1; q += stride * p.group) {
      if (whole_width(q, p.group)) continue;
      for (uint32_t v = q; v < q + stride * p.group; v += stride) {
        const uint32_t w = Be32(base + v);
        saved.emplace_back(v, w);
        const float x = x_at(v) * k + shift;
        uint32_t out;
        std::memcpy(&out, &x, 4);
        SetBe32(base + v, out);
      }
    }
  }
}
static void SqueezeHudCall(PPCContext& ctx, uint8_t* base, uint32_t stride, bool typed,
                           void (*original)(PPCContext&, uint8_t*)) {
  thread_local std::vector<std::pair<uint32_t, uint32_t>> saved;
  saved.clear();
  if (const float wide = WideAspect(); wide > 1.78f)
    // The edges in gameplay only: a menu's labels and values in different thirds would part.
    SqueezeHud(base, ctx.r3.u32, (16.0f / 9.0f) / wide, WideReal() && !nier::pc_options_state().GameInMenu(), stride,
               typed, saved);
  original(ctx, base);
  for (auto it = saved.rbegin(); it != saved.rend(); ++it) SetBe32(base + it->first, it->second);
}
extern "C" void __imp__sub_82590CC0(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_82590CC0) { SqueezeHudCall(ctx, base, 24, true, __imp__sub_82590CC0); }
// sub_82590850(list r3, context r4): its sibling for text and the menus' sprites, the same list with untyped entries
// of 28-byte vertices.
extern "C" void __imp__sub_82590850(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_82590850) { SqueezeHudCall(ctx, base, 28, false, __imp__sub_82590850); }

std::atomic<uint64_t> g_shop_list_ms{0};  // a shop's list took input then (it takes the drag's steps)

void NativeMenuTick(PPCContext& ctx, uint8_t* base) {
  // The mouse wheel's steps (menu_mouse.h): the settings list scrolls a row a step, its cursor staying on the
  // same row of the screen, laid out again as the game does after a change. Not while a shop's list runs: this tick
  // took its drags first and dropped them (2026-11-19).
  if (const int steps = nier::menu_mouse::NowMs() - g_shop_list_ms.load() < 300
                            ? 0
                            : nier::menu_mouse::nodes().scroll_request.exchange(0)) {
    const uint32_t obj_addr = nier::menu_mouse::nodes().obj.load();
    uint8_t* obj = obj_addr ? base + obj_addr : nullptr;
    const uint32_t list_state = obj ? Be32(obj + kOffState) : 0;
    // Only on the list on screen now: the pause's Options object keeps its state after it closes, and laying
    // that out again read 0x834 (crash 2026-11-13: Tab's item menu after the pause's Options). Drawn in the
    // last frames (sub_82419518), with a list menu open (0x82757AB1).
    // sub_82419518 runs when the list is laid out, not every frame: on the PC the moving mouse kept laying it
    // out, a still finger does not (the touch's drags were all dropped, 2026-11-13). Also on screen: one of our
    // pages open in it, or the pause's Options list (pause_on_screen, cleared when it is left).
    auto& opts = nier::pc_options_state();
    // 2026-11-20 (Android): the title's own Options pages (Game Settings...) did not scroll with a drag: still, the list
    // is not laid out again (drawn_ms old) and it is none of ours nor the pause's. Its cursor's being the newest
    // screen's (the tracker) counts too.
    const bool showing = base[0x82757AB1] != 0 &&
                         (nier::menu_mouse::NowMs() - nier::menu_mouse::nodes().drawn_ms.load() < 100 ||
                          nier::menu_mouse::tracker().ListOnTop() ||
                          (g_pages.page != 0 && opts.native_page.load() != 0) ||
                          (opts.PauseListShowing() && opts.game_options_menu.load() == obj));
    if (obj && showing && (list_state == 4 || list_state == 7)) {
      const int count = int(Be32(obj + kOffBCount));
      const int scroll = int(Be32(obj + kOffBScroll));
      const int top = std::max(0, count - 9);  // 9 rows shown
      const int next = std::clamp(scroll + steps, 0, top);
      if (next != scroll) {
        SetBe32(obj + kOffBScroll, uint32_t(next));
        const int sel = int(Be32(obj + kOffBSelected));
        if (next + sel >= count) SetBe32(obj + kOffBSelected, uint32_t(std::max(0, count - 1 - next)));
        PPCContext saved = ctx;
        ctx.r3.u64 = obj_addr;
        ctx.r4.u64 = list_state;
        __imp__sub_824197A8(ctx, base);
        // The side scroll bar (the page's +2804): sub_82404458(page, first row shown) moves its thumb.
        ctx.r3.u64 = obj_addr;
        ctx.r4.u64 = uint32_t(next);
        __imp__sub_82404458(ctx, base);
        ctx = saved;
      }
    }
  }
  if (nier::achievements::TakeSound()) {  // an achievement unlocked: the game's menu sound
    static const int which = std::getenv("NIER_ACH_SOUND") ? std::atoi(std::getenv("NIER_ACH_SOUND")) : 5;
    PlayMenuSound(ctx, base, uint32_t(which));
  }
  auto& state = nier::pc_options_state();
  {  // The game's Quit Game dialog asks our question while the cursor is on Quit to Desktop.
    const bool on_quit = state.quit_desktop_armed ||
                         (!g_pages.page && state.PauseListShowing() &&
                          state.PauseSelectedLabel() == nier::PcOptionsState::kQuitLabelId);
    SetQuitDialogText(base, on_quit);
  }
  {  // TEXT\EXAMPLE\PC_TEXT.TXD, once the game (and its achievements) is up.
    static int frames = 0;
    if (++frames == 600) nier::pc_text::WriteExample();
  }
  uint8_t* obj = state.game_options_menu.load();
  int request = state.native_open_request.exchange(0);
  const uint32_t events = state.native_events.exchange(0);
  {  // TEMP (dev): NIER_NATIVE_DEMO=<seconds>|<page> opens a page once the Options list shows.
    static const char* env = std::getenv("NIER_NATIVE_DEMO");
    static bool done = false;
    static std::chrono::steady_clock::time_point shown;
    if (env && !done && state.game_options_on_screen) {
      if (shown.time_since_epoch().count() == 0) shown = std::chrono::steady_clock::now();
      if (std::chrono::steady_clock::now() - shown > std::chrono::seconds(std::atoi(env))) {
        done = true;
        if (const char* bar = std::strchr(env, '|')) request = std::atoi(bar + 1);
      }
    }
  }
  if (!g_pages.page) {
    const bool pause_list = state.PauseListShowing();
    if (request == 4) {
      REXLOG_INFO("Menu: Fechar o jogo");
      if (!state.config_path.empty()) nier::SavePcSettings(state.config_path);
      if (state.quit_game) {
        state.quit_game();
      } else {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
      }
      return;
    }
    // 1 Video, 2 Controls, 3 Language; 5 = the title's Achievements row (page 4).
    if (request == 5 && pause_list) request = 0;
    if (request == 6 && !pause_list) request = 0;  // 6 = the pause's Mods row (page 5)
    if (((request >= 1 && request <= 3) || request == 5 || request == 6) && obj &&
        (state.game_options_on_screen || pause_list)) {
      const bool pause = pause_list;
      g_pages.state = pause ? kPauseState : kSettingsState;
      g_pages.return_scroll = Be32(obj + kOffBScroll);
      g_pages.return_selected = Be32(obj + kOffBSelected);
      g_pages.pending = request == 5 ? 4 : request == 6 ? 5 : request;
      PPCContext saved = ctx;
      if (pause) {
        // Already in that state: the page goes straight over the list on screen. The game's step
        // built its own list first, where row 1 (Video Options in ours) is a blank its drawn cursor
        // skips: the white cursor then sat one row below the selection, there and back (2026-11-12).
        FillSettingsList(ctx, base, uint32_t(obj - base));
      } else {
        ctx.r3.u64 = uint32_t(obj - base);
        ctx.r4.u64 = g_pages.state;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        __imp__sub_8241B908(ctx, base);  // the game's own step into that settings list
        ctx = saved;
      }
      if (g_pages.pending && pause) {  // already in that state: build the list in place
        ctx.r3.u64 = uint32_t(obj - base);
        ctx.r4.u64 = g_pages.state;
        __imp__sub_8241A188(ctx, base);
        ctx = saved;
        if (g_pages.pending) FillSettingsList(ctx, base, uint32_t(obj - base));
      }
      if (g_pages.pending) {
        REXLOG_WARN("Menu: o jogo nao montou a lista de ajustes; pagina {} nao aberta", request);
        g_pages.pending = 0;
      }
    }
    return;
  }
  {  // The state turns to the settings one when its transition ends; leaving it later is B or Return.
    const bool in_state = obj && Be32(obj + kOffState) == g_pages.state;
    if (in_state) g_pages.entered = true;
    // The pause menu's object keeps its state after the pause closes (Start / Esc there leaves the
    // whole pause menu): back in gameplay the page is gone too, or left/right (the arrow keys as
    // the camera) redrew a closed list and the game crashed reading 0x834 (2026-11-12).
    // Another screen of the pause menu (LB/RB to the book's Weapons, ...) leaves the page as well.
    // The pause's cross on screen (its window manager runs with the cross on top) means the Options list
    // is gone too, whatever its object says: a page left "open" took the left the mouse sends to the cross's
    // Options arm and redrew a closed list (crash 0x834, 2026-11-13).
    // The cross counts only after 4 checks in a row (2026-11-23, the player: turning V-Sync off rebuilt the swapchain,
    // one frame's order of events looked like the cross on top, and the page closed under the open list: the game's
    // own rows with <NoText>, left / right acting on them).
    static int cross_checks = 0;
    cross_checks = nier::menu_mouse::tracker().CrossOnTop(nier::menu_mouse::NowMs()) ? cross_checks + 1 : 0;
    const bool pause_closed = g_pages.state == kPauseState &&
                              (!state.GameInMenu() || base[0x82757AC5] != g_pages.screen || cross_checks >= 4);
    if (pause_closed) {
      cross_checks = 0;
      REXLOG_INFO("Menu: pagina {} fechada com o pause (tela {:02X} -> {:02X}, menu {})", g_pages.page,
                  g_pages.screen, base[0x82757AC5], state.GameInMenu());
      g_pages.saved_obj = 0;
      state.pause_on_screen = false;
    }
    if (!obj || pause_closed || (g_pages.entered && !in_state) ||
        (!g_pages.entered && ++g_pages.waiting > 600)) {
      ClosePage();
      RestoreGameList(base, obj);
      return;
    }
  }
  const uint32_t obj_addr = uint32_t(obj - base);
  if (g_pages.capture_hotkey >= 0) {  // Nier Plus: a mod window's new key
    const std::string key = nier::PollCapturedKey();
    if (!key.empty()) {
      if (key != "Escape") nier::mods::SetHotkey(g_pages.capture_hotkey, key);
      g_pages.capture_hotkey = -1;
      state.native_capturing = false;
      RedrawSettings(ctx, base, obj_addr);
    }
    return;
  }
  if (g_pages.pad_capturing >= 0) {  // Controller page: the action's new button
    const int got = nier::pad_remap::TakeCaptured();
    const bool cancel = nier::PollCapturedKey() == "Escape" || nier::menu_mouse::NowMs() - g_pages.pad_capture_ms > 8000;
    if (got >= 0 || cancel) {
      if (got >= 0) nier::pad_remap::Assign(g_pages.pad_capturing, got);
      nier::pad_remap::CancelCapture();
      g_pages.pad_capturing = -1;
      state.native_capturing = false;
      RedrawSettings(ctx, base, obj_addr);
    }
    return;
  }
  if (g_pages.capturing >= 0) {
    const std::string key = nier::PollCapturedKey();
    if (key == "Escape" || !key.empty()) {
      if (key != "Escape") AssignKey(size_t(g_pages.capturing), key);
      g_pages.capturing = -1;
      state.native_capturing = false;
      RedrawSettings(ctx, base, obj_addr);
    }
    return;
  }
  if (!events) return;
  int page_now;
  {
    std::lock_guard lock(g_pages.mutex);
    page_now = g_pages.page;
  }
  // The Control Options' own pages (touch, keyboard and mouse, controller): B and Return go back to their list, not
  // out of Options (2026-11-17, the user).
  const auto back_to_controls = [&] {
    GoToPage(2);
    g_pages.refill = false;
    WritePage(obj, 2);
    RedrawSettings(ctx, base, obj_addr);
  };
  if ((events & nier::PcOptionsState::kEventB) && (page_now == 7 || page_now == 8 || page_now == 9)) {
    back_to_controls();
    return;
  }
  if (events & nier::PcOptionsState::kEventB) {
    if (g_pages.state == kPauseState) {
      BackToPauseList(ctx, base, obj_addr);  // the game didn't get this B (input_recorder.h)
    } else {
      ClosePage();
      RestoreGameList(base, obj);
    }
    return;
  }
  const uint32_t row = Be32(obj + kOffBScroll) + Be32(obj + kOffBSelected);
  Row picked;
  bool back = false;
  {
    std::lock_guard lock(g_pages.mutex);
    if (row >= g_pages.rows.size()) return;
    back = g_pages.rows[row].back;
    if (!back) picked = g_pages.rows[row];
  }
  if (back && (page_now == 7 || page_now == 8 || page_now == 9) && (events & nier::PcOptionsState::kEventA)) {
    back_to_controls();
    return;
  }
  if (back) {
    // Return on a page in the pause menu's Options: back to that list (the game's own Return
    // there would leave Options; the A is ours anyway).
    if (g_pages.state == kPauseState && (events & nier::PcOptionsState::kEventA)) {
      BackToPauseList(ctx, base, obj_addr);
    }
    return;
  }
  if (!picked.act) return;
  if (events & nier::PcOptionsState::kEventA) picked.act(0);
  if (events & nier::PcOptionsState::kEventLeft) picked.act(-1);
  if (events & nier::PcOptionsState::kEventRight) picked.act(+1);
  if (g_pages.refill) {  // the achievements' other half
    g_pages.refill = false;
    WritePage(obj, g_pages.page);
  }
  {  // The bars' levels follow the values.
    std::lock_guard lock(g_pages.mutex);
    for (size_t i = 0; i < g_pages.rows.size() && i < kMaxBItems; ++i) {
      uint8_t* item = obj + kOffBItems + i * kItemSize;
      if (g_pages.rows[i].level && (item[kItemKind] == 5 || item[kItemKind] == 6)) {
        SetBarLevel(item, g_pages.rows[i].level());
      }
    }
  }
  RedrawSettings(ctx, base, obj_addr);
}


// The mouse on the other menus (menu_mouse.h's Tracker): the game's node lookups name the rows, and the two
// ways its menus mark the selected row say where the cursor is.
extern "C" void __imp__sub_82405028(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823F4518(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823F45E8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823F4830(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823F46B8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823F4788(PPCContext& ctx, uint8_t* base);

namespace {
std::string GuestName(const uint8_t* base, uint32_t a) {
  std::string s;
  if (a >= 0x1000 && a < 0xF0000000)
    for (uint32_t i = 0; i < 64 && base[a + i]; ++i) s += char(base[a + i]);
  return s;
}
void SystemCursor(PPCContext& ctx, const char* fn) {
  nier::menu_mouse::tracker().OnSystemEvent();
}
}  // namespace

// 2D layout node by name (r3 = owner, r4 = name): rows of the menus, by name.
REX_HOOK_RAW(sub_823BC8F0) {
  const uint32_t owner = ctx.r3.u32, name_addr = ctx.r4.u32;
  __imp__sub_823BC8F0(ctx, base);
#if NIER_MENU_POINTER
  const uint32_t node = ctx.r3.u32;
  if (node < 0x1000 || node >= 0x20000000) return;
  // Each node once (they stay where they are while their screen lives); the names copied only then.
  static std::mutex m;
  static std::map<uint32_t, uint32_t> known;  // node -> owner
  {
    std::lock_guard lock(m);
    auto it = known.find(node);
    if (it != known.end() && it->second == owner) return;
    if (known.size() > 40000) known.clear();
    known[node] = owner;
  }
  const std::string name = GuestName(base, name_addr);
  {  // TEMP (2026-11-16, the shops on touch): NIER_SHOPLOG logs each node looked up, with where it is drawn
    static const bool log = std::getenv("NIER_SHOPLOG") != nullptr;
    int x = 0, y = 0;
    if (log && nier::menu_mouse::NodePos(base, node, x, y))
      REXLOG_INFO("SHOPLOG no {:08X} dono {:08X} '{}' em {},{} (lr {:08X})", node, owner, name, x, y, uint32_t(ctx.lr));
  }
  if (nier::menu_mouse::Tracker::Wanted(name)) nier::menu_mouse::tracker().OnLookup(owner, name, node);
#endif
}

// Places r4 (a sprite) on page r3's node named r5: the book pages' cursor (and row icons).
REX_HOOK_RAW(sub_82405028) {
#if NIER_MENU_POINTER
  if (ctx.r4.u32 && ctx.r5.u32) {
    const std::string name = GuestName(base, ctx.r5.u32);
    {  // TEMP (SHOPLOG): something placed on a page's row
      static const bool log = std::getenv("NIER_SHOPLOG") != nullptr;
      static std::string last;
      if (log && name != last) {
        last = name;
        REXLOG_INFO("SHOPLOG coloca pagina {:08X} sprite {:08X} em '{}' (lr {:08X})", ctx.r3.u32, ctx.r4.u32, name,
                    uint32_t(ctx.lr));
      }
    }
    // The page's cursor is its +2808 (sub_82405120 & co. place it from there); the rest are row icons.
    const bool is_cursor = ctx.r4.u32 == Be32(base + ctx.r3.u32 + 2808);
    nier::menu_mouse::tracker().OnPlace(ctx.r3.u32, is_cursor, name, nier::menu_mouse::nodes().obj.load());
  }
#endif
  __imp__sub_82405028(ctx, base);
}

// Places page r3's own cursor (its +2808) on its node named r4: Weapons' and Words' lists (and the pages'
// heading icons).
extern "C" void __imp__sub_82404F40(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_82404F40) {
#if NIER_MENU_POINTER
  if (ctx.r4.u32 && Be32(base + ctx.r3.u32 + 2808)) {
    const std::string name = GuestName(base, ctx.r4.u32);
    // On a heading the mouse does nothing (no rows beside it): still the cursor's place.
    nier::menu_mouse::tracker().OnPlace(ctx.r3.u32, true, name, nier::menu_mouse::nodes().obj.load());
  }
#endif
  __imp__sub_82404F40(ctx, base);
}

// The system windows mark their selected row (r4 = window, r5 = the row's plate node).
REX_HOOK_RAW(sub_823F4518) {
  SystemCursor(ctx, "4518");
  __imp__sub_823F4518(ctx, base);
}
REX_HOOK_RAW(sub_823F45E8) {
  SystemCursor(ctx, "45E8");
  __imp__sub_823F45E8(ctx, base);
}
REX_HOOK_RAW(sub_823F4830) {
  SystemCursor(ctx, "4830");
  __imp__sub_823F4830(ctx, base);
}
REX_HOOK_RAW(sub_823F46B8) {
  SystemCursor(ctx, "46B8");
  __imp__sub_823F46B8(ctx, base);
}
REX_HOOK_RAW(sub_823F4788) {
  SystemCursor(ctx, "4788");
  __imp__sub_823F4788(ctx, base);
}

// The system windows' manager (the title's and the pause's cross, New Game / Load, the save lists,
// dialogs) updates the window on top once a frame: r3 = the manager (measured 2026-11-13 from sub_823FA048
// and sub_823F79C0): +161 busy (a window changing), +164 the active window, +(w+42)*4 the window objects, the
// byte at +71840+w*8 which of the three cursors (+52032, 2784 bytes each) marks its rows, the cursor's +2768
// the plate node it is on. Their rows are the plates "=_com[sys_menu_win]_NN_win" (or "_win_L") of the
// layout the cursor's plate belongs to; the game looks up only the ones it shows the cursor on, so they are
// looked up here, all of them.
extern "C" void __imp__sub_823FA048(PPCContext& ctx, uint8_t* base);
namespace {
uint32_t LookupNode(PPCContext& ctx, uint8_t* base, uint32_t owner, const std::string& name) {
  const uint32_t s = GuestString(name);
  if (!s) return 0;
  PPCContext saved = ctx;
  ctx.r3.u64 = owner;
  ctx.r4.u64 = s;
  __imp__sub_823BC8F0(ctx, base);
  const uint32_t node = ctx.r3.u32;
  ctx = saved;
  return node >= 0x1000 && node < 0x20000000 ? node : 0;
}

void ScanSystemWindows(PPCContext& ctx, uint8_t* base, uint32_t mgr) {
  if (base[mgr + 161] != 0) return;  // between windows
  const uint32_t w = Be32(base + mgr + 164);
  if (w >= 8) return;
  const uint32_t win = Be32(base + mgr + (w + 42) * 4);
  const uint32_t k = base[mgr + 71840 + w * 8];
  if (!win || k >= 3) return;
  // The pause's cross opens with no arm marked (a direction picks and opens one): no cursor there.
  uint32_t cursor = Be32(base + mgr + 52032 + k * 2784 + 2768);
  if (cursor < 0x1000 || cursor >= 0x20000000) cursor = 0;

  static uint32_t last_cursor = 0, last_window = ~0u;
  static std::vector<uint32_t> rows;
  static bool cross = false;
  static int age = 0;
  if (cursor != last_cursor || w != last_window || ++age > 120) {
    last_cursor = cursor;
    last_window = w;
    age = 0;
    // The owners to try: the one the game found the cursor's plate in, and the window's own layout.
    std::vector<uint32_t> owners;
    const auto info = nier::menu_mouse::tracker().NodeInfo(cursor);
    if (info.first) owners.push_back(info.first);
    owners.push_back(win + 48);
    rows.clear();
    cross = false;
    for (uint32_t owner : owners) {
      for (const char* suffix : {"_win", "_win_L"}) {
        std::vector<uint32_t> found;
        for (int n = 0; n <= 12; ++n) {
          const uint32_t node = LookupNode(ctx, base, owner, fmt::format("=_com[sys_menu_win]_{:02}{}", n, suffix));
          if (node) found.push_back(node);
        }
        if ((cursor && std::find(found.begin(), found.end(), cursor) != found.end()) ||
            (!cursor && found.size() == 4 && owner == win + 48 && std::string(suffix) == "_win")) {
          rows = found;
          break;
        }
      }
      if (!rows.empty()) break;
    }
    // A cross: four plates, one each way from their middle.
    if (rows.size() == 4) {
      double ax = 0, ay = 0;
      std::vector<std::pair<int, int>> p;
      for (uint32_t n : rows) {
        int x = 0, y = 0;
        nier::menu_mouse::NodePos(base, n, x, y);
        p.push_back({x, y});
        ax += x, ay += y;
      }
      ax /= 4, ay /= 4;
      int ways = 0;
      for (auto& [x, y] : p) {
        const double dx = x - ax, dy = y - ay;
        ways |= std::abs(dx) > std::abs(dy) ? (dx < 0 ? 1 : 2) : (dy < 0 ? 4 : 8);
      }
      cross = ways == 15;
    }
  }
  // A window without a cursor counts only as a cross (clicks pick its arms by their direction).
  if (rows.size() >= 2 && (cursor || cross)) {
    nier::menu_mouse::tracker().SetSystem(w, cursor ? cursor : rows[0], rows, cross, nier::menu_mouse::NowMs());
  }
}
}  // namespace

REX_HOOK_RAW(sub_823FA048) {
  const uint32_t mgr = ctx.r3.u32;
  __imp__sub_823FA048(ctx, base);
#if NIER_MENU_POINTER
  ScanSystemWindows(ctx, base, mgr);
#endif
}

// The title screen's lists, drawn without layout nodes and updated once a frame by the title object
// (measured 2026-11-13): the title menu (New Game / Continue / Options / Downloadable Content, sub_82199788:
// +2984 its state, 1 = taking input; +3020 the row, 0..3, the last only when +3043 is set) and the difficulty
// list after New Game (sub_82199C58: +2988, +3024, 0..2). When the row changes the game plays the move sound,
// sets +3032 (the highlight's timer) to 1.0 and, at the end of its update, starts the row's highlight on its
// layout (+188, virtual +32, r4 = the row's animation name). The mouse changes the row the same way, after
// the game's update; a click is the A the mouse already gives in menus.
extern "C" void __imp__sub_82199788(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82199C58(PPCContext& ctx, uint8_t* base);
namespace {
struct TitleList {
  uint32_t state_off, row_off;
  int rows;
  int y[4];          // the rows' centres in 1280x720 (measured on screen)
  uint32_t anim[4];  // the rows' highlight animation names
};
constexpr TitleList kTitleMenu{2984, 3020, 4, {459, 482, 506, 555}, {0x82019358, 0x8201934C, 0x8201933C, 0x82019330}};
constexpr TitleList kDifficulty{2988, 3024, 3, {461, 484, 506, 0}, {0x8201938C, 0x82019398, 0x820193A8, 0}};

double BeFloat(const uint8_t* base, uint32_t a) {
  const uint32_t v = Be32(base + a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void TitleSetRow(PPCContext& ctx, uint8_t* base, uint32_t obj, const TitleList& list, int hovered);

void TitleListMouse(PPCContext& ctx, uint8_t* base, uint32_t obj, const TitleList& list) {
#if NIER_MENU_POINTER
  {  // TEMP (TOUCHLOG, 2026-11-14): why the title menu takes no touch at times; on change.
    static const bool on = std::getenv("NIER_TOUCHLOG") != nullptr;
    static uint32_t last = ~0u;
    const uint32_t now_state = Be32(base + obj + list.state_off) << 8 | base[0x82757AB1];
    if (on && now_state != last) {
      last = now_state;
      REXLOG_INFO("TOUCHLOG titulo: estado {} lista {} AB1 {}", now_state >> 8, list.rows, now_state & 0xFF);
    }
  }
  if (Be32(base + obj + list.state_off) != 1) return;
  // A list menu over the title (its Options): the title's state stays 1 under it, and its rows took the touches.
  if (base[0x82757AB1] != 0) return;
  // The phone's carousel (title_carousel.cpp) in place of the rows: it shows the list and asks for rows.
  if (nier::title_carousel::Enabled()) {
    const int rows = &list == &kTitleMenu && base[obj + 3043] == 0 ? 3 : list.rows;
    const int row = int(Be32(base + obj + list.row_off));
    nier::menu_mouse::MarkOutsideMenu(1);
    const char* names[4] = {};
    for (int i = 0; i < list.rows; ++i)
      if (list.anim[i]) names[i] = reinterpret_cast<const char*>(base + list.anim[i]);
    const int want = nier::title_carousel::OnTitleList(&list == &kTitleMenu ? 0 : 1, rows, row, names);
    if (want >= 0 && want < rows && want != row) TitleSetRow(ctx, base, obj, list, want);
    return;
  }
  auto& tm = nier::menu_mouse::title();
  int mx = 0, my = 0;
  const bool inside = nier::menu_mouse::MouseInGame(mx, my);
  int hovered = -1;
  if (inside && mx >= 520 && mx < 760) {
    for (int i = 0; i < list.rows; ++i) {
      const bool apart = i > 0 && list.y[i] - list.y[i - 1] > 30;  // Downloadable Content
      const int top = apart ? list.y[i] - 17 : list.y[i] - 11, bottom = apart ? list.y[i] + 17 : list.y[i] + 12;
      if (my >= top && my < bottom) hovered = i;
    }
  }
  if (&list == &kTitleMenu && hovered == 3 && base[obj + 3043] == 0) hovered = -1;
  tm.ms = nier::menu_mouse::NowMs();
  nier::menu_mouse::MarkOutsideMenu(1);
  tm.hovered = hovered;
  const bool moved = inside && (mx != tm.last_x || my != tm.last_y);
  tm.last_x = mx;
  tm.last_y = my;
  if (!moved || hovered < 0 || Be32(base + obj + list.row_off) == uint32_t(hovered)) return;
  TitleSetRow(ctx, base, obj, list, hovered);
#else
  (void)ctx, (void)base, (void)obj, (void)list;
#endif
}

// Moves the list to a row as the game does (its sound, its highlight's timer and animation).
void TitleSetRow(PPCContext& ctx, uint8_t* base, uint32_t obj, const TitleList& list, int hovered) {
  auto& tm = nier::menu_mouse::title();
  SetBe32(base + obj + list.row_off, uint32_t(hovered));
  tm.moved_ms = nier::menu_mouse::NowMs();
  const double one = BeFloat(base, 0x820454E0);
  float onef = float(one);
  uint32_t onebits;
  std::memcpy(&onebits, &onef, 4);
  SetBe32(base + obj + 3032, onebits);
  PPCContext saved = ctx;
  ctx.r3.u64 = 0x8447BB90;  // the move sound, as the game plays it
  ctx.r4.u64 = 5;
  ctx.f1.f64 = BeFloat(base, 0x820007A4);
  ctx.f2.f64 = one;
  __imp__sub_82112190(ctx, base);
  ctx = saved;
  const uint32_t layout = Be32(base + obj + 188);  // the row's highlight
  if (layout) {
    const uint32_t fn = Be32(base + Be32(base + layout) + 32);
    if (PPCFunc* f = rex::runtime::ResolveIndirectFunction(fn)) {
      ctx.r3.u64 = layout;
      ctx.r4.u64 = list.anim[hovered];
      ctx.r5.u64 = 0;
      f(ctx, base);
      ctx = saved;
    }
  }
}
}  // namespace

REX_HOOK_RAW(sub_82199788) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_82199788(ctx, base);
  nier::title_carousel::OnTitleTick();
  TitleListMouse(ctx, base, obj, kTitleMenu);
}
REX_HOOK_RAW(sub_82199C58) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_82199C58(ctx, base);
  TitleListMouse(ctx, base, obj, kDifficulty);
}
// The game's choice dialogs ("Overwrite the system data saved before?" Yes / No, ...), updated once a frame
// by sub_823C6048 (r3 = the dialog; measured 2026-11-13): it takes input while +16 and +17 are set and +18 is
// 1; +20 the number of choices, +24 the chosen one (left / right step it and play the sound in +60). With two
// choices they are drawn side by side (measured on the overwrite question: Yes at 580, No at 722, y 416).
// The pointer picks the choice under it the way left / right do; a click is the A the mouse gives in menus.
extern "C" void __imp__sub_823C6048(PPCContext& ctx, uint8_t* base);
namespace {
std::atomic<uint64_t> g_dialog_ms{0};  // a choice dialog took input then (the list under it waits)
std::atomic<int> g_column_want{-1};    // a talk's column: the row a press woke it on (sub_823C5E18)
std::atomic<uint64_t> g_column_want_ms{0};
void ChoiceDialogMouse(PPCContext& ctx, uint8_t* base, uint32_t dlg, bool column = false) {
#if NIER_MENU_POINTER
  if (!base[dlg + 16] || !base[dlg + 17] || base[dlg + 18] != 1) return;
  const uint32_t count = Be32(base + dlg + 20);
  if (count < 2 || count > 8) return;
  g_dialog_ms = nier::menu_mouse::NowMs();
#if !defined(_WIN32)
  // Touch: a talk's column goes by the stick (touch_controls.h kChoice; 2026-11-19, the user: a finger picked at
  // random). No pointer, and no menu mark (a menu would hide the stick).
  if (column) {
    nier::menu_mouse::choice_ms() = nier::menu_mouse::NowMs();
    return;
  }
#endif
  auto& tm = nier::menu_mouse::title();
  int mx = 0, my = 0;
  const bool inside = nier::menu_mouse::MouseInGame(mx, my);
  int hovered = -1;
  if (count == 2 && !column) {
    // By the screen's halves around the two choices' middle (651), over the dialog's height band: the dialogs sit at
    // different heights (the load question is not the overwrite one's y 416; a tap on its No took Yes, 2026-11-14).
    if (inside && my >= 250 && my < 600 && mx >= 400 && mx < 900) hovered = mx < 651 ? 0 : 1;
  } else if (inside && mx >= 140 && mx < 700) {
    // A column under a talk's text (sub_823C5AF8, the same dialog drawn as a column; 2026-11-17, Jakob's
    // Strengthen / Buy and sell / Cancel: rows at y 604, 635, 665).
    const int row = (my - 589) / 31;
    if (my >= 589 && row < int(count)) hovered = row;
  }
  // The row a touch woke the column on (sub_823C5E18): the cursor goes there now, though the finger did not move
  // since (2026-11-19, the phone: the cursor stayed on the first choice, or came only at random).
  bool woken = false;
  if (column) {
    const int want = g_column_want.exchange(-1);
    if (want >= 0 && want < int(count) && nier::menu_mouse::NowMs() - g_column_want_ms.load() < 1500)
      hovered = want, woken = true;
  }
  tm.ms = nier::menu_mouse::NowMs();
  nier::menu_mouse::MarkOutsideMenu(2);
  tm.hovered = hovered;
  const bool moved = woken || (inside && (mx != tm.last_x || my != tm.last_y));
  tm.last_x = mx;
  tm.last_y = my;
  if (!moved || hovered < 0 || Be32(base + dlg + 24) == uint32_t(hovered)) return;
  SetBe32(base + dlg + 24, uint32_t(hovered));
  tm.moved_ms = nier::menu_mouse::NowMs();  // a touch's A waits for the move (touch_controls.h)
  PPCContext saved = ctx;
  ctx.r3.u64 = 0x8447BB90;  // the dialog's move sound, as left / right play it
  ctx.r4.u64 = Be32(base + dlg + 60);
  ctx.f1.f64 = BeFloat(base, 0x820007A4);
  ctx.f2.f64 = BeFloat(base, 0x820454E0);
  __imp__sub_82112190(ctx, base);
  ctx = saved;
#else
  (void)ctx, (void)base, (void)dlg;
#endif
}
}  // namespace

REX_HOOK_RAW(sub_823C6048) {
  ChoiceDialogMouse(ctx, base, ctx.r3.u32);
  __imp__sub_823C6048(ctx, base);
}
// The column form's drawing (its +4): the only part that runs while the talk's choices show with none chosen yet
// (+18 not 1, measured 2026-11-17 with a read watch: the update, below, runs from the first up / down on). The
// pointer over a choice there shows the cursor and presses down once, which wakes the list on its first choice;
// the update then walks it to the one under the pointer.
extern "C" void __imp__sub_823C5E18(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_823C5E18) {
#if NIER_MENU_POINTER
  const uint32_t d = ctx.r3.u32;
  const uint32_t count = Be32(base + d + 20);
#if !defined(_WIN32)
  if (base[d + 16] && base[d + 17] && base[d + 18] != 1 && count >= 2 && count <= 8)
    nier::menu_mouse::choice_ms() = nier::menu_mouse::NowMs();  // touch: the stick (ChoiceDialogMouse)
  else
#endif
  if (base[d + 16] && base[d + 17] && base[d + 18] != 1 && count >= 2 && count <= 8) {
    // The cursor shows at once (the mouse turned the camera until then: held in the middle, it never got to the
    // choices, 2026-11-17).
    nier::menu_mouse::MarkOutsideMenu(2);
    int mx = 0, my = 0;
    if (nier::menu_mouse::MouseInGame(mx, my) && mx >= 140 && mx < 700 && my >= 589 && (my - 589) / 31 < int(count)) {
      auto& tm = nier::menu_mouse::title();
      const uint64_t now = nier::menu_mouse::NowMs();
      tm.ms = now;
      tm.hovered = -2;  // a click before the list wakes is nothing (all of it: a touch's A passed after 120 ms)
      if (nier::menu_mouse::PointerDown()) {  // a press (a touch): the row under it, once awake
        g_column_want = (my - 589) / 31;
        g_column_want_ms = now;
      }
      static uint64_t woke = 0;
      if (now - woke > 500) {
        woke = now;
        tm.press = 0x0002;  // down: the list wakes on its first choice
        tm.moved_ms = now;  // a touch's A waits
      }
    }
  }
#endif
  __imp__sub_823C5E18(ctx, base);
}
// Its column form: the talks' choices (the shopkeepers' "Strengthen weapons / Buy and sell items / Cancel").
extern "C" void __imp__sub_823C5AF8(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_823C5AF8) {
  {  // TEMP (SHOPLOG): does the talk's column run, and what does it hold
    static const bool log = std::getenv("NIER_SHOPLOG") != nullptr;
    static uint32_t last = ~0u;
    const uint32_t d = ctx.r3.u32;
    const uint32_t sig = uint32_t(base[d + 16]) << 24 | uint32_t(base[d + 17]) << 16 | uint32_t(base[d + 18]) << 8 |
                         (Be32(base + d + 20) & 0xF) << 4 | (Be32(base + d + 24) & 0xF);
    if (log && sig != last) {
      last = sig;
      REXLOG_INFO("SHOPLOG coluna {:08X}: +16 {} +17 {} +18 {} itens {} linha {}", d, base[d + 16], base[d + 17],
                  base[d + 18], Be32(base + d + 20), Be32(base + d + 24));
    }
  }
  ChoiceDialogMouse(ctx, base, ctx.r3.u32, true);
  __imp__sub_823C5AF8(ctx, base);
}

// The save list (Continue / Load, and saving): updated once a frame by sub_82427DC8 (r3 = the list;
// measured 2026-11-13): it takes input while +8372 and the byte +8354 are 0; +8360 the row (0..2 the three
// saves, 3 Select Device), the bytes +8364..+8367 which rows can be chosen. sub_824274E8(list, row) is the
// game's own "select this row" (its up / down call it): highlight, the row, the move sound. The mouse calls
// it with the row under the pointer; a click is the A the mouse gives in menus.
extern "C" void __imp__sub_82427DC8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_824274E8(PPCContext& ctx, uint8_t* base);
namespace {
void SaveListMouse(PPCContext& ctx, uint8_t* base, uint32_t list) {
#if NIER_MENU_POINTER
  if (Be32(base + list + 8372) != 0 || base[list + 8354] != 0) return;
  if (nier::menu_mouse::NowMs() - g_dialog_ms.load() < 200) return;  // "Load the selected file?" over it
  auto& tm = nier::menu_mouse::title();
  int mx = 0, my = 0;
  const bool inside = nier::menu_mouse::MouseInGame(mx, my);
  int hovered = -1;
  if (inside && mx >= 200 && mx < 1090) {
    for (int i = 0; i < 3; ++i)  // the saves' cards (measured: 168..292, 159 apart)
      if (my >= 168 + 159 * i && my < 292 + 159 * i) hovered = i;
    if (my >= 616 && my < 652) hovered = 3;  // Select Device
  }
  if (hovered >= 0 && base[list + 8364 + uint32_t(hovered)] == 0) hovered = -1;
  tm.ms = nier::menu_mouse::NowMs();
  nier::menu_mouse::MarkOutsideMenu(3);
  tm.hovered = hovered;
  const bool moved = inside && (mx != tm.last_x || my != tm.last_y);
  tm.last_x = mx;
  tm.last_y = my;
  if (!moved || hovered < 0 || Be32(base + list + 8360) == uint32_t(hovered)) return;
  PPCContext saved = ctx;
  ctx.r3.u64 = list;
  ctx.r4.u64 = uint32_t(hovered);
  __imp__sub_824274E8(ctx, base);
  ctx = saved;
#else
  (void)ctx, (void)base, (void)list;
#endif
}
}  // namespace

REX_HOOK_RAW(sub_82427DC8) {
  SaveListMouse(ctx, base, ctx.r3.u32);
  __imp__sub_82427DC8(ctx, base);
}

// Weiss's book, page by page (measured 2026-11-13 from the code): each list of a page has its own input
// handler, called once a frame while it takes input. The mouse picks the row under it the way the handler's
// up / down do, with the same calls, before the handler runs (so a click's A acts on that row); a click is the
// A the mouse gives in menus. All these lists draw their rows at the same place: the first at y 273, 39 apart.
extern "C" void __imp__sub_824053C0(PPCContext& ctx, uint8_t* base);  // menu sound (r3 = which, f1, f2)
extern "C" void __imp__sub_82406148(PPCContext& ctx, uint8_t* base);  // Items: category r4 unhighlighted (r5 = down)
extern "C" void __imp__sub_824061E8(PPCContext& ctx, uint8_t* base);  // Items: category r4 highlighted (r5 = down)
extern "C" void __imp__sub_824060E8(PPCContext& ctx, uint8_t* base);  // Items: the cursor on the category
extern "C" void __imp__sub_82408510(PPCContext& ctx, uint8_t* base);  // Items: categories' input
extern "C" void __imp__sub_82415240(PPCContext& ctx, uint8_t* base);  // Quests: categories' input
extern "C" void __imp__sub_8241FD88(PPCContext& ctx, uint8_t* base);  // Weapons: categories' input
extern "C" void __imp__sub_82425010(PPCContext& ctx, uint8_t* base);  // Word Edit: categories' input
extern "C" void __imp__sub_82412F28(PPCContext& ctx, uint8_t* base);  // Quests: category changed
extern "C" void __imp__sub_8241D020(PPCContext& ctx, uint8_t* base);  // Weapons: category changed
extern "C" void __imp__sub_82421E88(PPCContext& ctx, uint8_t* base);  // Word Edit: category changed
extern "C" void __imp__sub_82408688(PPCContext& ctx, uint8_t* base);  // Items: list's input
extern "C" void __imp__sub_824153D8(PPCContext& ctx, uint8_t* base);  // Quests: list's input
extern "C" void __imp__sub_8241F468(PPCContext& ctx, uint8_t* base);  // Weapons: list's input
extern "C" void __imp__sub_82424278(PPCContext& ctx, uint8_t* base);  // Word Edit: targets' input
extern "C" void __imp__sub_824072B0(PPCContext& ctx, uint8_t* base);  // Items: list laid out (r4 1 up / 2 down)
extern "C" void __imp__sub_824144A8(PPCContext& ctx, uint8_t* base);  // Quests: list laid out
extern "C" void __imp__sub_8241E9E8(PPCContext& ctx, uint8_t* base);  // Weapons: list laid out
extern "C" void __imp__sub_82423CD0(PPCContext& ctx, uint8_t* base);  // Word Edit: targets laid out
extern "C" void __imp__sub_8240F5B0(PPCContext& ctx, uint8_t* base);  // Notes: lists' input (4 of them)
extern "C" void __imp__sub_82410C38(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82410998(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82411008(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8240EB78(PPCContext& ctx, uint8_t* base);  // Notes: lists laid out
extern "C" void __imp__sub_824101C8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8240FF08(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82410748(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8240E998(PPCContext& ctx, uint8_t* base);  // Notes: a row left (its index)
extern "C" void __imp__sub_8240F540(PPCContext& ctx, uint8_t* base);
namespace {
void BookSound(PPCContext& ctx, uint8_t* base, uint32_t which) {
  PPCContext saved = ctx;
  ctx.r3.u64 = which;
  ctx.f1.f64 = BeFloat(base, 0x820007A4);
  ctx.f2.f64 = BeFloat(base, 0x820454E0);
  __imp__sub_824053C0(ctx, base);
  ctx = saved;
}
void Call3(PPCContext& ctx, uint8_t* base, PPCFunc* f, uint32_t r3, uint32_t r4, uint32_t r5) {
  PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  f(ctx, base);
  ctx = saved;
}
// The page's layout (+16) plays the named animation (its virtual +32), as the categories' up / down do.
void LayoutAnim(PPCContext& ctx, uint8_t* base, uint32_t layout, uint32_t name) {
  const uint32_t fn = Be32(base + Be32(base + layout) + 32);
  if (PPCFunc* f = rex::runtime::ResolveIndirectFunction(fn)) Call3(ctx, base, f, layout, name, 0);
}

// The book row under the pointer (0..rows-1, -1: none), noted for the click (title()); `act`: the pointer moved,
// or the button is down (a click on a row the cursor is not on picks it first).
int BookRow(int rows, bool& act, int y0 = 273) { return nier::menu_mouse::BookRowUnderMouse(rows, act, y0); }

// A page's categories: +idx the category (0..count-1); the old one's "off" animation, the new one's "on", the
// page's own refresh, sound 5. `guard`: a byte that, set, stops the game's up / down (Weapons: one category).
struct BookCategories {
  uint32_t idx, count, on_table, off_table;
  PPCFunc* refresh;
  uint32_t guard;
};
void BookCategoriesMouse(PPCContext& ctx, uint8_t* base, uint32_t page, const BookCategories& c) {
  if (c.guard && base[page + c.guard]) return;
  bool act = false;
  const int row = BookRow(int(c.count), act);
  const uint32_t cur = Be32(base + page + c.idx);
  if (!act || row < 0 || uint32_t(row) == cur || cur >= c.count) return;
  LayoutAnim(ctx, base, page + 16, Be32(base + c.off_table + cur * 4));
  SetBe32(base + page + c.idx, uint32_t(row));
  LayoutAnim(ctx, base, page + 16, Be32(base + c.on_table + uint32_t(row) * 4));
  Call3(ctx, base, c.refresh, page, 0, 0);
  BookSound(ctx, base, 5);
}

// A page's scrolling list (the book's own list steps, sub_82404AB0 & co.): +sel the cursor's row among those
// shown, +scroll the first shown, +count how many; the page lays the rows out again (r4 1 = up, 2 = down) and
// plays sound 5.
struct BookScrollList {
  uint32_t sel, scroll, count, shown;
  PPCFunc* layout;
  int y0 = 273;              // the first row's y (the Notes' lists: the layout's second screen, 152)
  PPCFunc* left = nullptr;   // called with the row left (its index in the whole list), when the list does
};
void BookScrollMouse(PPCContext& ctx, uint8_t* base, uint32_t page, const BookScrollList& l) {
  bool act = false;
  const int row = BookRow(int(l.shown), act, l.y0);
  const uint32_t cur = Be32(base + page + l.sel), first = Be32(base + page + l.scroll);
  const uint32_t count = Be32(base + page + l.count);
  if (!act || row < 0 || uint32_t(row) == cur || first + uint32_t(row) >= count) return;
  if (l.left) Call3(ctx, base, l.left, page, first + cur, 0);
  SetBe32(base + page + l.sel, uint32_t(row));
  Call3(ctx, base, l.layout, page, uint32_t(row) < cur ? 1 : 2, 0);
  BookSound(ctx, base, 5);
}

// Items' categories: their own calls (sub_82406148 / sub_824061E8 / sub_824060E8), +20224, 6 of them.
std::atomic<uint32_t> g_items_page{0};  // the Items page (its quantity box is handled from the book's update)
std::atomic<bool> g_items_qty_shown{false};  // the quantity box came up for the item (until back on the list)
void ItemCategoriesMouse(PPCContext& ctx, uint8_t* base, uint32_t page) {
  g_items_page = page;
  bool act = false;
  const int row = BookRow(6, act);
  const uint32_t cur = Be32(base + page + 20224);
  if (!act || row < 0 || uint32_t(row) == cur || cur >= 6) return;
  const uint32_t down = uint32_t(row) > cur ? 1 : 0;
  Call3(ctx, base, __imp__sub_82406148, page, cur, down);
  SetBe32(base + page + 20224, uint32_t(row));
  Call3(ctx, base, __imp__sub_824061E8, page, uint32_t(row), down);
  Call3(ctx, base, __imp__sub_824060E8, page, 0, 0);
  BookSound(ctx, base, 5);
}

const BookCategories kQuestCategories{8704, 4, 0x827610B8, 0x827610C8, __imp__sub_82412F28, 0};
const BookCategories kWeaponCategories{27376, 4, 0x827621BC, 0x827621D0, __imp__sub_8241D020, 27398};
const BookCategories kWordCategories{9516, 4, 0x82762FD0, 0x82762FE0, __imp__sub_82421E88, 0};
const BookScrollList kItemList{20228, 20232, 3580, 6, __imp__sub_824072B0};
const BookScrollList kQuestList{8708, 8712, 3576, 6, __imp__sub_824144A8};
const BookScrollList kWeaponList{27380, 27384, 3576, 6, __imp__sub_8241E9E8};
const BookScrollList kWordTargets{9520, 9528, 3580, 5, __imp__sub_82423CD0};
// The Notes' lists (Word List, Documents, Tutorials, Fishing Records): on the layout's second screen, rows from y 152.
const BookScrollList kNotesList1{3560, 3564, 22352, 9, __imp__sub_8240EB78, 152};
const BookScrollList kNotesList2{3560, 3564, 5684, 9, __imp__sub_824101C8, 152};
const BookScrollList kNotesList3{3560, 3564, 3608, 8, __imp__sub_8240FF08, 152, __imp__sub_8240E998};
const BookScrollList kNotesList4{3560, 3564, 24428, 9, __imp__sub_82410748, 152, __imp__sub_8240F540};
}  // namespace

#if NIER_MENU_POINTER
#define NIER_BOOK_HOOK(addr, call)  \
  REX_HOOK_RAW(sub_##addr) {        \
    call;                           \
    __imp__sub_##addr(ctx, base);   \
  }
#else
#define NIER_BOOK_HOOK(addr, call) \
  REX_HOOK_RAW(sub_##addr) { __imp__sub_##addr(ctx, base); }
#endif
NIER_BOOK_HOOK(82408510, ItemCategoriesMouse(ctx, base, ctx.r3.u32))
NIER_BOOK_HOOK(82415240, BookCategoriesMouse(ctx, base, ctx.r3.u32, kQuestCategories))
NIER_BOOK_HOOK(8241FD88, BookCategoriesMouse(ctx, base, ctx.r3.u32, kWeaponCategories))
NIER_BOOK_HOOK(82425010, BookCategoriesMouse(ctx, base, ctx.r3.u32, kWordCategories))
NIER_BOOK_HOOK(82408688, (g_items_page = ctx.r3.u32, BookScrollMouse(ctx, base, ctx.r3.u32, kItemList)))
NIER_BOOK_HOOK(824153D8, BookScrollMouse(ctx, base, ctx.r3.u32, kQuestList))
NIER_BOOK_HOOK(8241F468, BookScrollMouse(ctx, base, ctx.r3.u32, kWeaponList))
NIER_BOOK_HOOK(82424278, BookScrollMouse(ctx, base, ctx.r3.u32, kWordTargets))
NIER_BOOK_HOOK(8240F5B0, BookScrollMouse(ctx, base, ctx.r3.u32, kNotesList1))
NIER_BOOK_HOOK(82410C38, BookScrollMouse(ctx, base, ctx.r3.u32, kNotesList2))
NIER_BOOK_HOOK(82410998, BookScrollMouse(ctx, base, ctx.r3.u32, kNotesList3))
NIER_BOOK_HOOK(82411008, BookScrollMouse(ctx, base, ctx.r3.u32, kNotesList4))

// The book's tabs (Maps, Items, Quests, Weapons, Word Edit, Notes): the book's manager, once a frame
// (sub_82401BE8, r3 = the book; measured 2026-11-13): +4 the page shown, +8 how many; after its update the
// byte +22 says the page may be turned and +0x4C1A0 < 2 that no turn is under way; sub_824001F8 turns to the
// next page and sub_82400390 to the previous one (its RB / LB). A click on a tab turns, one page each time
// the book allows it, to that page.
extern "C" void __imp__sub_82401BE8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_824001F8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82400390(PPCContext& ctx, uint8_t* base);
namespace {
void BookTabsMouse(PPCContext& ctx, uint8_t* base, uint32_t book) {
#if NIER_MENU_POINTER
  // The tabs across the top, left to right (measured on screen, 1280x720).
  static constexpr int kTabX[7] = {88, 310, 430, 595, 720, 875, 1028};
  static int target = -1;
  static bool left_was = false;
  // The manager runs in gameplay too: only with the book showing (a list menu, its screen the book: the same
  // marks as PcOptionsState::BookOpen, read here without its menu test). Else a click up there in gameplay
  // was a tab, and the box's check kept saying "menu" (2026-11-13).
  if (base[0x82757AB1] == 0 || base[0x82757AC5] != 0x10) {
    target = -1;
    left_was = false;
    return;
  }
  int mx = 0, my = 0;
  const bool inside = nier::menu_mouse::MouseInGame(mx, my);
  const bool left = inside && nier::menu_mouse::PointerDown();
  if (left && !left_was && my >= 100 && my < 156) {
    for (int i = 0; i < 6; ++i)
      if (mx >= kTabX[i] && mx < kTabX[i + 1]) target = i;
    auto& tm = nier::menu_mouse::title();  // the click is not the page's A
    tm.ms = nier::menu_mouse::NowMs();
    tm.hovered = -1;
  }
  left_was = left;
  const uint32_t count = Be32(base + book + 8), cur = Be32(base + book + 4);
  {  // Items' quantity box (how many to discard; the page's state 4): beside the chosen item (its row: +20228),
     // x 852..980, its up arrow at the item's centre +0..24, its down arrow +55..80 (measured on screen). A click
     // on an arrow is that arrow's up / down; on the number, the A.
    const uint32_t page = cur < count ? Be32(base + book + (cur + 11868) * 4) : 0;
    if (page && page == g_items_page.load()) {
      const uint32_t st = Be32(base + page + 3552);
      if (st == 4) g_items_qty_shown = true;
      if (st == 1 || st == 2) g_items_qty_shown = false;  // back on the categories / the list
    }
    if (page && page == g_items_page.load() && Be32(base + page + 3552) == 4) {
      auto& tm = nier::menu_mouse::title();
      const int centre = 273 + 39 * int(Be32(base + page + 20228)) + 11;
      const bool in_box = inside && mx >= 852 && mx < 980 && my >= centre && my < centre + 80;
      const int arrow = !in_box ? 0 : my < centre + 24 ? 1 : my >= centre + 55 ? 2 : 0;
      tm.ms = nier::menu_mouse::NowMs();
      tm.hovered = in_box && !arrow ? 0 : -1;
      static bool was = false;
      if (left && !was && arrow) tm.press = arrow == 1 ? 0x0001 : 0x0002;
      was = left;
    }
  }
  if (target < 0 || uint32_t(target) >= count || cur == uint32_t(target)) {
    target = -1;
    return;
  }
  if (!base[book + 22] || Be32(base + book + 0x4C1A0) >= 2) return;  // not now (a turn under way, a list open)
  PPCContext saved = ctx;
  ctx.r3.u64 = book;
  if (uint32_t(target) > cur) {
    __imp__sub_824001F8(ctx, base);
  } else {
    __imp__sub_82400390(ctx, base);
  }
  ctx = saved;
#else
  (void)ctx, (void)base, (void)book;
#endif
}
}  // namespace

// The quick weapon switch's equip (weapon_switch.h), on the game's thread (perf_overlay.cpp's present hook): the
// player's choice for that kind (+140 / +144 / +148, already what the equip takes: the weapon menu compares
// sub_8234B420's answer with the equipped one, +136) to sub_823374A0(0x83C01E80, it), as the menu's A does; then
// that menu's sound (42).
extern "C" void __imp__sub_823374A0(PPCContext& ctx, uint8_t* base);
// 2026-11-23 (release): Back+Start's quick PC panel is a developer's shortcut, off in the public build; the
// developer's own nier.toml turns it on (nier_dev_panel = true). input_recorder.h reads it.
REXCVAR_DEFINE_BOOL(nier_dev_panel, false, "NieR/Debug", "Back+Start opens the PC options panel (developer shortcut).");

// TEMP (2026-11-20, the player: keep the combo going across a quick switch): where Nier keeps his place in a combo.
// Each time his animation (+0x9D8) changes, a snapshot of his object's words; once it stays 2 s, the words that went
// up by one at three or more changes in a row (small numbers) are logged: "[combo] +offset: values".
REXCVAR_DEFINE_BOOL(nier_combo_scan, false, "NieR/Debug",
                    "Look for Nier's combo counter (logs [combo] candidates after a string of attacks).");
namespace {
void ComboScan(const uint8_t* base) {
  if (!REXCVAR_GET(nier_combo_scan)) return;
  constexpr uint32_t kNier = 0x848BE4B0u, kWords = 0x5000 / 4;
  static std::vector<std::vector<uint32_t>> shots;
  static std::vector<uint32_t> anims;
  static uint32_t last_anim = 0xFFFFFFFFu, still = 0;
  if (nier::weapon_switch::Be32(base, kNier + 3472) != 0x82740048u) return;
  const uint32_t anim = nier::weapon_switch::Be32(base, kNier + 0x9D8);
  if (anim != last_anim) {
    last_anim = anim;
    still = 0;
    std::vector<uint32_t> w(kWords);
    for (uint32_t i = 0; i < kWords; ++i) w[i] = nier::weapon_switch::Be32(base, kNier + i * 4);
    shots.push_back(std::move(w));
    anims.push_back(anim);
    if (shots.size() > 16) shots.erase(shots.begin()), anims.erase(anims.begin());
    return;
  }
  if (++still != 120 || shots.size() < 4) return;
  std::string seq;
  for (uint32_t a : anims) seq += fmt::format(" {}", a);
  int logged = 0;
  for (uint32_t i = 0; i < kWords && logged < 12; ++i) {
    int run = 0, best = 0;
    for (size_t s = 1; s < shots.size(); ++s) {
      const uint32_t a = shots[s - 1][i], b = shots[s][i];
      run = (b == a + 1 && b < 16) ? run + 1 : 0;
      best = std::max(best, run);
    }
    if (best < 3) continue;
    std::string vals;
    for (const auto& s : shots) vals += fmt::format(" {}", s[i]);
    REXLOG_INFO("[combo] +{} (0x{:X}):{}", i * 4, i * 4, vals);
    ++logged;
  }
  REXLOG_INFO("[combo] {} trocas de animacao:{} ({} candidatos)", shots.size(), seq, logged);
  shots.clear();
  anims.clear();
}
}  // namespace
// The combo across a quick switch (2026-11-20, the player: one-handed hit 1, then the two-handed's hit 2). Nier's place
// in his string of attacks is +0x4704 (found with ComboScan: -1 idle, 0, 1, 2... one per hit; the one-handed's five
// went 0..4, the spear's 0..2). Kept from before the equip and given back for 20 frames if the equip set it lower;
// a hit of his own (a higher number) ends it.
struct ComboKeep {
  int32_t value = -1;
  int frames = 0;
};
ComboKeep g_combo_keep;
constexpr uint32_t kNierCombo = 0x848BE4B0u + 0x4704;
void ComboKeepTick(uint8_t* base) {
  ComboKeep& c = g_combo_keep;
  if (c.frames <= 0) return;
  --c.frames;
  const int32_t now = int32_t(nier::weapon_switch::Be32(base, kNierCombo));
  if (now > c.value) {
    c.frames = 0;
    return;
  }
  if (now < c.value) {
    const uint32_t v = uint32_t(c.value);
    base[kNierCombo] = uint8_t(v >> 24), base[kNierCombo + 1] = uint8_t(v >> 16);
    base[kNierCombo + 2] = uint8_t(v >> 8), base[kNierCombo + 3] = uint8_t(v);
  }
}
// TEMP (2026-11-20, the player: the boss time achievements never come): which words of the save block (play time)
// go up, how much in 10 s of real time, and the game frames in that time. A clock of 1/30 s units counting frames
// would show twice the seconds at 60 fps.
REXCVAR_DEFINE_BOOL(nier_time_scan, false, "NieR/Debug", "Log the save block's counters every 10 s ([tempo]).");
void TimeScan(const uint8_t* base) {
  if (!REXCVAR_GET(nier_time_scan)) return;
  constexpr uint32_t kBlock = 0x84CC6BA0u, kWords = 0xC30 / 4;
  static std::vector<uint32_t> last;
  static auto last_at = std::chrono::steady_clock::now();
  static uint32_t frames = 0;
  ++frames;
  const auto now = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(now - last_at).count();
  if (!last.empty() && secs < 10.0) return;
  std::vector<uint32_t> w(kWords);
  for (uint32_t i = 0; i < kWords; ++i) w[i] = nier::weapon_switch::Be32(base, kBlock + i * 4);
  if (!last.empty()) {
    std::string line;
    for (uint32_t i = 0; i < kWords; ++i) {
      if (w[i] == last[i]) continue;
      float f0, f1;
      std::memcpy(&f0, &last[i], 4);
      std::memcpy(&f1, &w[i], 4);
      const int64_t di = int64_t(w[i]) - int64_t(last[i]);
      if (di > 0 && di < 100000)
        line += fmt::format(" +{}:int+{}", i * 4, di);
      else if (std::isfinite(f0) && std::isfinite(f1) && f1 > f0 && f1 - f0 < 100000.0f)
        line += fmt::format(" +{}:float+{:.2f}", i * 4, f1 - f0);
    }
    REXLOG_INFO("[tempo] {:.1f} s reais, {} quadros do jogo ({:.0f} fps):{}", secs, frames, frames / secs, line);
  }
  last = std::move(w);
  last_at = now;
  frames = 0;
}
namespace nier::weapon_switch {
void EquipTick(PPCContext& ctx, uint8_t* base) {
  ComboScan(base);
  TimeScan(base);
  ComboKeepTick(base);
  const int k = Request().exchange(-1);
  if (k < 0 || k > 2) return;
  const uint32_t at = kPlayer + kPlayerWeapons;
  uint32_t weapon = Be32(base, at + uint32_t(k) * 4);
  if (const char* test = std::getenv("NIER_WEAPON_SWITCH_TEST"); test && *test >= '0' && *test <= '9')
    weapon = uint32_t(std::atoi(test));  // tests: equip this one
  REXLOG_INFO("Troca rapida: tipo {} arma {:X} (equipada {:X}; escolhidas {:X} {:X} {:X})", k, weapon,
              Be32(base, kPlayer + 136), Be32(base, at), Be32(base, at + 4), Be32(base, at + 8));
  if (weapon == 0xFFFFFFFFu || weapon == Be32(base, kPlayer + 136)) return;
  const int32_t combo = int32_t(Be32(base, kNierCombo));
  if (combo >= 0) {
    g_combo_keep = {combo, 20};
    REXLOG_INFO("Troca rapida: combo no golpe {} mantido", combo + 1);
  }
  PPCContext saved = ctx;
  // 0x84C01E80 (lis -31552, +7808: the weapon menu's sub_82431DD0 and sub_82385120 / sub_82386778). 2026-11-20 (the
  // player, after the timeskip): it was 0x83C01E80 here - the equipped number changed, the weapon went invisible.
  ctx.r3.u64 = 0x84C01E80u;
  ctx.r4.u64 = weapon;
  __imp__sub_823374A0(ctx, base);
  ctx = saved;
  ctx.r3.u64 = 0x8447BB90u;
  ctx.r4.u64 = 42;
  ctx.f1.f64 = BeFloat(base, 0x820007A4);
  ctx.f2.f64 = BeFloat(base, 0x820454E0);
  __imp__sub_82112190(ctx, base);
  ctx = saved;
}
}  // namespace nier::weapon_switch
REX_HOOK_RAW(sub_82401BE8) {
  const uint32_t book = ctx.r3.u32;
  __imp__sub_82401BE8(ctx, base);
  BookTabsMouse(ctx, base, book);
}

// Items' little menus over the chosen item (measured 2026-11-13): "Use / Discard" (sub_824088E8's state,
// sub_82407750: shown while the byte +36969 is set, the choice the byte +36970) and, after Discard, "Discard /
// Keep" (sub_82408A38's state, sub_82406790: the byte +36971). Up / down flip the choice, play the choice's
// highlight on the page's layout and sound 5. Drawn beside the chosen item (its row: +20228): Use / Discard at
// x 725..915, the item's centre +2 and +43; Discard / Keep at x 852..1043, -20 and +22.
extern "C" void __imp__sub_824088E8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82408A38(PPCContext& ctx, uint8_t* base);
namespace {
void ItemChoiceMouse(PPCContext& ctx, uint8_t* base, uint32_t page, uint32_t choice_byte, int x0, int x1,
                     const int (&dy)[2], uint32_t name_on, uint32_t name_off, bool reversed) {
  auto& tm = nier::menu_mouse::title();
  int mx = 0, my = 0;
  const bool inside = nier::menu_mouse::MouseInGame(mx, my);
  const int centre = 273 + 39 * int(Be32(base + page + 20228)) + 11;
  int hovered = -1;
  if (inside && mx >= x0 && mx < x1) {
    for (int i = 0; i < 2; ++i)
      if (my >= centre + dy[i] - 20 && my < centre + dy[i] + 20) hovered = i;
  }
  tm.ms = nier::menu_mouse::NowMs();
  tm.hovered = hovered;
  const bool moved = inside && (mx != tm.last_x || my != tm.last_y);
  tm.last_x = mx;
  tm.last_y = my;
  const bool act = moved || (inside && nier::menu_mouse::PointerDown());
  // Shown top to bottom; the byte counts the other way round on Use / Discard (1 = Use, the user's test).
  const uint8_t want = uint8_t(reversed ? 1 - hovered : hovered);
  const uint8_t cur = base[page + choice_byte];
  if (!act || hovered < 0 || want == cur) return;
  base[page + choice_byte] = want;
  LayoutAnim(ctx, base, page + 16, Be32(base + (want ? name_on : name_off)));
  BookSound(ctx, base, 5);
}
}  // namespace

REX_HOOK_RAW(sub_824088E8) {
#if NIER_MENU_POINTER
  const uint32_t page = ctx.r3.u32;
  if (base[page + 36969]) {
    static constexpr int kDy[2] = {2, 43};
    ItemChoiceMouse(ctx, base, page, 36970, 725, 915, kDy, 0x8275BDF0, 0x8275BDF4, true);
  }
#endif
  __imp__sub_824088E8(ctx, base);
}
REX_HOOK_RAW(sub_82408A38) {
#if NIER_MENU_POINTER
  // After the quantity box (more than one of the item) the box sits further right and lower (measured).
  if (g_items_qty_shown.load()) {
    static constexpr int kDy[2] = {23, 64};
    ItemChoiceMouse(ctx, base, ctx.r3.u32, 36971, 966, 1157, kDy, 0x8275BE18, 0x8275BE1C, true);
  } else {
    static constexpr int kDy[2] = {-20, 22};
    ItemChoiceMouse(ctx, base, ctx.r3.u32, 36971, 852, 1043, kDy, 0x8275BE18, 0x8275BE1C, true);
  }
#endif
  __imp__sub_82408A38(ctx, base);
}

// Word Edit's word lists (the first word's at x 248..588, the second's at 518..858, rows from y 182; measured
// 2026-11-13): sub_824251F8 / sub_824255D8, how many words in +5384 / +7460. Up to five: an ordinary list
// (the row +9536 / +9540, the first shown +9548 / +9552). More: the list turns round under a fixed cursor on
// its top row (+9548 / +9552 the word on top), so a click on a row turns it to the top (its A then takes it);
// pointing does nothing there. Laid out again by sub_82424AB8 / sub_82424D28, sound 5.
extern "C" void __imp__sub_824251F8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_824255D8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82424AB8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82424D28(PPCContext& ctx, uint8_t* base);
namespace {
struct WordList {
  uint32_t count, sel, top;
  int x0, x1;
  PPCFunc* layout;
};
void WordListMouse(PPCContext& ctx, uint8_t* base, uint32_t page, const WordList& w) {
#if NIER_MENU_POINTER
  // Called every frame, the row of three and its menu showing too (2026-11-13): only while its list is open
  // (the page's state, +3552: 6 the first word's, 7 the second's; sub_82426118's A).
  {
    const uint32_t st = Be32(base + page + 3552);
    if (st != 6 && st != 7) return;
  }
  const uint32_t count = Be32(base + page + w.count);
  if (count == 0 || count > 64) return;
  const int shown = int(std::min<uint32_t>(count, 5));
  bool act = false;
  const int row = nier::menu_mouse::BookRowUnderMouse(shown, act, 182, w.x0, w.x1);
  // The word taken is the top one (+top) plus the cursor's row (+sel), wrapped past the end (sub_82422720), and
  // the layout draws the highlight on that row (sub_82424AB8) whichever the count: so the pointer moves the
  // cursor over the five rows shown as in any list. Over five, the game's up / down (the wheel) turn the list
  // under the cursor (+top), which stays on its row. (Turning the list to the pointer jumped about, 2026-11-13.)
  if (row < 0) return;
  const uint32_t cur = Be32(base + page + w.sel);
  if (!act || uint32_t(row) == cur) return;
  SetBe32(base + page + w.sel, uint32_t(row));
  Call3(ctx, base, w.layout, page, 0, 0);
  BookSound(ctx, base, 5);
#else
  (void)ctx, (void)base, (void)page, (void)w;
#endif
}
const WordList kWordList1{5384, 9536, 9548, 248, 588, __imp__sub_82424AB8};
const WordList kWordList2{7460, 9540, 9552, 518, 858, __imp__sub_82424D28};
}  // namespace

// The Maps page (measured 2026-11-13). The local map's list (sub_8240BAC0): the byte +3554 its row, 0..7, the
// last one "World Map" (its A opens the world map); rows at y 297 + 39i (centres), World Map at 590, x 135..420.
// The world map's (sub_8240BD60): the byte +3555, 0..5, row 0 "Local Map" at y 166, the others at 225 + 39(i-1).
// A row may be shut (greyed): its byte in the table (0x82047BE4 local, 0x82047BEC world) is <= -2, or >= 0 and
// the area's count (+50936 + 412 * it) is 0; -1 is always open (sub_82409F70 / sub_8240A038, the game's own
// steps). A row taken: the byte, +3558 = 1 (laid out again), sound 5.
extern "C" void __imp__sub_8240BAC0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8240BD60(PPCContext& ctx, uint8_t* base);
namespace {
bool MapRowOpen(const uint8_t* base, uint32_t page, uint32_t table, int row) {
  const int8_t v = int8_t(base[table + uint32_t(row)]);
  if (v <= -2) return false;
  if (v < 0) return true;
  return int32_t(Be32(base + page + 50936 + uint32_t(v) * 412)) > 0;
}
void MapListMouse(PPCContext& ctx, uint8_t* base, uint32_t page, uint32_t sel_byte, uint32_t table, const int* centres,
                  int rows) {
#if NIER_MENU_POINTER
  auto& tm = nier::menu_mouse::title();
  int mx = 0, my = 0;
  const bool inside = nier::menu_mouse::MouseInGame(mx, my);
  int hovered = -1;
  if (inside && mx >= 135 && mx < 420) {
    for (int i = 0; i < rows; ++i)
      if (my >= centres[i] - 19 && my < centres[i] + 20 && MapRowOpen(base, page, table, i)) hovered = i;
  }
  tm.ms = nier::menu_mouse::NowMs();
  tm.hovered = hovered;
  const bool moved = inside && (mx != tm.last_x || my != tm.last_y);
  tm.last_x = mx;
  tm.last_y = my;
  const bool act = moved || (inside && nier::menu_mouse::PointerDown());
  if (!act || hovered < 0 || base[page + sel_byte] == uint8_t(hovered)) return;
  base[page + sel_byte] = uint8_t(hovered);
  base[page + 3558] = 1;
  BookSound(ctx, base, 5);
#else
  (void)ctx, (void)base, (void)page, (void)sel_byte, (void)table, (void)centres, (void)rows;
#endif
}
constexpr int kLocalMapRows[8] = {297, 336, 375, 414, 453, 492, 531, 590};
constexpr int kWorldMapRows[6] = {166, 225, 264, 303, 342, 381};
}  // namespace

REX_HOOK_RAW(sub_8240BAC0) {
  MapListMouse(ctx, base, ctx.r3.u32, 3554, 0x82047BE4, kLocalMapRows, 8);
  __imp__sub_8240BAC0(ctx, base);
}
REX_HOOK_RAW(sub_8240BD60) {
  MapListMouse(ctx, base, ctx.r3.u32, 3555, 0x82047BEC, kWorldMapRows, 6);
  __imp__sub_8240BD60(ctx, base);
}

// Word Edit's row of three (the first word | the second | the target; sub_82426118, measured 2026-11-13): the
// slot +9532 (0..2), left / right step it, then sub_82421DF8 and sub_82422E18 lay it out, sound 5; the byte
// +9560 set once the row is up. On screen x 245 | 500 | 780 | 1080, y 172..214. Its menu (Remove all / Equip
// best / Back to list; sub_824245C8): +9544 (0..2), up / down step it, then sub_82422040, sound 5; it takes
// input once its opening is over (float +2740 >= +2744); rows at y 153 + 39i, x 790..1125.
extern "C" void __imp__sub_82426118(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_824245C8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82421DF8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82422E18(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82422040(PPCContext& ctx, uint8_t* base);
namespace {
// The cell under the pointer among `cells` side by side (x edges) or stacked (y centres); noted for the click.
int WordCellUnderMouse(const int* xs, int ncols, int y0, int y1, const int* ys, int nrows, int x0, int x1,
                       bool& act) {
  auto& tm = nier::menu_mouse::title();
  int mx = 0, my = 0;
  const bool inside = nier::menu_mouse::MouseInGame(mx, my);
  int hovered = -1;
  if (inside && ncols && my >= y0 && my < y1) {
    for (int i = 0; i < ncols; ++i)
      if (mx >= xs[i] && mx < xs[i + 1]) hovered = i;
  }
  if (inside && nrows && mx >= x0 && mx < x1) {
    for (int i = 0; i < nrows; ++i)
      if (my >= ys[i] - 19 && my < ys[i] + 20) hovered = i;
  }
  tm.ms = nier::menu_mouse::NowMs();
  tm.hovered = hovered;
  const bool moved = inside && (mx != tm.last_x || my != tm.last_y);
  tm.last_x = mx;
  tm.last_y = my;
  act = moved || (inside && nier::menu_mouse::PointerDown());
  return hovered;
}
}  // namespace

REX_HOOK_RAW(sub_82426118) {
#if NIER_MENU_POINTER
  const uint32_t page = ctx.r3.u32;
  if (base[page + 9560]) {
    static constexpr int kXs[4] = {245, 500, 780, 1080};
    bool act = false;
    const int cell = WordCellUnderMouse(kXs, 3, 172, 214, nullptr, 0, 0, 0, act);
    if (act && cell >= 0 && Be32(base + page + 9532) != uint32_t(cell)) {
      SetBe32(base + page + 9532, uint32_t(cell));
      Call3(ctx, base, __imp__sub_82421DF8, page, 0, 0);
      Call3(ctx, base, __imp__sub_82422E18, page, 0, 0);
      BookSound(ctx, base, 5);
    }
  }
#endif
  const uint32_t page_r3 = ctx.r3.u32, st0 = Be32(base + ctx.r3.u32 + 3552);
  __imp__sub_82426118(ctx, base);
}
REX_HOOK_RAW(sub_824245C8) {
#if NIER_MENU_POINTER
  const uint32_t page = ctx.r3.u32;
  if (BeFloat(base, page + 2740) >= BeFloat(base, page + 2744)) {
    static constexpr int kYs[3] = {153, 192, 231};
    bool act = false;
    const int row = WordCellUnderMouse(nullptr, 0, 0, 0, kYs, 3, 790, 1125, act);
    if (act && row >= 0 && Be32(base + page + 9544) != uint32_t(row)) {
      SetBe32(base + page + 9544, uint32_t(row));
      Call3(ctx, base, __imp__sub_82422040, page, 0, 0);
      BookSound(ctx, base, 5);
    }
  }
#endif
  __imp__sub_824245C8(ctx, base);
}

REX_HOOK_RAW(sub_824251F8) {
  WordListMouse(ctx, base, ctx.r3.u32, kWordList1);
  __imp__sub_824251F8(ctx, base);
}
REX_HOOK_RAW(sub_824255D8) {
  WordListMouse(ctx, base, ctx.r3.u32, kWordList2);
  __imp__sub_824255D8(ctx, base);
}

// The shops' lists (2026-11-16, measured at the forge: NIER_SHOPLOG + a write watch on the row): the item shop's
// buy and sell lists and the weapon forge's are the same list (shop_plate): its button step (r3 = the list, once a
// frame while it takes input) keeps the row on screen at +12 and the first row shown at +16, 8 rows shown, the
// count at +64; a move goes through sub_824054A8 (up) / sub_82405528 (down), then the list's own redraw (r4 = 0)
// and the menus' move sound (5). The rows ("=_txt_1_item".."8") are drawn from y 113, 30 apart, x 126..~800.
// The pointer moves the row under it the same way; a click (touch: a tap, after the row moved) is the A the menus
// get. A drag up / down scrolls a row a step (the touch's scroll_request), as the game's own up / down at an end.
extern "C" void __imp__sub_823E4F68(PPCContext& ctx, uint8_t* base);  // the forge's list
extern "C" void __imp__sub_823E78A0(PPCContext& ctx, uint8_t* base);  // the shop's lists
extern "C" void __imp__sub_823E66E8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823E4D88(PPCContext& ctx, uint8_t* base);  // their redraws
extern "C" void __imp__sub_823E74A8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823E6408(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_824054A8(PPCContext& ctx, uint8_t* base);  // a list's row up / down
extern "C" void __imp__sub_82405528(PPCContext& ctx, uint8_t* base);
namespace {
thread_local uint32_t t_list_force = 0;  // a quantity arrow clicked: left / right answered "pressed" this frame
void ShopListMouse(PPCContext& ctx, uint8_t* base, uint32_t obj, void (*redraw)(PPCContext&, uint8_t*),
                   bool arrows = false) {
#if NIER_MENU_POINTER
  const uint32_t count = Be32(base + obj + 64);
  if (count == 0 || count > 500) return;
  if (nier::menu_mouse::NowMs() - g_dialog_ms.load() < 200) return;  // a question over the list
  // The list takes input: a menu for the touch too (a finger is the pointer, a drag scrolls; 2026-11-19, the phone's
  // shops did not scroll), and the settings list's tick leaves the drag's steps to it.
  nier::menu_mouse::MarkOutsideMenu(4);
  g_shop_list_ms = nier::menu_mouse::NowMs();
  {  // TEMP (SHOPLOG): which list runs, its row
    static const bool log = std::getenv("NIER_SHOPLOG") != nullptr;
    static uint32_t last = 0;
    if (log && obj != last) {
      last = obj;
      REXLOG_INFO("SHOPLOG lista {:08X} linha {} topo {} itens {}", obj, Be32(base + obj + 12), Be32(base + obj + 16),
                  count);
    }
  }
  auto& tm = nier::menu_mouse::title();
  const uint32_t shown = std::min<uint32_t>(count, 8);
  auto redraw_and_sound = [&] {
    PPCContext saved = ctx;
    ctx.r3.u64 = obj;
    ctx.r4.u64 = 0;
    redraw(ctx, base);
    ctx = saved;
    BookSound(ctx, base, 5);
    tm.moved_ms = nier::menu_mouse::NowMs();
  };
  // A drag: a row a step, through the game's own up / down (they scroll at the ends).
  if (const int steps = nier::menu_mouse::nodes().scroll_request.exchange(0); steps != 0 && count > 1) {
    for (int i = 0; i < std::abs(steps); ++i) {
      PPCContext saved = ctx;
      ctx.r3.u64 = obj + 12;
      ctx.r4.u64 = obj + 16;
      ctx.r5.u64 = 8;
      ctx.r6.u64 = count;
      if (steps > 0) __imp__sub_82405528(ctx, base);
      else __imp__sub_824054A8(ctx, base);
      ctx = saved;
    }
    redraw_and_sound();
  }
  int mx = 0, my = 0;
  const bool inside = nier::menu_mouse::MouseInGame(mx, my);
  int hovered = -1;
  if (inside && mx >= 100 && mx < 820 && my >= 108 && my < 108 + 30 * int(shown)) hovered = (my - 108) / 30;
  // Buy / sell: the quantity arrows on the chosen row (< at ~644, > at ~721): a click is left / right, not A.
  static thread_local bool was_down = false;
  const bool down = nier::menu_mouse::PointerDown();
  const bool pressed = down && !was_down;
  was_down = down;
  if (arrows && hovered >= 0 && uint32_t(hovered) == Be32(base + obj + 12) && (mx >= 618 && mx < 670 || mx >= 696 && mx < 750)) {
    tm.ms = nier::menu_mouse::NowMs();
    tm.hovered = -2;  // the click's own A is eaten (all of it: a touch's A bought one, 2026-11-19)
    tm.last_x = mx;
    tm.last_y = my;
    if (pressed) t_list_force = mx < 680 ? 0x00040004u : 0x00080008u;
    return;
  }
  tm.ms = nier::menu_mouse::NowMs();  // the title's path in MenuMouse::Apply: a click off the rows is nothing
  tm.hovered = hovered;
  const bool moved = inside && (mx != tm.last_x || my != tm.last_y);
  tm.last_x = mx;
  tm.last_y = my;
  if (!moved || hovered < 0 || Be32(base + obj + 12) == uint32_t(hovered)) return;
  SetBe32(base + obj + 12, uint32_t(hovered));
  redraw_and_sound();
#else
  (void)ctx, (void)base, (void)obj, (void)redraw;
#endif
}
}  // namespace

REX_HOOK_RAW(sub_823E4F68) {
  ShopListMouse(ctx, base, ctx.r3.u32, __imp__sub_823E4D88);
  __imp__sub_823E4F68(ctx, base);
}
REX_HOOK_RAW(sub_823E78A0) {
  ShopListMouse(ctx, base, ctx.r3.u32, __imp__sub_823E74A8, true);
  __imp__sub_823E78A0(ctx, base);
  t_list_force = 0;
}
REX_HOOK_RAW(sub_823E66E8) {
  ShopListMouse(ctx, base, ctx.r3.u32, __imp__sub_823E6408, true);
  __imp__sub_823E66E8(ctx, base);
  t_list_force = 0;
}

// The shops' other screens (2026-11-17, found with memory diffs and a write watch at the forge and the item shop):
// the tabs (the item shop's Buy / Sell / Cancel: sub_823E7508, +8 = 0..2; the forge's Forge / Cancel: sub_823E4DE8,
// +8 = 2..3) and the Yes / No questions (forge sub_823E52E0, buy sub_823E76D8, sell sub_823E7DF0: the byte +20 is 1
// on Yes). Their buttons are read through sub_823E3EF8 (pressed), sub_823E3EE8 and sub_823E3F28 (repeating) with a
// mask: A 0x02000007, B 0x02000008, left 0x00040004, right 0x00080008, up 0x00010001, down 0x00020002. The pointer
// walks them the game's way: while the screen's handler runs, the press it needs (left / right toward the choice
// under the pointer) is answered "pressed" for one frame, so the game moves its own cursor, plays its sound and
// redraws. A click on the chosen one is the A the menus get. The buy / sell lists' quantity arrows (on the chosen
// row, x ~644 and ~721) are clicked the same way.
extern "C" void __imp__sub_823E3EF8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823E3EE8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823E3F28(PPCContext& ctx, uint8_t* base);
namespace {
constexpr uint32_t kShopLeft = 0x00040004, kShopRight = 0x00080008;
thread_local uint32_t t_shop_force = 0;  // the mask answered "pressed" while a shop handler runs
bool ShopForced(PPCContext& ctx) {
  if ((t_shop_force && ctx.r4.u32 == t_shop_force) || (t_list_force && ctx.r4.u32 == t_list_force)) {
    ctx.r3.u64 = 1;
    return true;
  }
  return false;
}
// The choice under the pointer among `count` boxes [x0[i], x1[i]) on the band [y0, y1); -1 none. Notes it for the
// click (MenuMouse::Apply's title path: a click off the choices is nothing).
int ShopChoiceUnderPointer(const int* x0, const int* x1, int count, int y0, int y1, bool& moved) {
  auto& tm = nier::menu_mouse::title();
  int mx = 0, my = 0;
  const bool inside = nier::menu_mouse::MouseInGame(mx, my);
  int hovered = -1;
  if (inside && my >= y0 && my < y1)
    for (int i = 0; i < count; ++i)
      if (mx >= x0[i] && mx < x1[i]) hovered = i;
  tm.ms = nier::menu_mouse::NowMs();
  tm.hovered = hovered;
  moved = inside && (mx != tm.last_x || my != tm.last_y);
  tm.last_x = mx;
  tm.last_y = my;
  return hovered;
}
// One step a frame toward `want` from `now` (left lowers it): the press for this frame, 0 none.
uint32_t StepToward(int now, int want) {
  if (want < 0 || want == now) return 0;
  nier::menu_mouse::title().moved_ms = nier::menu_mouse::NowMs();  // a touch's A waits for the move
  return want < now ? kShopLeft : kShopRight;
}
// The target the pointer chose, kept until reached (the handler steps a choice a frame).
struct ShopTarget {
  uint32_t obj = 0;
  int want = -1;
  int steps = 0;  // presses given toward it (a choice never reached is given up: no endless presses)
};
thread_local ShopTarget t_shop_target;

void RunShopScreen(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), int now, const int* x0,
                   const int* x1, int count, int y0, int y1, const int* values) {
#if NIER_MENU_POINTER
  // A choice dialog over it (the shopkeeper's talk: the shop's screens already run under it, and their "off the
  // choices" ate the talk's click, 2026-11-17).
  if (nier::menu_mouse::NowMs() - g_dialog_ms.load() < 200) {
    original(ctx, base);
    return;
  }
  const uint32_t obj = ctx.r3.u32;
  bool moved = false;
  const int hovered = ShopChoiceUnderPointer(x0, x1, count, y0, y1, moved);
  if (t_shop_target.obj != obj) t_shop_target = {obj, -1};
  if (moved && hovered >= 0 && t_shop_target.want != values[hovered]) t_shop_target = {obj, values[hovered], 0};
  if (t_shop_target.want == now || t_shop_target.steps > 6) t_shop_target.want = -1;
  if (t_shop_target.want >= 0) ++t_shop_target.steps;
  t_shop_force = StepToward(now, t_shop_target.want);
#else
  (void)base, (void)now, (void)x0, (void)x1, (void)count, (void)y0, (void)y1, (void)values;
#endif
  original(ctx, base);
  t_shop_force = 0;
}
}  // namespace

REX_HOOK_RAW(sub_823E3EF8) {
  if (!ShopForced(ctx)) __imp__sub_823E3EF8(ctx, base);
}
REX_HOOK_RAW(sub_823E3EE8) {
  if (!ShopForced(ctx)) __imp__sub_823E3EE8(ctx, base);
}
REX_HOOK_RAW(sub_823E3F28) {
  if (!ShopForced(ctx)) __imp__sub_823E3F28(ctx, base);
}

// The quick menus of gameplay (2026-11-17, with the user): items (D-pad up) and the weapons (left / right / down), one
// object each, updated once a frame by sub_82432910 (r3 = the menu; +8 its state, 2 = taking input). Its input
// (sub_82431DD0) asks the player's pad (0x827CD42C) through sub_82103CF0: 0x00010001 up (the row before: node +16),
// 0x00020002 down (the row after: node +12), A 0x02000007, B 0x02000008. The rows: a list from +1904 (the first
// node; +1908 the last, +1920 how many), linked by +12; +32 the node the cursor is on. On screen (1280x720) the rows
// are 48 apart from y 139, x 100..575 (measured 2026-11-17: 5 swords, 2 items). The pointer over a row presses the
// way to it, one row a frame (the game's own move: sound, text, highlight); a click there is A (MenuMouse's title
// path eats the A of a click off the rows).
extern "C" void __imp__sub_82432910(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82103CF0(PPCContext& ctx, uint8_t* base);
namespace {
thread_local uint32_t t_quick_force = 0;  // the mask answered "pressed" while a quick menu runs
thread_local bool t_quick_inside = false;
constexpr uint32_t kPlayerPad = 0x827CD42C, kQuickUp = 0x00010001, kQuickDown = 0x00020002;
constexpr int kQuickY0 = 139, kQuickRowH = 48, kQuickX0 = 100, kQuickX1 = 575, kQuickMaxRows = 10;

void QuickMenuMouse(uint8_t* base, uint32_t menu) {
  t_quick_force = 0;
  if (Be32(base + menu + 8) != 2) return;
  std::vector<uint32_t> rows;
  for (uint32_t n = Be32(base + menu + 1904); n >= 0x40000000u && n < 0xA0000000u && rows.size() < 64; n = Be32(base + n + 12))
    rows.push_back(n);
  const uint32_t cur_node = Be32(base + menu + 32);
  int cur = -1;
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i] == cur_node) cur = int(i);
  if (rows.size() < 1 || cur < 0) return;
  const int shown = int(std::min<size_t>(rows.size(), kQuickMaxRows));
  auto& tm = nier::menu_mouse::title();
  int mx = 0, my = 0;
  const bool inside = nier::menu_mouse::MouseInGame(mx, my);
  int hovered = -1;
  if (inside && mx >= kQuickX0 && mx < kQuickX1 && my >= kQuickY0 && my < kQuickY0 + shown * kQuickRowH)
    hovered = (my - kQuickY0) / kQuickRowH;
  const bool moved = inside && (mx != tm.last_x || my != tm.last_y);
  tm.ms = nier::menu_mouse::NowMs();
  tm.last_x = mx;
  tm.last_y = my;
  // The click's A only on the row the cursor is on (a click on another row moves there first).
  tm.hovered = hovered == cur ? hovered : -1;
  if (hovered >= 0 && hovered != cur && (moved || nier::menu_mouse::PointerDown()))
    t_quick_force = hovered > cur ? kQuickDown : kQuickUp;
}
}  // namespace

REX_HOOK_RAW(sub_82432910) {
#if NIER_MENU_POINTER
  QuickMenuMouse(base, ctx.r3.u32);
  t_quick_inside = true;
  __imp__sub_82432910(ctx, base);
  t_quick_inside = false;
  t_quick_force = 0;
#else
  __imp__sub_82432910(ctx, base);
#endif
}
REX_HOOK_RAW(sub_82103CF0) {
  if (t_quick_inside && t_quick_force && ctx.r3.u32 == kPlayerPad && ctx.r4.u32 == t_quick_force) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82103CF0(ctx, base);
}
// The tabs (1280x720: the item shop's Buy / Sell / Cancel from x 700 / 855 / 1005, the forge's Forge / Cancel split
// at 955; y 160..215). The item shop's Cancel is +8 = 3 (right: 0 -> 1 -> 3 -> 0; there is no 2).
extern "C" void __imp__sub_823E7508(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823E4DE8(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_823E7508) {
  static const int x0[3] = {700, 855, 1005}, x1[3] = {855, 1005, 1180}, values[3] = {0, 1, 3};
  RunShopScreen(ctx, base, __imp__sub_823E7508, int(Be32(base + ctx.r3.u32 + 8)), x0, x1, 3, 160, 215, values);
}
REX_HOOK_RAW(sub_823E4DE8) {
  static const int x0[2] = {720, 955}, x1[2] = {955, 1200}, values[2] = {2, 3};
  RunShopScreen(ctx, base, __imp__sub_823E4DE8, int(Be32(base + ctx.r3.u32 + 8)), x0, x1, 2, 160, 215, values);
}
// The questions: Yes ~568, No ~712, y ~598 (the forge's "Proceed with the forging?"). Choice 0 = Yes = byte 1.
extern "C" void __imp__sub_823E52E0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823E76D8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823E7DF0(PPCContext& ctx, uint8_t* base);
namespace {
void ShopQuestion(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*)) {
  static const int x0[2] = {470, 640}, x1[2] = {640, 810}, values[2] = {0, 1};
  const int now = base[ctx.r3.u32 + 20] ? 0 : 1;  // 0 Yes, 1 No
  RunShopScreen(ctx, base, original, now, x0, x1, 2, 570, 630, values);
}
}  // namespace
REX_HOOK_RAW(sub_823E52E0) { ShopQuestion(ctx, base, __imp__sub_823E52E0); }
REX_HOOK_RAW(sub_823E76D8) { ShopQuestion(ctx, base, __imp__sub_823E76D8); }
REX_HOOK_RAW(sub_823E7DF0) { ShopQuestion(ctx, base, __imp__sub_823E7DF0); }
