// nier - on-screen touch controls (Android): Grimoire Weiss's sigils as buttons.
//
// The buttons press the Xbox 360 buttons NieR Gestalt reads (keyboard_binds.h has what each does): A jump,
// B action, X attack, Y heavy attack, RT evade, LT defend, LB/RB magic, Start, Back, the d-pad (items and
// weapons) and R3 (camera recentre). A finger that lands on the left side away from the buttons is a floating
// stick; on the right side it turns the camera. The art is the game's own (game_assets.h, read from the user's
// data, never shipped): the sigils of Grimoire Weiss's word screen, cut to their circle, and the 360 button
// glyphs.
//
// Shown by "Controles na tela" (Options > Control Options on Android): automatic (only without a physical
// pad), always, or off. "Editar posições" opens the editor: drag a button to move it, + / - change the
// selected one's size. The layout is kept in touch_controls.txt next to nier.toml.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_touch.h>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "mods.h"
#include "pc_options.h"
#include "pc_text.h"
#include "title_carousel.h"

namespace nier::touch {

using rex::X_RESULT;
using rex::X_STATUS;

enum : uint16_t { kPadL3 = 0x0040, kPadR3 = 0x0080, kPadX = 0x4000, kPadY = 0x8000 };

// Windows (a PC with a touch screen): a touch also moves the mouse and clicks, which already drive the menus; the
// buttons and the stick are this file's, the menus the mouse's.
#if defined(_WIN32)
constexpr bool kOsPointer = true;
#else
constexpr bool kOsPointer = false;
#endif

// kWeapons opens the weapons (the d-pad's left / right / down, its popup buttons) around itself; one of them, or
// the button again, closes them.
enum class Kind : uint8_t { kButton, kLeftTrigger, kRightTrigger, kStick, kWeapons };

struct Control {
  const char* id;
  Kind kind;
  uint16_t bit;      // kButton
  const char* icon;  // menu art (MenuArtWanted), or nullptr: the glyph alone
  int glyph;         // cell of COMMON/bt_icon_360_bt_icon_D5 (32x32, 4 a row), -1 none
  float x, y;        // centre, fractions of the screen's width and height (a popup's: placed when it opens)
  float size;        // diameter, fraction of the screen's height
  bool popup = false;  // one of the weapons: only while they are open, not in the editor
  int slot = -1;       // ability slot (0 LB, 1 RB, 2 LT, 3 RT): its sigil is what is set there, none: hidden
  int weapon = -1;     // a weapon type (0 one-handed sword, 1 two-handed, 2 spear): only once the player has one
};

// The player's data (NieR Replicant ver.1.22's Player struct, Acurisu's notes; the same in Gestalt's 0xC30-byte
// save): a static of the XEX, the live copy (the one saved follows at +0xC30). Big-endian.
constexpr uint32_t kPlayer = 0x84CC6BA0u;
constexpr uint32_t kPlayerWeapons = 140;    // selected one-handed, two-handed sword, spear (int; -1: none owned)
constexpr uint32_t kPlayerAbilities = 160;  // ability_lb, _rb, _lt, _rt (int each)

// What the game lets the buttons do now (read each poll).
enum class Context : uint8_t {
  kGameplay,
  kStartOnly,  // the opening videos, logos and "Press START": Start and A
  kMenu,       // list menus (title, pause): A, B and the stick
  kBook,       // Grimoire Weiss's book: also LB / RB, which turn its pages
  kCutscene,   // a full cutscene (827CD450 and 8413676C == 0): Start, which skips it
  kTalk,       // a talk or an interaction runs (827CD450 == 0 alone): A and B, which move its text on
  kChoice,     // a talk's choices in a column (Jakob's...): the stick (up / down as the d-pad), A and B; no pointer
  kFishing,    // fishing (Nier's animations 272..279, _01_FISHING / FISHGET): the stick, A and B; a touch elsewhere nothing
};

// The sigil of each ability (Ability enum: 0 none, 1 defend, 2 evade, 3 Dark Blast, 4 Dark Phantasm, 5 Dark Hand,
// 6 Dark Lance, 7 Dark Whirlwind, 8 Dark Gluttony, 9 Dark Wall, 10 Dark Execution).
inline const char* AbilityIcon(uint32_t ability) {
  static const char* const kIcons[] = {
      nullptr,
      "menu_word_edit_target_p_guard",
      "menu_word_edit_target_p_dodge",
      "menu_word_edit_target_m_vulcan",
      "menu_word_edit_target_m_vision",
      "menu_word_edit_target_m_hand",
      "menu_word_edit_target_m_lance",
      "menu_word_edit_target_m_round",
      "menu_word_edit_target_m_drain",
      "menu_word_edit_target_m_wall",
      "menu_word_edit_target_m_ground",
  };
  return ability < std::size(kIcons) ? kIcons[ability] : nullptr;
}

// The sigil art: a paper tag with the sigil in a circle, centred here (fractions of the 255x256 image).
constexpr float kSigilU0 = 33.0f / 255, kSigilV0 = 25.0f / 256, kSigilU1 = 223.0f / 255, kSigilV1 = 215.0f / 256;

struct Layout {
  std::vector<Control> controls;
  int mode = 1;          // 0 off, 1 automatic (no physical pad), 2 always
  float opacity = 0.7f;  // 0.1 .. 1
  float scale = 1.0f;    // all buttons, 0.5 .. 2
  float camera = 1.0f;   // camera speed, 0.25 .. 4
};

// Default places for a screen of this width / height.
inline Layout DefaultLayout(float aspect) {
  aspect = std::max(aspect, 1.0f);
  auto L = [aspect](float d) { return d / aspect; };         // d screen heights from the left edge
  auto R = [aspect](float d) { return 1.0f - d / aspect; };  // ... from the right edge
  Layout l;
  l.controls = {
      {"stick", Kind::kStick, 0, nullptr, -1, L(0.28f), 0.70f, 0.34f},
      {"a", Kind::kButton, kPadA, nullptr, 0, R(0.30f), 0.84f, 0.15f},
      {"b", Kind::kButton, kPadB, nullptr, 1, R(0.14f), 0.69f, 0.15f},
      {"x", Kind::kButton, kPadX, "menu_word_edit_target_w_ssword", 2, R(0.46f), 0.69f, 0.15f},
      {"y", Kind::kButton, kPadY, "menu_word_edit_target_w_lsword", 3, R(0.30f), 0.54f, 0.15f},
      {"rt", Kind::kRightTrigger, 0, "menu_word_edit_target_p_dodge", 5, R(0.12f), 0.44f, 0.13f, false, 3},
      {"lt", Kind::kLeftTrigger, 0, "menu_word_edit_target_p_guard", 4, R(0.48f), 0.44f, 0.13f, false, 2},
      {"lb", Kind::kButton, kPadLB, "menu_word_edit_target_m_lance", 6, R(0.38f), 0.28f, 0.13f, false, 0},
      {"rb", Kind::kButton, kPadRB, "menu_word_edit_target_m_hand", 7, R(0.22f), 0.28f, 0.13f, false, 1},
      {"up", Kind::kButton, kPadUp, "icn_item_drug", 18, L(0.12f), 0.30f, 0.12f},
      {"weapons", Kind::kWeapons, 0, "icn_we_fencing", -1, L(0.30f), 0.30f, 0.12f},
      {"left", Kind::kButton, kPadLeft, "icn_arms_l_sword", 17, 0, 0, 0.11f, true, -1, 1},
      {"right", Kind::kButton, kPadRight, "icn_arms_s_sword", 16, 0, 0, 0.11f, true, -1, 0},
      {"down", Kind::kButton, kPadDown, "icn_arms_spear", 19, 0, 0, 0.11f, true, -1, 2},
      {"back", Kind::kButton, kPadBack, nullptr, 12, 0.5f - L(0.08f), 0.07f, 0.09f},
      {"start", Kind::kButton, kPadStart, nullptr, 13, 0.5f + L(0.08f), 0.07f, 0.09f},
      {"r3", Kind::kButton, kPadR3, nullptr, 11, R(0.08f), 0.10f, 0.09f},
      // The menus' only button (B: back); a touch anywhere else in a menu is the pointer (menu_mouse.h).
      {"menu_back", Kind::kButton, kPadB, nullptr, -1, L(0.10f), 0.10f, 0.11f},
  };
  return l;
}

// The art the buttons use (added to MenuArtWanted).
inline const std::vector<std::string>& MenuIcons() {
  static const std::vector<std::string> v = {
      "menu_word_edit_target_w_ssword", "menu_word_edit_target_w_lsword", "menu_word_edit_target_p_dodge",
      "menu_word_edit_target_p_guard",  "menu_word_edit_target_m_lance",  "menu_word_edit_target_m_hand",
      "menu_word_edit_target_m_vulcan", "menu_word_edit_target_m_vision", "menu_word_edit_target_m_round",
      "menu_word_edit_target_m_drain",  "menu_word_edit_target_m_wall",   "menu_word_edit_target_m_ground"};
  return v;
}
inline const std::vector<std::string>& CommonIcons() {
  static const std::vector<std::string> v = {"icn_item_drug", "icn_arms_l_sword", "icn_arms_s_sword", "icn_arms_spear",
                                             "icn_we_fencing"};
  return v;
}

struct Finger {
  SDL_FingerID id;
  float x, y;  // fractions of the window
};

struct State {
  std::mutex mutex;
  Layout layout;
  bool loaded = false;
  std::filesystem::path file;
  std::atomic<float> aspect{2.0f};   // of the window, from the drawing
  std::atomic<bool> physical_pad{false};
  std::atomic<bool> editing{false};
  // Read by the drawing (under the mutex).
  uint32_t pressed = 0;  // bit per control
  bool stick_active = false;
  float stick_ax = 0, stick_ay = 0, stick_kx = 0, stick_ky = 0;  // anchor and knob, fractions of the window
  // Fingers by role (input thread).
  std::map<SDL_FingerID, int> roles;  // -1 stick, -2 camera, -3 nothing, else button
  // The camera finger: its last place and when it changed (ns), and its speed (screen heights a second, smoothed).
  float camera_x = 0, camera_y = 0;
  uint64_t camera_ns = 0;
  float camera_vx = 0, camera_vy = 0;
  bool weapons_open = false;
  int pointer_x = -1, pointer_y = -1;  // the menu's pointer (game space), kept after the finger lifts
  uint64_t pointer_until = 0;          // a quick tap's pointer stays down until then (ms)
  uint64_t pointer_from = 0;           // when the pointer's touch began (ms): A only from 50 ms on, after the
                                       // menu's hook moved the row under it (else A took the old row)
  bool pointer_a_given = false;        // this touch's A was pressed
  // A finger dragged up / down a menu scrolls its list a row each ~row height (no pointer move, no A).
  float drag_fy = 0, last_fx = 0, last_fy = 0;
  bool dragging = false;
  uint32_t pointer_a_frame = 0;  // the game's frame the pointer's A began (it lasts 3 of them)
  int choice_dir = 0;            // kChoice: the stick's way (1 up, -1 down), its next step and the press
  uint64_t choice_next = 0, choice_until = 0;
  uint16_t choice_bit = 0;
  int drag_rows = 0;
  // The performance meter's finger (role -5): where it began, the meter's offset then, whether it dragged.
  float panel_fx = 0, panel_fy = 0, panel_dx0 = 0, panel_dy0 = 0;
  bool panel_dragged = false;
  uint64_t confirm_until = 0;  // a quick tap's confirm (role -6) stays pressed until then (ms)
  uint32_t abilities[4] = {};  // what LB, RB, LT, RT do now (kPlayer), read each poll
  bool weapons[3] = {};        // weapon types the player has (kPlayerWeapons)
  Context context = Context::kGameplay;
  int focus = -1;              // the control of a held charging magic (HeldMagic): the only one drawn
  struct Tap {
    SDL_FingerID id;
    float x, y;
  };
  // Finger-downs since the last poll (quick taps), under their own lock: SDL calls WatchTaps holding its
  // touch lock, so this lock is never held around an SDL call (the 'mutex' + SDL lock order ANR'd).
  std::mutex taps_mutex;
  std::vector<Tap> taps;
  std::vector<uint64_t> hold_until;           // per control, ms: a quick tap's press lasts this long
  std::atomic<bool> watching{false};
  // Nier Plus (2026-11-15): the code mods' icons (mods.cpp panels), a column on the screen's right edge. Their
  // circles (x, y, radius; fractions of the window, radius of its height), set by the drawing; a finger on one
  // opens its panel and never reaches the game. Under taps_mutex (WatchTaps reads them).
  struct ModIcon {
    int panel;
    float x, y, r;
  };
  std::vector<ModIcon> mod_icons;
  std::atomic<int> mod_toggle{-1};  // an icon touched: the panel the drawing opens or closes
  bool OnModIcon(float x, float y, float aspect, int* panel = nullptr) const {
    for (const ModIcon& m : mod_icons) {
      const float dx = (x - m.x) * aspect, dy = y - m.y;
      if (dx * dx + dy * dy <= m.r * m.r * 1.3f) {
        if (panel) *panel = m.panel;
        return true;
      }
    }
    return false;
  }

  bool Visible() {
    std::lock_guard lock(mutex);
    return VisibleLocked();
  }
  bool VisibleLocked() const {
#if defined(_WIN32)
    // A PC with a touch screen (2026-11-17): automatic shows them once a finger has touched it (no pad in use).
    return layout.mode == 2 || (layout.mode == 1 && !physical_pad.load() && touched.load());
#else
    return layout.mode == 2 || (layout.mode == 1 && !physical_pad.load());
#endif
  }
  std::atomic<bool> touched{false};  // a finger has been on the screen (this run)
};

inline State& state() {
  static State s;
  return s;
}

inline void SaveLocked(State& s) {
  if (s.file.empty()) return;
  if (std::FILE* f = std::fopen(s.file.string().c_str(), "w")) {
    std::fprintf(f, "mode %d\nopacity %.3f\nscale %.3f\ncamera %.3f\n", s.layout.mode, s.layout.opacity,
                 s.layout.scale, s.layout.camera);
    for (const Control& c : s.layout.controls) {
      if (c.popup)
        std::fprintf(f, "c %s 0 0 %.4f\n", c.id, c.size);  // placed when the weapons open
      else
        std::fprintf(f, "c %s %.4f %.4f %.4f\n", c.id, c.x, c.y, c.size);
    }
    std::fclose(f);
  }
}

// Defaults for this screen, then what the file changed.
inline void LoadLocked(State& s) {
  s.layout = DefaultLayout(s.aspect.load());
  s.loaded = true;
  s.hold_until.assign(s.layout.controls.size(), 0);
  if (s.file.empty()) return;
  std::FILE* f = std::fopen(s.file.string().c_str(), "r");
  if (!f) return;
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    char id[32] = {};
    float a = 0, b = 0, c = 0;
    int m = 0;
    if (std::sscanf(line, "mode %d", &m) == 1) s.layout.mode = std::clamp(m, 0, 2);
    else if (std::sscanf(line, "opacity %f", &a) == 1) s.layout.opacity = std::clamp(a, 0.1f, 1.0f);
    else if (std::sscanf(line, "scale %f", &a) == 1) s.layout.scale = std::clamp(a, 0.5f, 2.0f);
    else if (std::sscanf(line, "camera %f", &a) == 1) s.layout.camera = std::clamp(a, 0.25f, 4.0f);
    else if (std::sscanf(line, "c %31s %f %f %f", id, &a, &b, &c) == 4) {
      for (Control& ctl : s.layout.controls) {
        if (std::strcmp(ctl.id, id) == 0) {
          if (!ctl.popup) {
            ctl.x = std::clamp(a, 0.0f, 1.0f);
            ctl.y = std::clamp(b, 0.0f, 1.0f);
          }
          ctl.size = std::clamp(c, 0.04f, 0.6f);
        }
      }
    }
  }
  std::fclose(f);
}

inline void EnsureLoadedLocked(State& s) {
  if (!s.loaded) LoadLocked(s);
}

// Options rows (video_options_menu.cpp) change the layout through these.
inline void Change(const std::function<void(Layout&)>& f) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  EnsureLoadedLocked(s);
  f(s.layout);
  SaveLocked(s);
}
inline Layout Current() {
  State& s = state();
  std::lock_guard lock(s.mutex);
  EnsureLoadedLocked(s);
  return s.layout;
}
inline void RestoreDefaults() {
  State& s = state();
  std::lock_guard lock(s.mutex);
  const int mode = s.loaded ? s.layout.mode : 1;
  s.layout = DefaultLayout(s.aspect.load());
  s.layout.mode = mode;
  s.hold_until.assign(s.layout.controls.size(), 0);
  s.loaded = true;
  SaveLocked(s);
}

// Whether a control is on screen now, and the art it shows (`icon`: its own, a sigil, or nullptr for the glyph).
// The editor shows every one (but the weapons) with its own art.
inline bool ShownLocked(const State& s, const Control& c, const char** icon) {
  *icon = c.icon;
  if (s.editing.load()) return !c.popup;
  int weapons = 0;
  for (bool w : s.weapons) weapons += w ? 1 : 0;
  const bool back = std::strcmp(c.id, "menu_back") == 0;
  switch (s.context) {
    case Context::kStartOnly:
      return std::strcmp(c.id, "start") == 0 || std::strcmp(c.id, "a") == 0;
    case Context::kCutscene:
      return std::strcmp(c.id, "start") == 0;
    case Context::kTalk:
      return std::strcmp(c.id, "a") == 0 || std::strcmp(c.id, "b") == 0;
    case Context::kChoice:
    case Context::kFishing:
      return std::strcmp(c.id, "a") == 0 || std::strcmp(c.id, "b") == 0 || c.kind == Kind::kStick;
    case Context::kBook:
    case Context::kMenu:
      // The menus are touched as they are (a row, a tab, an arrow: menu_mouse.h); back is the one button
      // (2026-11-13, the user's plan: no A / B / stick on screen in menus).
      return back;
    case Context::kGameplay:
      break;
  }
  if (back) return false;
  if (c.slot >= 0) {
    *icon = AbilityIcon(s.abilities[c.slot]);
    return *icon != nullptr;
  }
  // One type: the button takes it out at once (TouchedLocked); more: it opens them.
  if (c.kind == Kind::kWeapons) return weapons >= 1;
  if (c.popup) return s.weapons_open && c.weapon >= 0 && s.weapons[c.weapon];
  return true;
}

// The control under (x, y) (fractions of the window), or -1. Generous: a thumb misses by a little.
inline int HitLocked(const State& s, float x, float y) {
  const float aspect = s.aspect.load();
  int best = -1;
  float best_d = 1e9f;
  for (size_t i = 0; i < s.layout.controls.size(); ++i) {
    const Control& c = s.layout.controls[i];
    const char* icon;
    if (c.kind == Kind::kStick || !ShownLocked(s, c, &icon)) continue;
    const float dx = (x - c.x) * aspect, dy = y - c.y;  // in screen heights
    const float d = std::sqrt(dx * dx + dy * dy);
    const float r = c.size * s.layout.scale * 0.5f * 1.15f;
    if (d <= r && d < best_d) {
      best = int(i);
      best_d = d;
    }
  }
  return best;
}

inline std::vector<Finger> Fingers() {
  std::vector<Finger> out;
  int devices = 0;
  SDL_TouchID* ids = SDL_GetTouchDevices(&devices);
  for (int d = 0; ids && d < devices; ++d) {
    int n = 0;
    SDL_Finger** f = SDL_GetTouchFingers(ids[d], &n);
    for (int i = 0; f && i < n; ++i) out.push_back({f[i]->id, f[i]->x, f[i]->y});
    SDL_free(f);
  }
  SDL_free(ids);
  return out;
}

// Quick taps: a finger can come and go between two polls of the pad (33 ms at 30 fps).
inline bool SDLCALL WatchTaps(void*, SDL_Event* e) {
  if (e->type == SDL_EVENT_FINGER_DOWN) {
    State& s = state();
    int panel = -1;
    {
      std::lock_guard lock(s.taps_mutex);
      if (!s.OnModIcon(e->tfinger.x, e->tfinger.y, s.aspect.load(), &panel) && s.taps.size() < 32)
        s.taps.push_back({e->tfinger.fingerID, e->tfinger.x, e->tfinger.y});
    }
    // A mod's icon: its panel, opened by the drawing (no SDL call from inside SDL's own lock here).
    if (panel >= 0) s.mod_toggle = panel;
  }
  return true;
}

// The weapons in a fan above and to the right of their button.
inline void PlaceWeaponsLocked(State& s) {
  const float aspect = s.aspect.load();
  const Control* menu = nullptr;
  for (const Control& c : s.layout.controls) {
    if (c.kind == Kind::kWeapons) menu = &c;
  }
  if (!menu) return;
  int owned = 0;
  for (const Control& c : s.layout.controls) owned += c.popup && c.weapon >= 0 && s.weapons[c.weapon] ? 1 : 0;
  // Degrees, 0 = right, -90 = up: the weapons the player has, spread over the arc.
  const float three[] = {-150.0f, -90.0f, -30.0f}, two[] = {-125.0f, -55.0f}, one[] = {-90.0f};
  const float* angles = owned >= 3 ? three : owned == 2 ? two : one;
  const int count = std::max(1, std::min(owned, 3));
  int k = 0;
  for (Control& c : s.layout.controls) {
    if (!c.popup || c.weapon < 0 || !s.weapons[c.weapon] || k >= count) continue;
    const float d = (menu->size + c.size) * 0.5f * s.layout.scale * 1.12f;  // screen heights
    const float a = angles[k++] * 3.14159265f / 180.0f;
    c.x = std::clamp(menu->x + std::cos(a) * d / aspect, 0.02f, 0.98f);
    c.y = std::clamp(menu->y + std::sin(a) * d, 0.03f, 0.97f);
  }
}

// A button touched: the weapons' button opens or closes them; a weapon closes them when its finger goes.
inline void TouchedLocked(State& s, int hit, uint64_t now) {
  const Control& c = s.layout.controls[size_t(hit)];
  if (c.kind == Kind::kWeapons) {
    int owned = 0, only = -1;
    for (size_t i = 0; i < s.layout.controls.size(); ++i) {
      const Control& w = s.layout.controls[i];
      if (w.popup && w.weapon >= 0 && s.weapons[w.weapon]) {
        ++owned;
        only = int(i);
      }
    }
    // Fewer than the three kinds (2026-11-19, the user: the button did nothing with the short swords alone): the
    // d-pad's left / right / down equip a kind only with all three (weapon_switch.h); before, the game's own weapons
    // list (d-pad down) opens, with the swords there are.
    if (owned < 3) {
      (void)only;
      s.weapons_open = false;
      for (size_t i = 0; i < s.layout.controls.size(); ++i)
        if (s.layout.controls[i].bit == kPadDown && i < s.hold_until.size()) s.hold_until[i] = now + 80;
      return;
    }
    s.weapons_open = !s.weapons_open;
    if (s.weapons_open) PlaceWeaponsLocked(s);
  }
}

// A finger in the game's 1280x720 space, the picture fitted to the window (black bars on) or stretched.
inline void ToGameSpace(float fx, float fy, float aspect, int& x, int& y) {
  // As present.cpp's FitFrame: the frame covers the screen, up to 15% of it trimmed off the long axis's other
  // edges (a 20:9 phone: its top and bottom), the rest fitted with bars.
  static const float kGame = 1280.0f / 720.0f;
  const std::string mode = rex::cvar::GetFlagByName("nier_screen_fit");  // present.cpp
  if (mode == "stretch") {
    x = int(fx * 1280.0f);
    y = int(fy * 720.0f);
    return;
  }
  // "wide", "widereal": the frame is stretched over the screen, but the menus are squeezed back into its middle 16:9
  // (video_options_menu.cpp, SqueezeHud), where bars would put them.
  const float kMaxTrim = mode == "bars" || mode == "wide" || mode == "widereal" ? 0.0f : 0.15f;
  float vis_x = 1.0f, vis_y = 1.0f;  // the share of the frame shown on each axis
  if (aspect <= 0.0f) aspect = kGame;
  if (kGame > aspect)
    vis_x = std::max(aspect / kGame, 1.0f - kMaxTrim);
  else
    vis_y = std::max(kGame / aspect, 1.0f - kMaxTrim);
  const float shown = kGame * vis_x / vis_y;  // the shape of the part shown
  const float sw = shown > aspect ? 1.0f : shown / aspect;  // its share of the screen
  const float sh = shown > aspect ? aspect / shown : 1.0f;
  const float u = 0.5f + (fx - 0.5f) / sw * vis_x;
  const float v = 0.5f + (fy - 0.5f) / sh * vis_y;
  x = int(u * 1280.0f);
  y = int(v * 720.0f);
}

// A stick length past the game's dead zone (about a quarter of the way, as XInput's): any movement turns the camera.
inline float PastDeadZone(float m) {
  return 0.26f + 0.74f * std::min(m, 1.0f);
}

inline uint32_t GuestBe32(const uint8_t* gb, uint32_t address) {
  const uint8_t* p = gb + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// What LB, RB, LT and RT do now; a value past the enum (data not loaded yet) is none.
inline void ReadAbilitiesLocked(State& s) {
  const uint8_t* gb = pc_options_state().guest_base.load();
  for (int k = 0; k < 4; ++k) {
    const uint32_t v = gb ? GuestBe32(gb, kPlayer + kPlayerAbilities + 4 * k) : 0;
    s.abilities[k] = v <= 10 ? v : 0;
  }
  for (int k = 0; k < 3; ++k) {
    s.weapons[k] = gb && GuestBe32(gb, kPlayer + kPlayerWeapons + 4 * k) != 0xFFFFFFFFu;
  }
  const auto& game = pc_options_state();
  // 1.5 s, not MenuSeenRecently's 0.3 s (2026-11-14): the title list and the Yes/No dialogs mark themselves only
  // while still (not while they animate) and a phone's hitch passed 0.3 s; the context fell to "cutscene" (the
  // flag is 0 there too) and every tap was a confirm: Continue at random, Yes/No wrong.
  if (nier::menu_mouse::NowMs() - nier::menu_mouse::outside_menu().ms.load() < 1500) {
    // The title screen's lists, choice dialogs, save lists: drawn by their own objects, not a list menu.
    s.context = Context::kMenu;
  } else if (game.BeforeTitleMenu()) {
    s.context = Context::kStartOnly;
  } else if (game.open || game.native_page || game.ach_open ||
             (gb && *reinterpret_cast<const volatile uint8_t*>(gb + 0x82757AB1) != 0)) {
    // A list menu (title, pause and its pages): GameInMenu's other byte toggles in some gameplay areas, and
    // the buttons went with it.
    s.context = game.BookOpen() ? Context::kBook : Context::kMenu;
  } else if (gb && *reinterpret_cast<const volatile uint8_t*>(gb + 0x827CD450) == 0) {
    // The game's own gate of Nier's controls: byte 36 of the player's input (0x827CD42C; the jump, sub_8222D908, and
    // the moves read the stick only while it is set). 0 from an interact press until the talk (a door, an item) is
    // over, and in cutscenes. 8413676C (FLAGSCAN, 2026-11-13: 0 in a full cutscene) tells those apart only with this
    // one: alone it flickered in Popola's library and stayed 0 in her room with the player walking (2026-11-15).
    s.context = *reinterpret_cast<const volatile uint8_t*>(gb + 0x8413676C) == 0 ? Context::kCutscene : Context::kTalk;
  } else {
    s.context = Context::kGameplay;
  }
  // A talk's column of choices taking input (video_options_menu.cpp, 2026-11-19: touching them picked at random;
  // the user's choice: the stick there instead).
  if (s.context != Context::kMenu && s.context != Context::kBook &&
      nier::menu_mouse::NowMs() - nier::menu_mouse::choice_ms().load() < 600)
    s.context = Context::kChoice;
  // Fishing (2026-11-20, the user: no button to fish, the game stuck): Nier's animation (+0x9D8, the index in his table)
  // is one of the fishing ones, 272..279; its controls are the stick and A (and B), not a tap's confirm.
  if (s.context != Context::kMenu && s.context != Context::kBook && gb) {
    constexpr uint32_t kNierObj = 0x848BE4B0u;
    if (GuestBe32(gb, kNierObj + 3472) == 0x82740048u) {
      const uint32_t anim = GuestBe32(gb, kNierObj + 0x9D8);
      if (anim >= 272 && anim <= 279) s.context = Context::kFishing;
    }
  }
  if (s.context != Context::kGameplay) s.weapons_open = false;
  // TEMP (TOUCHLOG, 2026-11-15: always on, the buttons flicker in Popola's library and go in her room): each change
  // with what decided it.
  static Context logged = Context::kGameplay;
  static int log_lines = 0;
  static uint64_t log_ms = 0;
  const uint64_t log_now = nier::menu_mouse::NowMs();
  if ((s.context != logged || (s.context != Context::kGameplay && log_now - log_ms > 3000)) && log_lines < 400) {
    logged = s.context;
    log_ms = log_now;
    ++log_lines;
    // Nier's object (mods/sdk/nier_mod.h): its action (+0x9D8: 0x87 on the ground, 0x89 / 0x8A jumps).
    constexpr uint32_t kNier = 0x848BE4B0u;
    const bool nier = gb && GuestBe32(gb, kNier + 3472) == 0x82740048u;
    REXLOG_INFO("TOUCHLOG nier {} acao {:X}", nier, nier ? GuestBe32(gb, kNier + 0x9D8) : 0);
    REXLOG_INFO("TOUCHLOG contexto {} (menu fora {} ms fonte {}, antes do titulo {}, open {} page {} ach {}, 82757AB1 {}, "
                "8413676C {}, 827CD450 {})",
                int(s.context), nier::menu_mouse::NowMs() - nier::menu_mouse::outside_menu().ms.load(),
                nier::menu_mouse::outside_menu().source.load(), game.BeforeTitleMenu(), game.open.load(), game.native_page.load(), game.ach_open.load(),
                gb ? gb[0x82757AB1] : 0, gb ? gb[0x8413676C] : 0, gb ? gb[0x827CD450] : 0);
  }
}

// The screens where a touch off the buttons confirms (2026-11-13, the user's plan: "System data loaded", the
// save loaded notice, "Press START" took Start or Back only): the opening, cutscenes and talks.
inline bool ConfirmsLocked(const State& s) {
  return s.context == Context::kStartOnly || s.context == Context::kCutscene || s.context == Context::kTalk;
}

// Magic held while it charges, the player standing still (Dark Lance): only its button stays on screen.
constexpr uint32_t kAbilityDarkLance = 6;
inline bool HeldMagic(uint32_t ability) { return ability == kAbilityDarkLance; }
// Adds the touches to the pad the game reads (input thread, each poll).
inline void Poll(rex::input::X_INPUT_GAMEPAD& g) {
  State& s = state();
  // SDL first, without 'mutex' (SDL holds its own locks around WatchTaps on the UI thread).
  if (!s.watching.exchange(true)) SDL_AddEventWatch(WatchTaps, nullptr);
  std::vector<Finger> fingers = Fingers();
  std::vector<State::Tap> taps;
  {
    std::lock_guard taps_lock(s.taps_mutex);
    taps.swap(s.taps);
    // A finger on a mod's icon is the icon's alone (WatchTaps opened its panel).
    const float aspect = s.aspect.load();
    std::erase_if(fingers, [&](const Finger& f) { return s.OnModIcon(f.x, f.y, aspect); });
  }
  // A finger on the screen: touch is the device in use (the buttons' icons follow, game_text_hooks.cpp).
  if (!fingers.empty() || !taps.empty()) {
    pc_options_state().touch_ms = nier::menu_mouse::NowMs();
    s.touched = true;
  }
  const uint64_t now = SDL_GetTicks();
  std::lock_guard lock(s.mutex);
  EnsureLoadedLocked(s);
  ReadAbilitiesLocked(s);
  // The title's cards (title_carousel.cpp): every finger not on one of the buttons (or already one's) is theirs.
  if (!s.editing.load() && nier::title_carousel::Showing()) {
    const float aspect = s.aspect.load();
    auto theirs = [&](float x, float y) { return !s.VisibleLocked() || HitLocked(s, x, y) < 0; };
    std::vector<nier::title_carousel::Touch> touches;
    std::vector<Finger> kept;
    for (const Finger& f : fingers) {
      if (s.roles.count(f.id) || !theirs(f.x, f.y)) kept.push_back(f);
      else touches.push_back({uint64_t(f.id), f.x, f.y});
    }
    nier::title_carousel::Fingers(touches.data(), int(touches.size()), aspect);
    std::erase_if(taps, [&](const State::Tap& t) {
      for (const Finger& f : fingers)
        if (f.id == t.id) return !s.roles.count(f.id) && theirs(f.x, f.y);  // a finger still down: handled above
      if (!theirs(t.x, t.y)) return false;
      nier::title_carousel::Tap(t.x, t.y, aspect);
      return true;
    });
    fingers.swap(kept);
  } else {
    nier::title_carousel::Fingers(nullptr, 0, s.aspect.load());
  }
  // The Jukebox page open: its own touches (ImGui) only. Its layer sliders lie where the face buttons are, and a
  // finger on one pressed X, the page's pause (2026-11-17: moving a layer paused the song).
  if (!s.VisibleLocked() || s.editing.load() || pc_options_state().jukebox_open.load()) {
    // The buttons hidden (a controller in use, "always" off): the performance meter still moves and shrinks
    // under a finger (2026-11-13: with a Bluetooth pad, or the handheld's own, it stayed put).
    auto& panel = nier::menu_mouse::perf_panel();
    for (const State::Tap& t : taps) {
      bool present = s.roles.count(t.id) != 0;
      for (const Finger& f : fingers) present |= f.id == t.id;
      if (!present && panel.Has(t.x, t.y)) panel.mini = !panel.mini.load();
    }
    for (auto it = s.roles.begin(); it != s.roles.end();) {
      bool still = false;
      for (const Finger& f : fingers) still |= f.id == it->first;
      if (!still && it->second == -5 && !s.panel_dragged) panel.mini = !panel.mini.load();
      it = still && it->second == -5 ? std::next(it) : s.roles.erase(it);
    }
    for (const Finger& f : fingers) {
      auto role = s.roles.find(f.id);
      if (role == s.roles.end()) {
        if (!panel.Has(f.x, f.y)) continue;
        s.panel_fx = f.x;
        s.panel_fy = f.y;
        s.panel_dx0 = panel.dx.load();
        s.panel_dy0 = panel.dy.load();
        s.panel_dragged = false;
        s.roles.emplace(f.id, -5);
        continue;
      }
      const float mx = f.x - s.panel_fx, my = f.y - s.panel_fy, a = s.aspect.load();
      if (!s.panel_dragged && mx * mx * a * a + my * my > 0.02f * 0.02f) s.panel_dragged = true;
      if (s.panel_dragged) {
        panel.dx = s.panel_dx0 + mx;
        panel.dy = s.panel_dy0 + my;
      }
    }
    s.pressed = 0;
    s.stick_active = false;
    nier::menu_mouse::touch_pointer().down = false;
    return;
  }
  const float aspect = s.aspect.load();
  const bool menu = s.context == Context::kMenu || s.context == Context::kBook;
  if (s.hold_until.size() != s.layout.controls.size()) s.hold_until.assign(s.layout.controls.size(), 0);
  // A finger already gone (down and up between two polls) still presses what it touched, briefly.
  for (const State::Tap& t : taps) {
    bool present = s.roles.count(t.id) != 0;
    for (const Finger& f : fingers) present |= f.id == t.id;
    if (present) continue;
    if (auto& panel = nier::menu_mouse::perf_panel();
        HitLocked(s, t.x, t.y) < 0 && panel.Has(t.x, t.y)) {  // a tap on the meter: shrink / grow
      panel.mini = !panel.mini.load();
      continue;
    }
    if (int hit = HitLocked(s, t.x, t.y); hit >= 0) {
      const Control& c = s.layout.controls[size_t(hit)];
      TouchedLocked(s, hit, now);
      if (c.kind != Kind::kWeapons) s.hold_until[size_t(hit)] = now + 80;
      if (c.popup) s.weapons_open = false;
    } else if (ConfirmsLocked(s)) {  // a quick tap on a notice, "Press START", a talk: its confirm
      s.confirm_until = now + 120;
    } else if (menu && !kOsPointer) {  // a quick tap on a menu: the pointer there, briefly down
      ToGameSpace(t.x, t.y, aspect, s.pointer_x, s.pointer_y);
      s.pointer_from = now;
      s.pointer_a_given = false;
      s.pointer_until = now + 140;
    }
  }
  for (auto it = s.roles.begin(); it != s.roles.end();) {  // fingers lifted
    bool still = false;
    for (const Finger& f : fingers) still |= f.id == it->first;
    if (!still && it->second >= 0 && s.layout.controls[size_t(it->second)].popup) s.weapons_open = false;
    // The pointer's finger lifted before its A: a tap; its A still comes (below).
    if (!still && it->second == -4 && !s.pointer_a_given) {
      if (!s.dragging && now - s.pointer_from < 80) {  // lifted before the pointer went there: it goes now
        ToGameSpace(s.last_fx, s.last_fy, s.aspect.load(), s.pointer_x, s.pointer_y);
        s.pointer_from = now;
      }
      s.pointer_until = now + 140;
    }
    // The meter's finger lifted without dragging: a tap, which shrinks it to the FPS (or brings the rest back).
    if (!still && it->second == -5 && !s.panel_dragged) {
      auto& panel = nier::menu_mouse::perf_panel();
      panel.mini = !panel.mini.load();
    }
    it = still ? std::next(it) : s.roles.erase(it);
  }
  const Control* stick = nullptr;
  for (const Control& c : s.layout.controls) {
    if (c.kind == Kind::kStick) stick = &c;
  }
  uint32_t pressed = 0;
  bool stick_seen = false, camera_seen = false, pointer_seen = false, confirm_seen = false;
  float lx = 0, ly = 0, rx = 0, ry = 0;
  for (const Finger& f : fingers) {
    auto role = s.roles.find(f.id);
    if (role == s.roles.end()) {
      int r;
      if (auto& panel = nier::menu_mouse::perf_panel(); HitLocked(s, f.x, f.y) < 0 && panel.Has(f.x, f.y)) {
        r = -5;  // the performance meter (under no button): dragged, or tapped
        s.panel_fx = f.x;
        s.panel_fy = f.y;
        s.panel_dx0 = panel.dx.load();
        s.panel_dy0 = panel.dy.load();
        s.panel_dragged = false;
      } else if (int hit = HitLocked(s, f.x, f.y); hit >= 0) {
        r = hit;
        TouchedLocked(s, hit, now);
      } else if (ConfirmsLocked(s)) {
        r = -6;  // the screen's confirm (A; Start too at "Press START"), down while it stays
      } else if (menu && kOsPointer) {
        r = -3;  // Windows: its touch moves the mouse, which drives the menus (menu_mouse.h)
      } else if (menu) {
        r = -4;  // the menu's pointer (menu_mouse.h): where it touches, down while it stays
        s.pointer_from = now;
        s.pointer_a_given = false;
        s.drag_fy = f.y;
        s.dragging = false;
        s.drag_rows = 0;
      } else if (s.weapons_open) {
        r = -3;  // a touch elsewhere only closes the weapons
        s.weapons_open = false;
      } else if (f.x < 0.5f) {
        const char* icon;
        r = stick && ShownLocked(s, *stick, &icon) ? -1 : -3;
        s.stick_ax = f.x;
        s.stick_ay = f.y;
      } else if (s.context != Context::kGameplay && s.context != Context::kFishing) {
        r = -3;  // the camera turns in gameplay (and fishing: the user, 2026-11-20) only
      } else {
        r = -2;
        s.camera_x = f.x;
        s.camera_y = f.y;
        s.camera_ns = SDL_GetTicksNS();
        s.camera_vx = s.camera_vy = 0;
      }
      role = s.roles.emplace(f.id, r).first;
    }
    if (role->second == -6) {
      confirm_seen = true;
    } else if (role->second == -5) {
      const float mx = f.x - s.panel_fx, my = f.y - s.panel_fy;
      if (!s.panel_dragged && mx * mx * aspect * aspect + my * my > 0.02f * 0.02f) s.panel_dragged = true;
      if (s.panel_dragged) {
        auto& panel = nier::menu_mouse::perf_panel();
        panel.dx = s.panel_dx0 + mx;
        panel.dy = s.panel_dy0 + my;
      }
    } else if (role->second == -4) {
      if (!pointer_seen) {
        pointer_seen = true;
        const float dy = f.y - s.drag_fy;
        if (!s.dragging && std::fabs(dy) > 0.035f) {
          s.dragging = true;  // a drag, not a touch on a row: it scrolls, and gives no A
          s.pointer_a_given = true;
        }
        s.last_fx = f.x;
        s.last_fy = f.y;
        if (s.dragging) {
          // A row (39 of 720) a step; the finger going up shows the rows below, as a phone's lists do.
          const int rows = int(-dy / (39.0f / 720.0f));
          auto& request = nier::menu_mouse::nodes().scroll_request;
          for (; s.drag_rows < rows; ++s.drag_rows) request.fetch_add(1);
          for (; s.drag_rows > rows; --s.drag_rows) request.fetch_add(-1);
        } else if (now - s.pointer_from >= 80) {
          // The pointer goes there once the finger held still (before, a drag's start walked the cursor).
          ToGameSpace(f.x, f.y, aspect, s.pointer_x, s.pointer_y);
        }
      }
    } else if (role->second >= 0) {
      // A button finger presses whatever button it is on now (it can slide from one to the next); the weapons'
      // own button only opens them.
      if (int hit = HitLocked(s, f.x, f.y); hit >= 0 && s.layout.controls[size_t(hit)].kind != Kind::kWeapons)
        pressed |= 1u << hit;
    } else if (role->second == -1 && !stick_seen && stick) {
      stick_seen = true;
      const float radius = stick->size * s.layout.scale * 0.5f;  // screen heights
      float dx = (f.x - s.stick_ax) * aspect / radius, dy = (f.y - s.stick_ay) / radius;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > 1.0f) {
        dx /= len;
        dy /= len;
      }
      lx = dx;
      ly = -dy;
      s.stick_kx = s.stick_ax + dx * radius / aspect;
      s.stick_ky = s.stick_ay + dy * radius;
    } else if (role->second == -2 && !camera_seen) {
      camera_seen = true;
      // The finger's speed, not its travel since the last poll: the game reads the pad more than once a frame,
      // and the reads after the first saw no travel. Measured when the finger moves (the touch screen reports
      // ~60-120 times a second), smoothed, and let go of within 80 ms of it stopping.
      const uint64_t ns = SDL_GetTicksNS();
      const float dt = float(ns - s.camera_ns) * 1e-9f;
      if (f.x != s.camera_x || f.y != s.camera_y) {
        if (dt > 0.001f) {
          const float vx = (f.x - s.camera_x) * aspect / dt, vy = (f.y - s.camera_y) / dt;  // heights a second
          s.camera_vx = 0.5f * s.camera_vx + 0.5f * vx;
          s.camera_vy = 0.5f * s.camera_vy + 0.5f * vy;
          s.camera_x = f.x;
          s.camera_y = f.y;
          s.camera_ns = ns;
        }
      } else if (dt > 0.08f) {
        s.camera_vx = s.camera_vy = 0;
      }
      // Full speed at 0.6 screen heights a second (at speed 1).
      const float k = s.layout.camera / 0.6f;
      const float mx = s.camera_vx * k, my = -s.camera_vy * k, m = std::sqrt(mx * mx + my * my);
      if (m >= 0.02f) {
        const float to = PastDeadZone(m) / m;  // the same direction, its length past the dead zone
        rx = mx * to;
        ry = my * to;
      }
    }
  }
  for (size_t i = 0; i < s.hold_until.size(); ++i) {
    if (now < s.hold_until[i]) pressed |= 1u << i;
  }
  s.pressed = pressed;
  s.stick_active = stick_seen;

  uint16_t buttons = 0;
  // The menu's pointer for the menus' hooks (menu_mouse.h): the finger as the mouse, its touch as the left
  // button, which is A in menus (as the mouse's click on the PC).
  {
    auto& tp = nier::menu_mouse::touch_pointer();
    // A finger on the menu is down after 80 ms that were not a drag (a drag starting on a row picked it); one
    // already lifted, until pointer_until.
    const bool settled = now - s.pointer_from >= 80;
    const bool down = menu && ((pointer_seen && settled && !s.dragging) || (!pointer_seen && now < s.pointer_until));
    tp.x = menu ? s.pointer_x : -1;
    tp.y = menu ? s.pointer_y : -1;
    tp.down = down;
    // A once the menu's hook moved the row under the finger (50 ms) and the row's highlight began (300 ms
    // after a move: the title menu takes no A before); a tap that lifted waits with it.
    const bool wait = now - s.pointer_from < 50 ||
                      nier::menu_mouse::NowMs() - nier::menu_mouse::title().moved_ms.load() < 300;
    if (down && wait && !pointer_seen) s.pointer_until = std::max(s.pointer_until, now + 90);
    if (down && !wait && !(pointer_seen && s.dragging)) {
      // At least 100 ms of A (a quick tap gave 30-50 ms, which a 30 fps screen could miss: "Operation complete").
      if (!s.pointer_a_given) {
        s.pointer_until = std::max(s.pointer_until, now + 250);
        s.pointer_a_frame = nier::menu_mouse::game_frame().load();
        static const bool log = std::getenv("NIER_TOUCHLOG") != nullptr;  // TEMP (TOUCHLOG)
        if (log)
          REXLOG_INFO("TOUCHLOG A em {},{} hovered_titulo {} contexto {}", s.pointer_x, s.pointer_y,
                      nier::menu_mouse::title().hovered.load(), int(s.context));
      }
      // Three of the game's frames of it, not the whole 250 ms: at 60 fps the shop's next screen (the question
      // after a buy's row) came up within them and took the same A (2026-11-19: a tap bought at once).
      if (nier::menu_mouse::game_frame().load() - s.pointer_a_frame < 3) buttons |= kPadA;
      s.pointer_a_given = true;
    }
  }
  // A touch on a screen without the game's controls (a notice, "Press START", a cutscene, a talk): its confirm.
  if (ConfirmsLocked(s) && (confirm_seen || now < s.confirm_until))
    buttons |= kPadA | (s.context == Context::kStartOnly ? kPadStart : 0);
  for (size_t i = 0; i < s.layout.controls.size(); ++i) {
    if (!(pressed & (1u << i))) continue;
    const Control& c = s.layout.controls[i];
    if (c.kind == Kind::kButton) buttons |= c.bit;
    if (c.kind == Kind::kLeftTrigger) g.left_trigger = 255;
    if (c.kind == Kind::kRightTrigger) g.right_trigger = 255;
  }
  g.buttons = uint16_t(uint16_t(g.buttons) | buttons);
  // A charging magic held, on screen or on the physical pad: its button alone stays on screen.
  s.focus = -1;
  for (size_t i = 0; i < s.layout.controls.size(); ++i) {
    const Control& c = s.layout.controls[i];
    if (c.slot < 0 || !HeldMagic(s.abilities[c.slot])) continue;
    const bool held = (pressed & (1u << i)) || (c.kind == Kind::kButton && (g.buttons & c.bit)) ||
                      (c.kind == Kind::kLeftTrigger && g.left_trigger > 64) ||
                      (c.kind == Kind::kRightTrigger && g.right_trigger > 64);
    if (held) s.focus = int(i);
  }
  if (s.context == Context::kChoice) {
    // The choices by the stick: up / down as the d-pad's, a step on the push and then every 300 ms while held.
    const int dir = !stick_seen ? 0 : ly > 0.55f ? 1 : ly < -0.55f ? -1 : 0;
    if (dir != s.choice_dir) {
      s.choice_dir = dir;
      s.choice_next = now;
    }
    if (dir != 0 && now >= s.choice_next) {
      s.choice_next = now + 300;
      s.choice_until = now + 80;
      s.choice_bit = dir > 0 ? kPadUp : kPadDown;
    }
    if (now < s.choice_until) g.buttons = uint16_t(uint16_t(g.buttons) | s.choice_bit);
    stick_seen = false;  // not as the stick too
  }
  if (stick_seen) {
    g.thumb_lx = int16_t(std::lround(lx * 32767));
    g.thumb_ly = int16_t(std::lround(ly * 32767));
  }
  if (camera_seen && (rx != 0 || ry != 0)) {
    g.thumb_rx = int16_t(std::lround(rx * 32767));
    g.thumb_ry = int16_t(std::lround(ry * 32767));
  }
}

// Between the real pad and the gate (input_recorder.h): adds the touches, and is the pad when there is none.
class TouchDriver final : public rex::input::InputDriver {
 public:
  explicit TouchDriver(std::unique_ptr<rex::input::InputDriver> inner)
      : InputDriver(nullptr, 0), inner_(std::move(inner)) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    const size_t before = out.size();
    inner_->EnumerateDevices(out);
    // A real pad; LiveDriver's test device (NIER_INPUT_LIVE, "LIVE") is none.
#if defined(_WIN32)
    // A PC: this wraps the keyboard and mouse's device (input_recorder.h), never a pad: the gate of the real pads says
    // whether one is connected (physical_pad), and no device of its own is added.
    (void)before;
    return;
#endif
    bool pad = false;
    for (size_t i = before; i < out.size(); ++i) pad |= out[i].id != static_cast<rex::input::DeviceId>(0x4C495645);
    if (state().physical_pad.exchange(pad) != pad) {
      REXLOG_INFO("Toque: controle fisico {}", pad ? "conectado (botoes na tela escondidos no automatico)"
                                                     : "desconectado (botoes na tela)");
    }
    if (!pad) {
      rex::input::DeviceInfo info;
      info.id = kDevice;
      info.name = "Touch";
      out.push_back(info);
    }
  }
  void OnWindowAvailable(rex::ui::Window* window) override { inner_->OnWindowAvailable(window); }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    X_RESULT r = X_ERROR_SUCCESS;
    if (id == kDevice) {
      if (out) {
        const uint32_t packet = out->packet_number;
        std::memset(out, 0, sizeof(*out));
        out->packet_number = packet + 1;
      }
    } else {
      r = inner_->GetDeviceState(id, out);
    }
    if (r != X_ERROR_SUCCESS || !out) return r;
    if (state().editing.load()) {  // the editor has the screen: the game gets an idle pad
      const uint32_t packet = out->packet_number;
      std::memset(out, 0, sizeof(*out));
      out->packet_number = packet;
      Poll(out->gamepad);
      return r;
    }
    Poll(out->gamepad);
    return r;
  }
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 rex::input::X_INPUT_CAPABILITIES* out) override {
    if (id == kDevice) return nop_.GetDeviceCapabilities(kNopId, flags, out);
    return inner_->GetDeviceCapabilities(id, flags, out);
  }
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION* v) override {
    if (id == kDevice) return X_ERROR_SUCCESS;
    return inner_->SetDeviceVibration(id, v);
  }
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              rex::input::X_INPUT_KEYSTROKE* out) override {
    if (id == kDevice) return X_ERROR_EMPTY;
    return inner_->GetDeviceKeystroke(id, flags, out);
  }

 private:
  static constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x54554348);  // "TUCH"
  static constexpr auto kNopId = static_cast<rex::input::DeviceId>(0x4E4F5000);   // NopInputDriver's
  std::unique_ptr<rex::input::InputDriver> inner_;
  rex::input::nop::NopInputDriver nop_{nullptr, 0};
};

// Draws the buttons, and the editor while it is open.
class TouchDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit TouchDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  void OnDraw(ImGuiIO& io) override {
    State& s = state();
    const ImVec2 screen = io.DisplaySize;
    if (screen.x < 1 || screen.y < 1) return;
    s.aspect = screen.x / screen.y;
    EnsureTextures();
    const bool editing = s.editing.load();
    DrawModIcons(screen, editing);
    if (editing) Edit(screen);
    std::lock_guard lock(s.mutex);
    EnsureLoadedLocked(s);
    if (!editing && !s.VisibleLocked()) return;
    if (!editing && pc_options_state().jukebox_open) return;  // the Jukebox page is touched itself
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float h = screen.y;
    if (editing) dl->AddRectFilled({0, 0}, screen, IM_COL32(0, 0, 0, 110));
    const float alpha = editing ? 1.0f : s.layout.opacity;
    for (size_t i = 0; i < s.layout.controls.size(); ++i) {
      const Control& c = s.layout.controls[i];
      if (c.popup && (editing || !s.weapons_open)) continue;
      if (!editing && s.focus >= 0 && int(i) != s.focus) continue;  // charging a magic: only its button
      const bool down = !editing && ((s.pressed & (1u << i)) || (c.kind == Kind::kWeapons && s.weapons_open));
      const bool selected = editing && int(i) == selected_;
      float r = c.size * s.layout.scale * 0.5f * h;
      ImVec2 centre(c.x * screen.x, c.y * h);
      if (c.kind == Kind::kStick) {
        const char* unused;
        if (!ShownLocked(s, c, &unused)) continue;
        if (!editing && s.stick_active) {
          centre = {s.stick_ax * screen.x, s.stick_ay * h};
        }
        DrawStick(dl, s, centre, r, alpha, editing, selected, screen);
        continue;
      }
      if (down) r *= 0.92f;
      Control shown = c;
      if (!ShownLocked(s, c, &shown.icon)) continue;  // what the game lets it do now, its sigil
      DrawButton(dl, shown, centre, r, alpha, down, selected);
      if (std::strcmp(c.id, "menu_back") == 0) DrawBackArrow(dl, centre, r, alpha, down);
    }
    if (editing) DrawToolbar(dl, screen);
  }

 private:
  // Nier Plus: a round icon per code mod panel (mods.cpp), a column on the right edge; a touch opens its panel
  // (WatchTaps), the same touch again closes it. Hidden while the buttons are being edited.
  void DrawModIcons(ImVec2 screen, bool editing) {
    State& s = state();
    // The Jukebox's icon (jukebox/jukebox_ui.h) heads the column: its own id, not a panel's.
    constexpr int kJukeboxIcon = 100000;
    if (const int p = s.mod_toggle.exchange(-1); p == kJukeboxIcon) {
      auto& pc = pc_options_state();
      pc.jukebox_open = !pc.jukebox_open;
      pc.TakeJukebox();
    } else if (p >= 0) {
      nier::mods::TogglePanel(p);
    }
    const auto panels = nier::mods::Panels();
    constexpr float kRadius = 0.045f;  // of the screen's height
    const float aspect = screen.x / screen.y;
    std::vector<State::ModIcon> icons;
    const bool jukebox_open = pc_options_state().jukebox_open;
    // A PC: only once its touch screen has been touched (the mouse has the mods' keys).
    if (!editing && !jukebox_open && (!kOsPointer || s.touched.load())) {
      // The mods' panels. The Jukebox has no icon of its own any more (2026-11-17, the user): it opens from the
      // pause's cross (its plate, or RB) and closes with its page's X.
      for (size_t i = 0; i < panels.size(); ++i)
        icons.push_back({panels[i].index, 1.0f - kRadius * 1.6f / aspect, 0.24f + float(i) * kRadius * 2.6f, kRadius});
    }
    {
      std::lock_guard lock(s.taps_mutex);
      s.mod_icons = icons;
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    for (size_t i = 0; i < icons.size(); ++i) {
      const ImVec2 c(icons[i].x * screen.x, icons[i].y * screen.y);
      const float r = icons[i].r * screen.y;
      dl->AddCircleFilled(c, r, IM_COL32(18, 20, 28, 170), 40);
      dl->AddCircle(c, r, IM_COL32(222, 214, 190, 220), 40, r * 0.08f);
      if (icons[i].panel == kJukeboxIcon) {
        // A record with a note.
        const ImU32 ink = IM_COL32(240, 232, 210, 240);
        dl->AddCircle(c, r * 0.62f, ink, 32, r * 0.07f);
        dl->AddCircleFilled(c, r * 0.14f, ink, 16);
        dl->AddLine({c.x + r * 0.30f, c.y - r * 0.55f}, {c.x + r * 0.30f, c.y + r * 0.10f}, ink, r * 0.09f);
        dl->AddCircleFilled({c.x + r * 0.16f, c.y + r * 0.14f}, r * 0.16f, ink, 16);
        dl->AddLine({c.x + r * 0.30f, c.y - r * 0.55f}, {c.x + r * 0.52f, c.y - r * 0.40f}, ink, r * 0.09f);
        continue;
      }
      const auto* panel = [&]() -> const decltype(panels)::value_type* {
        for (const auto& p : panels)
          if (p.index == icons[i].panel) return &p;
        return nullptr;
      }();
      if (!panel) continue;
      // The panel's first letter (its own icon art: later).
      const std::string letter = panel->title.empty() ? std::string("?") : panel->title.substr(0, 1);
      const float size = r * 1.1f;
      const ImVec2 t = font->CalcTextSizeA(size, FLT_MAX, 0.0f, letter.c_str());
      dl->AddText(font, size, {c.x - t.x * 0.5f, c.y - t.y * 0.5f}, IM_COL32(240, 232, 210, 240), letter.c_str());
    }
  }

  ImTextureID Texture(const std::string& name) const {
    auto it = textures_.find(name);
    return it == textures_.end() || !it->second ? ImTextureID{} : reinterpret_cast<ImTextureID>(it->second.get());
  }
  void EnsureTextures() {
    rex::ui::ImmediateDrawer* drawer = imgui_drawer()->immediate_drawer();
    if (drawer != owner_) {
      for (auto& [name, t] : textures_) t.release();
      textures_.clear();
      owner_ = drawer;
    }
    auto& art = menu_art();
    if (!drawer || !textures_.empty() || !art.ready) return;
    std::lock_guard lock(art.mutex);
    std::vector<std::string> names = MenuIcons();
    for (const std::string& n : CommonIcons()) names.push_back(n);
    names.push_back("bt_icon_360_bt_icon_D5");
    for (const std::string& name : names) {
      auto it = art.images.find(name);
      if (it == art.images.end()) continue;
      textures_[name] = drawer->CreateTexture(uint32_t(it->second.width), uint32_t(it->second.height),
                                              rex::ui::ImmediateTextureFilter::kLinear, false,
                                              it->second.rgba.data());
    }
  }

  static ImU32 Alpha(int r, int g, int b, float a) { return IM_COL32(r, g, b, int(std::clamp(a, 0.0f, 1.0f) * 255)); }

  void Glyph(ImDrawList* dl, int cell, ImVec2 a, ImVec2 b, float alpha) {
    ImTextureID sheet = Texture("bt_icon_360_bt_icon_D5");
    if (!sheet || cell < 0) return;
    const float u = float(cell % 4) * 32.0f / 128.0f, v = float(cell / 4) * 32.0f / 192.0f;
    dl->AddImage(sheet, a, b, {u, v}, {u + 32.0f / 128.0f, v + 32.0f / 192.0f}, Alpha(255, 255, 255, alpha));
  }

  void DrawButton(ImDrawList* dl, const Control& c, ImVec2 o, float r, float alpha, bool down, bool selected) {
    dl->AddCircleFilled(o, r, Alpha(18, 20, 28, alpha * (down ? 0.75f : 0.45f)), 48);
    ImTextureID icon = c.icon ? Texture(c.icon) : ImTextureID{};
    if (icon) {
      const bool sigil = std::strncmp(c.icon, "menu_word_edit_target_", 22) == 0;
      const float ir = r * (sigil ? 0.94f : 0.8f);
      const ImVec2 a(o.x - ir, o.y - ir), b(o.x + ir, o.y + ir);
      const ImU32 tint = down ? Alpha(255, 236, 190, alpha) : Alpha(255, 255, 255, alpha);
      if (sigil) {
        dl->AddImageRounded(icon, a, b, {kSigilU0, kSigilV0}, {kSigilU1, kSigilV1}, tint, ir);
      } else {
        // The menu icons are dark ink: on the sigils' paper colour, as in the book.
        dl->AddCircleFilled(o, r * 0.94f, down ? Alpha(255, 236, 190, alpha) : Alpha(214, 212, 204, alpha), 48);
        dl->AddImage(icon, a, b, {0, 0}, {1, 1}, Alpha(255, 255, 255, alpha));
      }
      // The 360 button in the corner, so the player knows which one it is.
      const float gr = r * 0.42f;
      const ImVec2 gc(o.x + r * 0.68f, o.y + r * 0.68f);
      Glyph(dl, c.glyph, {gc.x - gr, gc.y - gr}, {gc.x + gr, gc.y + gr}, alpha);
    } else {
      const float gr = r * 0.82f;
      Glyph(dl, c.glyph, {o.x - gr, o.y - gr}, {o.x + gr, o.y + gr}, alpha);
    }
    dl->AddCircle(o, r, selected ? IM_COL32(255, 210, 90, 255) : Alpha(230, 228, 220, alpha * 0.8f), 48,
                  selected ? 4.0f : 2.0f);
  }

  // The menus' back button: an arrow pointing left, in the book's paper colour.
  void DrawBackArrow(ImDrawList* dl, ImVec2 o, float r, float alpha, bool down) {
    const ImU32 ink = down ? Alpha(255, 236, 190, alpha) : Alpha(230, 228, 220, alpha);
    const float t = std::max(2.0f, r * 0.14f);
    dl->AddLine({o.x + r * 0.48f, o.y}, {o.x - r * 0.15f, o.y}, ink, t);
    dl->AddTriangleFilled({o.x - r * 0.52f, o.y}, {o.x - r * 0.10f, o.y - r * 0.36f}, {o.x - r * 0.10f, o.y + r * 0.36f},
                          ink);
  }

  void DrawStick(ImDrawList* dl, const State& s, ImVec2 o, float r, float alpha, bool editing, bool selected,
                 ImVec2 screen) {
    dl->AddCircleFilled(o, r, Alpha(18, 20, 28, alpha * 0.35f), 64);
    dl->AddCircle(o, r, selected ? IM_COL32(255, 210, 90, 255) : Alpha(230, 228, 220, alpha * 0.7f), 64,
                  selected ? 4.0f : 2.5f);
    ImVec2 knob = o;
    if (!editing && s.stick_active) knob = {s.stick_kx * screen.x, s.stick_ky * screen.y};
    dl->AddCircleFilled(knob, r * 0.42f, Alpha(230, 228, 220, alpha * 0.55f), 48);
    Glyph(dl, 20, {knob.x - r * 0.36f, knob.y - r * 0.36f}, {knob.x + r * 0.36f, knob.y + r * 0.36f}, alpha);
  }

  // The editor's bar: + / - size of the selected button, defaults, cancel, save.
  struct Tool {
    const char* text;  // English, through pc_text::T
    int action;
  };
  static constexpr Tool kTools[] = {{"-", 1}, {"+", 2}, {"Default", 3}, {"Cancel", 4}, {"Save", 5}};

  ImVec2 ToolRect(ImVec2 screen, int i, ImVec2* b) const {
    const float h = screen.y, w = h * 0.2f, gap = h * 0.02f, bh = h * 0.09f;
    const float total = std::size(kTools) * w + (std::size(kTools) - 1) * gap;
    const ImVec2 a((screen.x - total) * 0.5f + i * (w + gap), h * 0.42f);
    *b = {a.x + w, a.y + bh};
    return a;
  }

  void DrawToolbar(ImDrawList* dl, ImVec2 screen) {
    ImFont* font = ImGui::GetFont();
    const float size = screen.y * 0.045f;
    for (int i = 0; i < int(std::size(kTools)); ++i) {
      ImVec2 b;
      const ImVec2 a = ToolRect(screen, i, &b);
      dl->AddRectFilled(a, b, IM_COL32(214, 212, 204, 235), screen.y * 0.012f);
      dl->AddRect(a, b, IM_COL32(26, 30, 44, 255), screen.y * 0.012f, 0, 2.0f);
      const std::string text = pc_text::T(kTools[i].text);
      const ImVec2 ts = font->CalcTextSizeA(size, 1e9f, 0, text.c_str());
      dl->AddText(font, size, {(a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f}, IM_COL32(26, 30, 44, 255),
                  text.c_str());
    }
    const std::string hint = pc_text::T("Drag the buttons. Tap one and use - / + to change its size.");
    const ImVec2 ts = font->CalcTextSizeA(size * 0.8f, 1e9f, 0, hint.c_str());
    dl->AddText(font, size * 0.8f, {(screen.x - ts.x) * 0.5f, screen.y * 0.53f}, IM_COL32(240, 238, 230, 255),
                hint.c_str());
  }

  // Editor input, from the fingers (UI thread).
  void Edit(ImVec2 screen) {
    State& s = state();
    std::vector<Finger> fingers = Fingers();
    // A PC: the mouse moves the buttons too (one without a touch screen could not leave the editor otherwise).
    if (kOsPointer && fingers.empty() && ImGui::IsMouseDown(0)) {
      Finger m{};
      m.id = SDL_FingerID(0x7FFFFFFF);
      m.x = ImGui::GetIO().MousePos.x / screen.x;
      m.y = ImGui::GetIO().MousePos.y / screen.y;
      fingers.push_back(m);
    }
    std::lock_guard lock(s.mutex);
    EnsureLoadedLocked(s);
    s.weapons_open = false;
    if (!backup_taken_) {
      backup_ = s.layout;
      backup_taken_ = true;
    }
    const Finger* f = nullptr;
    for (const Finger& x : fingers) {
      if (x.id == drag_finger_) f = &x;
    }
    if (!f && !fingers.empty() && drag_finger_ != fingers.front().id) {
      // A new touch: a tool, else the button under it.
      const Finger& n = fingers.front();
      drag_finger_ = n.id;
      dragging_ = -1;
      const ImVec2 p(n.x * screen.x, n.y * screen.y);
      for (int i = 0; i < int(std::size(kTools)); ++i) {
        ImVec2 b;
        const ImVec2 a = ToolRect(screen, i, &b);
        if (p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y) {
          ToolLocked(s, kTools[i].action);
          return;
        }
      }
      int hit = HitLocked(s, n.x, n.y);
      if (hit < 0) {  // the stick is not in HitLocked (it floats in play): its own circle here
        for (size_t i = 0; i < s.layout.controls.size(); ++i) {
          const Control& c = s.layout.controls[i];
          if (c.kind != Kind::kStick) continue;
          const float dx = (n.x - c.x) * screen.x / screen.y, dy = n.y - c.y;
          if (std::sqrt(dx * dx + dy * dy) <= c.size * s.layout.scale * 0.5f) hit = int(i);
        }
      }
      if (hit >= 0) {
        selected_ = hit;
        dragging_ = hit;
        grab_dx_ = s.layout.controls[size_t(hit)].x - n.x;
        grab_dy_ = s.layout.controls[size_t(hit)].y - n.y;
      }
      return;
    }
    if (fingers.empty()) drag_finger_ = SDL_FingerID(-1);
    if (f && dragging_ >= 0) {
      Control& c = s.layout.controls[size_t(dragging_)];
      c.x = std::clamp(f->x + grab_dx_, 0.02f, 0.98f);
      c.y = std::clamp(f->y + grab_dy_, 0.03f, 0.97f);
    }
  }

  void ToolLocked(State& s, int action) {
    if (action == 1 || action == 2) {
      if (selected_ >= 0 && selected_ < int(s.layout.controls.size())) {
        Control& c = s.layout.controls[size_t(selected_)];
        c.size = std::clamp(c.size * (action == 2 ? 1.1f : 1.0f / 1.1f), 0.05f, 0.5f);
      }
      return;
    }
    if (action == 3) {
      const Layout keep = s.layout;
      s.layout = DefaultLayout(s.aspect.load());
      s.layout.mode = keep.mode;
      s.layout.opacity = keep.opacity;
      s.layout.scale = keep.scale;
      s.layout.camera = keep.camera;
      return;
    }
    if (action == 4) s.layout = backup_;
    if (action == 5) SaveLocked(s);
    backup_taken_ = false;
    selected_ = -1;
    dragging_ = -1;
    s.editing = false;
  }

  std::map<std::string, std::unique_ptr<rex::ui::ImmediateTexture>> textures_;
  rex::ui::ImmediateDrawer* owner_ = nullptr;
  SDL_FingerID drag_finger_ = SDL_FingerID(-1);
  int dragging_ = -1, selected_ = -1;
  float grab_dx_ = 0, grab_dy_ = 0;
  Layout backup_;
  bool backup_taken_ = false;
};

}  // namespace nier::touch
