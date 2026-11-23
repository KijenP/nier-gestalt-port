// nier - "OpÃ§Ãµes de PC": an in-game settings panel in NieR's visual style.
//
// Opens with F1 or Back+Start on the controller. While it is open the game
// gets a neutral pad (see GateDriver in input_recorder.h) and the pad (or the
// keyboard through its pad binds) drives the panel instead: up/down picks a
// row, left/right changes it, A confirms, B/Start backs out and saves
// nier.toml. The mouse works as in a native PC game: hovering selects, a
// click changes or confirms, the wheel scrolls, right click backs out.
// A second page, "Controles do teclado", rebinds the keyboard/mouse like
// Replicant's "Custom (Keyboard)" screen (keyboard_binds.h).

#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>

#include "menu_mouse.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>

#include "game_assets.h"
#include "game_text.h"
#include "keyboard_binds.h"

namespace nier {

// Pad buttons (XInput bit layout, as the guest sees them).
enum : uint16_t {
  kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008,
  kPadStart = 0x0010, kPadBack = 0x0020, kPadA = 0x1000, kPadB = 0x2000,
  kPadLB = 0x0100, kPadRB = 0x0200,
};

// Shared between the input thread (GateDriver) and the UI thread (panel).
struct PcOptionsState {
  std::atomic<bool> open{false};
  std::atomic<bool> toggle_requested{false};
  // Nier Plus (2026-11-15): a code mod's panel is open (mods.cpp): the game gets no input, the mouse is free.
  std::atomic<bool> mod_panel_open{false};
  // Nier Plus: a mod uses the controller (free camera): the game gets none, mouse look stays on (input_recorder.h).
  std::atomic<bool> mod_input_blocked{false};
  // The controller as the game would have read it, before any muting (mods' get_pad): buttons | lt << 16 | rt << 24,
  // and the sticks lx, ly, rx, ry (16 bits each, from the low end).
  std::atomic<uint32_t> pad_buttons{0};
  std::atomic<uint64_t> pad_sticks{0};
  // Set by the game's Options menu hooks (video_options_menu.cpp). The game
  // rebuilds a list only when a screen opens, so "on screen" is tracked by
  // events (list built / screen transition) and the selection is read live
  // from the menu object when A is pressed.
  std::atomic<bool> game_options_on_screen{false};
  std::atomic<uint8_t*> game_options_menu{nullptr};  // host pointer to the menu object
  std::atomic<int> game_item_index{-1};              // "Video Options" row in its item list
  std::atomic<int> game_controls_index{-1};          // "Control Options" row
  std::atomic<int> game_language_index{-1};          // "Language" row

  // 1 = our Video Options row is selected, 2 = Control Options, 3 = Language,
  // 4 = Quit to Desktop, 0 = none of them. The title's Options menu (list menu
  // type 1, its 6-row list) or the pause menu's Options (type 7, state 7: the
  // settings list, rows rewritten by video_options_menu.cpp).
  int GameItemSelected() const {
    uint8_t* obj = game_options_menu.load();
    if (!obj || native_page) return 0;
    auto be32 = [](const uint8_t* p) {
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    const uint32_t type = be32(obj + 3552);
    uint32_t id = 0;
    // The pause list is told by its own state (the game's screen transition right after the
    // build clears the "on screen" mark), and only while the game shows a menu.
    if (type == 1 && game_options_on_screen) {
      const uint32_t row = be32(obj + 3564) + be32(obj + 3568);
      if (row < 8) id = be32(obj + 3580 + row * 56 + 40);
    } else if (PauseListShowing() && !pause_dialog) {
      id = PauseSelectedId();
    }
    switch (id) {
      case kRowVideo: return 1;
      case kRowControls: return 2;
      case kRowLanguage: return 3;
      case kRowQuit: return 4;
      case kRowAchievements: return 5;
      case kRowMods: return 6;
      default: return 0;
    }
  }
  // The pause menu's Options list (type 7, state 7) is showing. Its object keeps that state after
  // it closes, so pause_on_screen is set when the list is built and cleared when it is left: B, or
  // A on Return / Quit Game / Load (NotePausePress).
  std::atomic<bool> pause_on_screen{false};
  bool PauseListShowing() const {
    uint8_t* obj = game_options_menu.load();
    if (!obj || !pause_on_screen || !GameInMenu()) return false;
    auto be32 = [](const uint8_t* p) {
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    return be32(obj + 3552) == 7 && be32(obj + 3556) == 7;
  }
  uint32_t PauseSelectedId() const {
    const uint8_t* obj = game_options_menu.load();
    auto be32 = [](const uint8_t* p) {
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    const uint32_t row = be32(obj + 3572) + be32(obj + 3576);
    return row < 24 ? be32(obj + 3916 + row * 56 + 40) : 0;
  }
  uint32_t PauseSelectedLabel() const {
    const uint8_t* obj = game_options_menu.load();
    auto be32 = [](const uint8_t* p) {
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    const uint32_t row = be32(obj + 3572) + be32(obj + 3576);
    return row < 24 ? be32(obj + 3916 + row * 56 + 44) : 0;
  }
  // Quit to Desktop in the pause menu asks first, with the game's own Quit Game dialog: A on that
  // row turns it into Quit Game (id 0x18) for the game, which then asks; its Yes heads for the title
  // screen, and the title's load (game_text_hooks.cpp) closes the game while this is armed. The
  // dialog's question is ours while armed (video_options_menu.cpp). A on another row or B disarms.
  static constexpr uint32_t kQuitLabelId = 0x7E000007;
  std::atomic<bool> quit_desktop_armed{false};
  // Its No / B: the row made Quit Game for the dialog becomes Quit to Desktop again. Left as Quit
  // Game, it acted as one and the next rewrite of the list (which looks for that row) doubled the
  // rows of ours (2026-11-12).
  void DisarmQuitDesktop() {
    quit_desktop_armed = false;
    uint8_t* obj = game_options_menu.load();
    if (!obj) return;
    auto be32 = [](const uint8_t* p) {
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    const uint32_t count = be32(obj + 5264);
    for (uint32_t i = 0; i < count && i < 24; ++i) {
      uint8_t* item = obj + 3916 + i * 56;
      if (be32(item + 44) == kQuitLabelId && be32(item + 40) == 0x18) {
        item[40] = uint8_t(kRowQuit >> 24); item[41] = uint8_t(kRowQuit >> 16);
        item[42] = uint8_t(kRowQuit >> 8); item[43] = uint8_t(kRowQuit);
      }
    }
  }
  // True when this A press is on our Quit row of the pause list (the row is then made Quit Game).
  bool ArmQuitDesktop(uint16_t pressed) {
    if (native_page || pause_dialog || !(pressed & kPadA) || !PauseListShowing()) return false;
    if (PauseSelectedLabel() != kQuitLabelId) return false;
    uint8_t* obj = game_options_menu.load();
    const uint32_t row = (uint32_t(obj[3572]) << 24 | obj[3573] << 16 | obj[3574] << 8 | obj[3575]) +
                         (uint32_t(obj[3576]) << 24 | obj[3577] << 16 | obj[3578] << 8 | obj[3579]);
    uint8_t* id = obj + 3916 + row * 56 + 40;
    id[0] = 0; id[1] = 0; id[2] = 0; id[3] = 0x18;
    quit_desktop_armed = true;
    return true;
  }
  // The game's Yes/No dialog over the pause list (Quit Game, Load, our Quit to Desktop): 0 = none,
  // 1 = open on No (where it opens), 2 = on Yes. Its No (or B) leaves the list showing; its Yes
  // leaves the pause menu.
  std::atomic<int> pause_dialog{0};
  void NotePausePress(uint16_t pressed) {
    if (native_page || !PauseListShowing()) return;
    if (pause_dialog) {
      if (pressed & kPadLeft) pause_dialog = 2;
      if (pressed & kPadRight) pause_dialog = 1;
      if (pressed & kPadB) {
        pause_dialog = 0;
        DisarmQuitDesktop();
      } else if (pressed & kPadA) {
        if (pause_dialog == 2) {
          pause_on_screen = false;  // Yes: on to the title (or Load); an armed quit closes the game there
        } else {
          DisarmQuitDesktop();
        }
        pause_dialog = 0;
      }
      return;
    }
    if (pressed & kPadB) {
      pause_on_screen = false;
      quit_desktop_armed = false;
    }
    if (pressed & kPadA) {
      const uint32_t id = PauseSelectedId();
      const bool ours = PauseSelectedLabel() == kQuitLabelId;
      if (!ours) quit_desktop_armed = false;
      if (id == 0x16) pause_on_screen = false;
      if (id == 0x18 || id == 0x19 || ours) pause_dialog = 1;
    }
  }
  // Grimoire Weiss's book (the pause menu's down: Maps, Items, Quests, Weapons, Word Edit, Notes):
  // open while its key hints offer LB/RB page changes (0x82757AC5 = 0x10, the hint block whose text
  // is "Mudar Pagina"); the page is told by the heading the game looks up when it shows one
  // (book_tab, set by video_options_menu.cpp's message hook). Notes' sixth row opens our
  // Achievements page drawn over the book (memo_achievements.cpp, achievements_ui.h); B goes back.
  std::atomic<int> book_tab{-1};
  std::atomic<bool> ach_open{false};
  std::atomic<int> input_kind{0};  // the device last used: 0 a controller, 1 the keyboard and mouse, 2 touch
  std::atomic<uint64_t> touch_ms{0};  // the last poll a finger was on the screen (touch_controls.h; ms)
  std::deque<uint16_t> ach_presses;  // under mutex
  bool BookOpen() const {
    const uint8_t* base = guest_base.load();
    return base && GameInMenu() && *reinterpret_cast<const volatile uint8_t*>(base + 0x82757AC5) == 0x10;
  }
  void PushAch(uint16_t buttons) {
    std::lock_guard lock(mutex);
    ach_presses.push_back(buttons);
  }
  std::deque<uint16_t> TakeAch() {
    std::lock_guard lock(mutex);
    std::deque<uint16_t> out;
    out.swap(ach_presses);
    return out;
  }
  // The Jukebox page (jukebox/jukebox_ui.h): open over the game, which then gets no input; its pad presses.
  std::atomic<bool> jukebox_open{false};
  // A tutorial of ours (tutorials.h) is up: the game gets no input; the presses that close it.
  std::atomic<bool> tutorial_open{false};
  // The pause's cross (the RB Jukebox hint goes on its key hints only, video_options_menu.cpp): when Start was last
  // pressed (it opens the pause) and when the cross was last on top (back from one of its arms).
  std::atomic<uint64_t> start_press_ms{0}, cross_seen_ms{0};
  std::atomic<uint16_t> tutorial_presses{0};
  std::deque<uint16_t> jukebox_presses;  // under mutex
  void PushJukebox(uint16_t buttons) {
    std::lock_guard lock(mutex);
    jukebox_presses.push_back(buttons);
  }
  std::deque<uint16_t> TakeJukebox() {
    std::lock_guard lock(mutex);
    std::deque<uint16_t> out;
    out.swap(jukebox_presses);
    return out;
  }
  // Item ids of our rows in the game's menus (the title's Video Options row keeps the
  // console's install entry id).
  static constexpr uint32_t kRowVideo = 0x25, kRowControls = 0x7E, kRowLanguage = 0x7D, kRowQuit = 0x7C,
                            kRowAchievements = 0x7B, kRowMods = 0x7A;  // Mods: the pause's (mods.cpp)
  // Page the panel opens on when toggled from the game's menu (2 = controls,
  // 3 = language).
  std::atomic<int> open_page{1};

  // The rows of ours in the game's Options list open pages inside that same
  // list (video_options_menu.cpp). The input thread only reports: A on one of
  // our rows (open request: 1 video, 2 controls, 3 language), then A/B/left/
  // right while a page is showing; the game thread acts on them.
  enum : uint32_t { kEventA = 1, kEventB = 2, kEventLeft = 4, kEventRight = 8 };
  std::atomic<int> native_open_request{0};
  std::atomic<int> native_page{0};  // 0 = the Options list itself
  std::atomic<bool> native_page_pause{false};  // that page rides on the pause menu's Options
  std::atomic<uint32_t> native_events{0};
  std::atomic<bool> native_capturing{false};  // waiting for a key: the game gets an idle pad
  std::filesystem::path config_path;  // nier.toml, set by the panel
  std::function<void()> quit_game;    // closes the game window (set by the app)
  // Guest memory base (set once the game image is loaded).
  std::atomic<uint8_t*> guest_base{nullptr};
  // 0x827EB0D8 is 1 on every non-gameplay screen (logos, title, load, loading,
  // pause menu) and 0 in gameplay: found by diffing memory dumps of a replay
  // (16 gameplay dumps vs 3 pause menu + 5 title/loading ones). Its 7
  // neighbours every 16 bytes match it, and 0x827E2B54 reads 4 in gameplay.
  // But in some areas it toggles during gameplay too (read at the pad read, it is 1 for seconds at
  // a time while walking: the cursor showed and a click jumped). 0x82757AB1 is 1 exactly while a
  // list menu (title menu, pause menu and its pages) is open and never toggles in gameplay (an
  // in-game scan, 2026-11-06: ~950 gameplay and ~400 menu samples), but it is 0 on the save select
  // and loading screens. So: a list menu, or 0x827EB0D8 until gameplay has been seen since the
  // last title / loading screen (gameplay_seen, cleared by game_text_hooks.cpp's file loads).
  mutable std::atomic<bool> gameplay_seen{false};
  bool GameInMenu() const { return MenuReason() != 0; }
  // Why the game counts as in a menu (0: gameplay): 1 a list menu (title menu, pause and its pages, the
  // book), 2..4 a menu the hooks saw that the game's marks miss (title lists, choice dialog, save list;
  // menu_mouse.h MarkOutsideMenu), 5 no gameplay seen since the last title / loading screen, 6 no game yet.
  int MenuReason() const {
    const uint8_t* base = guest_base.load();
    if (!base) return 6;
    auto byte = [base](uint32_t a) { return *reinterpret_cast<const volatile uint8_t*>(base + a); };
    if (byte(0x82757AB1) != 0) return 1;
    if (nier::menu_mouse::MenuSeenRecently()) return 1 + nier::menu_mouse::outside_menu().source.load();
    // 2026-11-12: after loading a save into some areas 0x827EB0D8 stayed 1 all through gameplay, so
    // gameplay was never seen (WASD dead, cursor shown); 0x827E2B54 = 4 is the second gameplay mark.
    if (byte(0x827EB0D8) == 0 || byte(0x827E2B54) == 4) {
      gameplay_seen = true;
      return 0;
    }
    // Before the first gameplay mark: the screens before the title menu (they take only Start, the click is
    // it) stay a menu. The others between the title and gameplay (save list, dialogs, title lists) mark
    // themselves now, so the loading and the first seconds of play no longer wait for the mark with the
    // cursor out and the click as A (2026-11-13: ~9 s after a load).
    return byte(0x826907DB) == 0 ? 5 : 0;
  }
  // 0x826907DB is 0 from boot through the opening videos, logos and "Press
  // START" and 1 from the title menu on (gameplay and pause included) - those
  // early screens take only Start, the later menus would close on it. Same
  // dump diff, plus 4 dumps of the opening videos.
  bool BeforeTitleMenu() const {
    const uint8_t* base = guest_base.load();
    return base && *reinterpret_cast<const volatile uint8_t*>(base + 0x826907DB) == 0;
  }

  std::mutex mutex;
  std::deque<uint16_t> presses;  // newly pressed buttons while open

  void Push(uint16_t buttons) {
    std::lock_guard lock(mutex);
    presses.push_back(buttons);
  }
  std::deque<uint16_t> Take() {
    std::lock_guard lock(mutex);
    std::deque<uint16_t> out;
    out.swap(presses);
    return out;
  }
};

inline PcOptionsState& pc_options_state() {
  static PcOptionsState state;
  return state;
}

// Fonts are added by NierApp::OnConfigureFonts (nullptr = ImGui default).
inline ImFont*& pc_options_title_font() { static ImFont* f = nullptr; return f; }
inline ImFont*& pc_options_font() { static ImFont* f = nullptr; return f; }

// The game's own menu art (game_assets.h), decoded by a background thread
// once the game data is mounted; the panel turns it into textures.
struct MenuArt {
  std::mutex mutex;
  std::atomic<bool> ready{false};
  std::map<std::string, GameImage> images;
};
inline MenuArt& menu_art() {
  static MenuArt art;
  return art;
}
inline const std::map<std::string, std::vector<std::string>>& MenuArtWanted() {
  static const std::map<std::string, std::vector<std::string>> wanted = [] {
    std::map<std::string, std::vector<std::string>> w = {
        {"MENU",
         {"menu_base_$book_a", "menu_base_$book_b", "menu_base_$book_d", "menu_base_$explanation"}},
        {"COMMON",
         {"back_back_D1", "back_moyo", "bt_icon_360_bt_icon_D5", "decision_line_line",
          "menu_com_line", "menu_com_title_icon", "scroll_scroll_top", "scroll_scroll_bottom",
          "arrow_varrow", "explanation_explanation_plate_D5"}},
    };
    // The on-screen buttons' art (touch_controls.h): Grimoire Weiss's sigils and the weapon/item icons (the PC's
    // touch screens too, 2026-11-17).
    for (const char* n : {"menu_word_edit_target_w_ssword", "menu_word_edit_target_w_lsword",
                          "menu_word_edit_target_p_dodge", "menu_word_edit_target_p_guard",
                          "menu_word_edit_target_m_lance", "menu_word_edit_target_m_hand",
                          "menu_word_edit_target_m_vulcan", "menu_word_edit_target_m_vision",
                          "menu_word_edit_target_m_round", "menu_word_edit_target_m_drain",
                          "menu_word_edit_target_m_wall", "menu_word_edit_target_m_ground"})
      w["MENU"].push_back(n);
    for (const char* n : {"icn_item_drug", "icn_arms_l_sword", "icn_arms_s_sword", "icn_arms_spear", "icn_we_fencing"})
      w["COMMON"].push_back(n);
    return w;
  }();
  return wanted;
}

struct PcOption {
  const char* label;
  const char* cvar;
  std::vector<std::pair<std::string, std::string>> values;  // cvar value, shown text
  bool needs_restart;
  const char* help;
  bool live_on_vulkan = false;  // applies while running when the game runs on Vulkan
  // Drawn as a slider (drag it, or step with left/right, which stop at the ends).
  bool slider = false;
};

// Slider values. Frame rate: 10 to 240 fps, then unlimited (0); 144 and 165 for those
// monitors. Resolution scale: 10% to 400% in steps of 5.
inline std::vector<std::pair<std::string, std::string>> FrameRateValues() {
  std::vector<std::pair<std::string, std::string>> v;
  for (int fps = 10; fps <= 240; fps += 5) {
    if (fps == 145) v.push_back({"144", "144 fps"});
    v.push_back({std::to_string(fps), std::to_string(fps) + " fps"});
  }
  v.push_back({"0", "Ilimitado"});
  return v;
}
inline std::vector<std::pair<std::string, std::string>> ScaleValues() {
  std::vector<std::pair<std::string, std::string>> v;
  for (int p = 10; p <= 400; p += 5) v.push_back({std::to_string(p), std::to_string(p) + "%"});
  return v;
}

// The option's value in its list; a slider's value not on it (an older nier.toml's 67%) shows at
// the nearest step (0, unlimited, past the last).
inline int OptionValueIndex(const PcOption& o) {
  const std::string v = rex::cvar::GetFlagByName(o.cvar);
  for (size_t i = 0; i < o.values.size(); ++i) {
    if (o.values[i].first == v) return int(i);
  }
  if (!o.slider) return 0;
  auto number = [](const std::string& s) { const int n = std::atoi(s.c_str()); return n == 0 ? 1 << 20 : n; };
  const int want = number(v);
  int best = 0;
  for (size_t i = 0; i < o.values.size(); ++i) {
    if (std::abs(number(o.values[i].first) - want) < std::abs(number(o.values[best].first) - want)) best = int(i);
  }
  return best;
}

// After a menu change: the frame rate limit also picks the game's own rate (fps_60: one
// refresh a frame above 30 fps, two at 30 or less).
inline void AfterOptionChange(const PcOption& o) {
  if (std::strcmp(o.cvar, "frame_rate_limit") == 0) {
    const int fps = std::atoi(rex::cvar::GetFlagByName("frame_rate_limit").c_str());
    rex::cvar::SetFlagByName("fps_60", fps == 0 || fps > 30 ? "true" : "false");
  }
}

// The Renut renderer has its own V-Sync, MSAA and anisotropy settings; the
// Xenos ones (vsync, swap_post_effect, native_2x_msaa, anisotropic_override,
// execute_unclipped_draw_vs_on_cpu) are not registered while it runs.
inline bool RenutRenderer() { return rex::cvar::GetFlagByName("nier_renderer") == "renut"; }

// Mouse options: on the controls pages, not with the video ones.
inline std::vector<PcOption> MouseOptions() {
  return {
      {"Mouse na cÃ¢mera", "mnk_mouse", {{"true", "Ligado"}, {"false", "Desligado (setas)"}},
       false, "Mouse gira a cÃ¢mera (o cursor some no jogo). Desligado: cÃ¢mera nas setas."},
      {"Sensibilidade do mouse", "mnk_sensitivity",
       {{"1.000000", "1"}, {"2.000000", "2"}, {"3.000000", "3"}, {"4.000000", "4"},
        {"5.000000", "5"}, {"6.000000", "6"}, {"8.000000", "8"}, {"10.000000", "10"}}, false,
       "Velocidade da cÃ¢mera com o mouse."},
  };
}
inline std::vector<PcOption> MakePcOptionsAll();
// The options this platform has: a phone is always full screen (a window there only broke the touch).
inline std::vector<PcOption> MakePcOptions() {
  std::vector<PcOption> options = MakePcOptionsAll();
  // The buttons' icons (game_text_hooks.cpp): the game ships both sheets; more styles can be added to the list.
  options.push_back({"Ãcones dos botÃµes", "nier_button_icons", {{"x360", "Xbox 360"}, {"ps3", "PlayStation 3"}}, false,
                     "Os Ã­cones do controle nas dicas e menus. Com teclado e mouse aparecem as teclas."});
#if defined(__ANDROID__)
  // 2026-11-20 (the player): no MSAA on phones either (not needed there; the renderer ignores it, surfaces.cpp).
  std::erase_if(options, [](const PcOption& o) {
    return std::string_view(o.cvar) == "fullscreen" || std::string_view(o.cvar) == "sa2_renut_msaa";
  });
#endif
  return options;
}
inline std::vector<PcOption> MakePcOptionsAll() {
  if (RenutRenderer()) {
    return {
        {"Medidor de desempenho", "perf_overlay", {{"false", "Desligado"}, {"true", "Ligado"}}, false,
         "Mostra o fps real do jogo, tempo de quadro, uso da GPU e da CPU. Atalho: F2."},
        {"Modo de tela", "fullscreen", {{"true", "Tela cheia"}, {"false", "Janela"}}, false,
         "Tela cheia ou janela."},
        {"ResoluÃ§Ã£o", "resolution",
         {{"360p", "360p"}, {"480p", "480p"}, {"640p", "640p"}, {"", "720p"}, {"1080p", "1080p"},
          {"1440p", "1440p"}, {"4k", "4K"}}, true,
         "Quantidade de pixels desenhados (e o tamanho da janela, de 720p para cima). "
         "Abaixo de 720p a imagem Ã© ampliada para a tela.", true},
        {"Escala de resoluÃ§Ã£o", "render_scale_percent", ScaleValues(), true,
         "Multiplica a resoluÃ§Ã£o. Abaixo de 100%: desenha menos pixels e amplia (use AMD FSR), "
         "bem mais leve. Acima: mais nÃ­tido e mais pesado.", true, true},
        {"Nitidez 3D", "dynamic_resolution_level",
         {{"-1", "AutomÃ¡tica"}, {"2", "MÃ­nima"}, {"1", "MÃ©dia"}, {"0", "MÃ¡xima"}}, false,
         "ResoluÃ§Ã£o 3D interna do jogo. AutomÃ¡tica: fica na MÃ¡xima enquanto o jogo alcanÃ§a o "
         "limite de fps e baixa sÃ³ o necessÃ¡rio quando nÃ£o alcanÃ§a."},
        {"Limite de fps", "frame_rate_limit", FrameRateValues(), false,
         "Quadros por segundo, atÃ© Ilimitado. Com V-Sync ligado a tela tambÃ©m limita "
         "(60 numa tela de 60 Hz). Mais fps pesa mais.", false, true},
        {"V-Sync", "renut_vsync", {{"true", "Ligado"}, {"false", "Desligado"}}, false,
         "Desligado: a imagem nÃ£o espera a tela (pode rasgar) e o fps passa da taxa da tela.",
         false},
        {"Anti-aliasing (MSAA)", "sa2_renut_msaa",
         {{"0", "Desligado"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}}, true,
         "Suaviza o serrilhado das bordas. Cada nÃ­vel pesa mais na placa de vÃ­deo."},
        {"Upscaling", "present_effect",
         {{"bilinear", "Bilinear"}, {"cas", "AMD CAS (nitidez)"}, {"fsr", "AMD FSR"}}, false,
         "Como a imagem Ã© ampliada para a tela. AMD FSR amplia com bordas nÃ­tidas, mas sÃ³ dÃ¡ desempenho com a "
         "Escala de resoluÃ§Ã£o abaixo de 100%: 75% = Qualidade, 67% = Equilibrado, 50% = Desempenho."},
        {"Filtro anisotrÃ³pico", "renut_anisotropic_filtering",
         {{"Game", "Do jogo"}, {"2x", "2x"}, {"4x", "4x"}, {"8x", "8x"}, {"16x", "16x"}}, false,
         "Texturas nÃ­tidas em Ã¢ngulos inclinados (chÃ£o, paredes)."},
        {"Ajuste da tela", "nier_screen_fit",
         {{"crop", "Preencher (corta)"}, {"bars", "Tarjas pretas"}, {"stretch", "Esticar"},
          {"wide", "Widescreen"}, {"widereal", "Widescreen real"}}, false,
         "O jogo Ã© 16:9. Em telas mais largas: Preencher amplia e corta um pouco em cima e embaixo; Tarjas "
         "pretas mostra a imagem inteira; Esticar enche a tela alargando a imagem; Widescreen enche a tela "
         "com o HUD na proporÃ§Ã£o certa no meio; Widescreen real abre a cÃ¢mera para os lados (mais visÃ£o, nada "
         "esticado) e leva o HUD para as bordas."},
        {"Brilho", "nier_brightness",
         {{"-2", "-2"}, {"-1", "-1"}, {"0", "Do jogo"}, {"1", "+1"}, {"2", "+2"}, {"3", "+3"}}, false,
         "Clareia ou escurece os tons mÃ©dios da imagem (preto e branco continuam iguais)."},
        {"Desfoque de perto", "nier_near_blur", {{"true", "Ligado (do jogo)"}, {"false", "Desligado"}}, false,
         "O borrÃ£o nos personagens quando a cÃ¢mera chega perto. Desligado: ficam nÃ­tidos (melhor em 4K). "
         "O desfoque de longe continua."},
        {"Sombras", "nier_shadows", {{"true", "Ligadas"}, {"false", "Desligadas"}}, false,
         "Sombras projetadas por personagens e objetos. Desligadas: mais leve, mais fps em aparelhos fracos."},

    };
  }
  return {
      {"Medidor de desempenho", "perf_overlay", {{"false", "Desligado"}, {"true", "Ligado"}}, false,
       "Mostra o fps real do jogo, tempo de quadro, uso da GPU e da CPU. Atalho: F2."},
      {"Modo de tela", "fullscreen", {{"true", "Tela cheia"}, {"false", "Janela"}}, false,
       "Tela cheia ou janela."},
      {"ResoluÃ§Ã£o", "resolution",
       {{"", "720p"}, {"1080p", "1080p"}, {"1440p", "1440p"}, {"4k", "4K"}}, true,
       "Tamanho da janela e quantidade de pixels desenhados.", true},
      {"Escala de resoluÃ§Ã£o", "render_scale_percent",
       {{"50", "50%"}, {"67", "67%"}, {"100", "100%"}, {"150", "150%"}, {"200", "200%"},
        {"300", "300%"}}, true,
       "Abaixo de 100%: desenha menos pixels e amplia (use AMD FSR), bem mais leve. "
       "Acima: desenha mais e reduz, mais nÃ­tido e mais pesado.", true},
      {"Nitidez da resoluÃ§Ã£o", "dynamic_resolution_level",
       {{"-1", "AutomÃ¡tica"}, {"2", "MÃ­nima"}, {"1", "MÃ©dia"}, {"0", "MÃ¡xima"}}, false,
       "ResoluÃ§Ã£o 3D interna do jogo. AutomÃ¡tica: o jogo baixa quando pesa. MÃ¡xima: nÃ­tido."},
      {"Nitidez mÃ­nima", "min_3d_resolution_percent",
       {{"60", "768x432 (original)"}, {"50", "640x360"}, {"40", "512x288"}}, true,
       "Tamanho da nitidez MÃ­nima (e da AutomÃ¡tica quando o jogo pesa). Menor = mais leve."},
      {"Taxa de quadros", "fps_60", {{"false", "30 fps"}, {"true", "60 fps"}}, true,
       "60 fps na gameplay. Mais pesado: em 1080p ou mais a placa pode nÃ£o acompanhar."},
      {"V-Sync", "vsync", {{"true", "Ligado"}, {"false", "Desligado (instÃ¡vel)"}}, false,
       "Desligado, o jogo perde o ritmo de quadros. Recomendado: ligado."},
      {"Anti-aliasing", "swap_post_effect",
       {{"none", "Desligado"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA mÃ¡ximo"}}, false,
       "Suaviza serrilhado nas bordas."},
      {"MSAA 2x", "native_2x_msaa", {{"true", "Ligado"}, {"false", "Desligado"}}, true,
       "Anti-serrilhado do prÃ³prio Xbox 360."},
      {"Upscaling", "present_effect",
       {{"bilinear", "Bilinear"}, {"cas", "AMD CAS (nitidez)"}, {"fsr", "AMD FSR"}}, false,
       "Como a imagem Ã© ampliada para a tela."},
      {"Filtro anisotrÃ³pico", "anisotropic_override",
       {{"0", "Desligado"}, {"2", "2x"}, {"3", "4x"}, {"4", "8x"}, {"5", "16x"}}, false,
       "Texturas nÃ­tidas em Ã¢ngulos inclinados (chÃ£o, paredes)."},
      {"Tarjas pretas", "present_letterbox", {{"true", "Ligadas"}, {"false", "Desligadas"}}, false,
       "MantÃ©m a proporÃ§Ã£o 16:9 em telas de outro formato."},
      {"Efeitos completos", "execute_unclipped_draw_vs_on_cpu",
       {{"true", "Ligado"}, {"false", "Desligado"}}, false,
       "NÃ©voa e outros efeitos de tela cheia. Recomendado: ligado."},

  };
}

// Writes only the PC settings into nier.toml, keeping every other line.
// (cvar::SaveConfig would also persist command-line-only values such as
// --headless or a temporary --game_data_root.)
inline void SavePcSettings(const std::filesystem::path& config_path) {
  std::vector<PcOption> options = MakePcOptions();
  for (auto& o : MouseOptions()) options.push_back(o);
  std::vector<std::string> lines;
  if (std::FILE* f = std::fopen(config_path.string().c_str(), "r")) {
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), f)) {
      std::string line(buf);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      bool ours = false;
      auto is_key = [&line](std::string_view key) {
        return line.rfind(key, 0) == 0 && line.size() > key.size() &&
               (line[key.size()] == ' ' || line[key.size()] == '=');
      };
      for (const auto& o : options) {
        if (is_key(o.cvar)) ours = true;
      }
      for (const KeyBindRow& row : KeyBindRows()) {
        if (is_key(row.cvar)) ours = true;
      }
      if (is_key("nier_language") || is_key("nier_pad_map") || is_key("nier_weapon_switch") || is_key("nier_tutorials_seen"))
        ours = true;
#if defined(__ANDROID__)
      // An MSAA left in a phone's nier.toml by an older build is dropped (the default, off, stays).
      if (is_key("sa2_renut_msaa")) ours = true;
#endif
      if (!ours) lines.push_back(line);
    }
    std::fclose(f);
  }
  for (const auto& o : options) {
    std::string v = rex::cvar::GetFlagByName(o.cvar);
    // Unquoted only for a whole number or a bool ("4x" would break the file).
    char* end = nullptr;
    std::strtod(v.c_str(), &end);
    const bool is_number = !v.empty() && end == v.c_str() + v.size();
    bool is_text = o.values.front().first.empty() || !(is_number || v == "true" || v == "false");
    lines.push_back(std::string(o.cvar) + " = " + (is_text ? "\"" + v + "\"" : v));
  }
  lines.push_back("nier_language = \"" + rex::cvar::GetFlagByName("nier_language") + "\"");
  // The controller's buttons (pad_remap.h), when changed.
  if (const std::string map = rex::cvar::GetFlagByName("nier_pad_map"); !map.empty())
    lines.push_back("nier_pad_map = \"" + map + "\"");
  lines.push_back("nier_weapon_switch = \"" + rex::cvar::GetFlagByName("nier_weapon_switch") + "\"");
  if (const std::string seen = rex::cvar::GetFlagByName("nier_tutorials_seen"); !seen.empty())
    lines.push_back("nier_tutorials_seen = \"" + seen + "\"");
  // Only changed binds, so later default changes still reach everyone else.
  for (const KeyBindRow& row : KeyBindRows()) {
    std::string v = rex::cvar::GetFlagByName(row.cvar);
    if (v != row.default_keys) {
      lines.push_back(std::string(row.cvar) + " = \"" + v + "\"");
    }
  }
  if (std::FILE* f = std::fopen(config_path.string().c_str(), "w")) {
    for (const auto& l : lines) std::fprintf(f, "%s\n", l.c_str());
    std::fclose(f);
    REXLOG_INFO("OpÃ§Ãµes de PC salvas em {}", config_path.string());
  }
}

class PcOptionsDialog final : public rex::ui::ImGuiDialog {
 public:
  PcOptionsDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path)
      : ImGuiDialog(drawer),
        config_path_(std::move(config_path)),
        options_(MakePcOptions()),
        running_vulkan_(rex::cvar::GetFlagByName("renderer") == "vulkan") {
    pc_options_state().config_path = config_path_;
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    auto& state = pc_options_state();
    if (state.toggle_requested.exchange(false)) {
      SetOpen(!state.open);
      // Opened from the game's "Control Options": straight to that page, and
      // backing out of it closes the panel (back to the game's menu).
      controls_only_ = state.open && state.open_page == 2;
      language_only_ = state.open && state.open_page == 3;
      if (controls_only_) OpenControls();
      if (language_only_) OpenLanguage();
    }
    {  // TEMP (dev): NIER_PANEL_DEMO=<seconds>|<settings|controls> opens the
       // panel by itself, for screenshots in replay runs.
      static const char* demo = std::getenv("NIER_PANEL_DEMO");
      static bool demo_done = false;
      if (demo && !demo_done && ImGui::GetTime() > std::atof(demo)) {
        demo_done = true;
        SetOpen(true);
        if (std::strstr(demo, "controls")) {
          OpenControls();
          bind_selected_ = 5;
        } else if (std::strstr(demo, "language")) {
          OpenLanguage();
        } else {
          selected_ = 14;
          EnsureVisible();
        }
      }
    }
    if (!state.open) {
      return;
    }
    // The cursor stays visible while open: mouse look's release restores
    // fullscreen's "hidden until moved", which would leave it invisible.
    if (rex::ui::Window* window = imgui_drawer()->window();
        window && window->GetCursorVisibility() != rex::ui::Window::CursorVisibility::kVisible) {
      window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    }
    // With a pad connected the keyboard is not a pad (mnk_mode off), so the
    // panel reads the keys itself.
    if (capturing_ < 0 && ImGui::GetTime() >= ignore_pad_until_ &&
        rex::cvar::GetFlagByName("mnk_mode") != "true") {
      auto pressed = [](std::initializer_list<ImGuiKey> keys) {
        for (ImGuiKey k : keys) {
          if (ImGui::IsKeyPressed(k)) return true;
        }
        return false;
      };
      if (pressed({ImGuiKey_UpArrow, ImGuiKey_W})) Move(-1);
      if (pressed({ImGuiKey_DownArrow, ImGuiKey_S})) Move(+1);
      if (pressed({ImGuiKey_LeftArrow, ImGuiKey_A})) Change(-1);
      if (pressed({ImGuiKey_RightArrow, ImGuiKey_D})) Change(+1);
      if (pressed({ImGuiKey_Enter, ImGuiKey_Space})) Activate();
      if (pressed({ImGuiKey_Escape, ImGuiKey_Backspace})) Back();
      if (!state.open) return;
    }
    if (capturing_ >= 0) {
      state.Take();  // the key being captured also reaches the pad gate
      CaptureKey();
    } else if (ImGui::GetTime() < ignore_pad_until_) {
      state.Take();  // late pad echo of the key just captured
    } else {
      for (uint16_t b : state.Take()) {
        if (b & kPadUp) Move(-1);
        if (b & kPadDown) Move(+1);
        if (b & kPadLeft) Change(-1);
        if (b & kPadRight) Change(+1);
        if (b & kPadA) Activate();
        if (b & (kPadB | kPadStart)) Back();
        if (!state.open) return;
      }
    }
    DrawPanel(io);
  }

 private:
  enum class Page { kSettings, kControls, kLanguage };
  static constexpr int kVisibleRows = 9;  // as many as the game's Game Settings shows

  // Settings page: the options, then links to the controls and language pages.
  // Controls page: the binds, then "Aplicar" and "Restaurar padrÃ£o". Language
  // page: one row per language.
  int RowCount() const {
    switch (page_) {
      case Page::kSettings: return int(options_.size()) + 2;
      case Page::kControls: return int(KeyBindRows().size()) + 2;
      case Page::kLanguage: return int(languages_.size());
    }
    return 0;
  }
  int& Selected() {
    return page_ == Page::kSettings ? selected_
           : page_ == Page::kControls ? bind_selected_
                                      : lang_selected_;
  }
  int& Scroll() {
    return page_ == Page::kSettings ? scroll_ : page_ == Page::kControls ? bind_scroll_ : lang_scroll_;
  }
  bool IsLinkRow(int i) const { return page_ == Page::kSettings && i >= int(options_.size()); }
  bool IsLanguageLink(int i) const { return page_ == Page::kSettings && i == int(options_.size()) + 1; }
  int ApplyRow() const { return int(KeyBindRows().size()); }
  int DefaultRow() const { return int(KeyBindRows().size()) + 1; }

  void EnsureVisible() {
    int& scroll = Scroll();
    int sel = Selected();
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + kVisibleRows) scroll = sel - kVisibleRows + 1;
    scroll = std::clamp(scroll, 0, std::max(0, RowCount() - kVisibleRows));
  }

  void Activate() {
    int sel = Selected();
    if (page_ == Page::kSettings) {
      if (IsLanguageLink(sel)) {
        OpenLanguage();
      } else if (IsLinkRow(sel)) {
        OpenControls();
      } else {
        Change(+1);
      }
      return;
    }
    if (page_ == Page::kLanguage) {
      if (sel >= 0 && sel < int(languages_.size()) && text::Select(languages_[sel].name)) {
        dirty_ = true;
        applied_until_ = ImGui::GetTime() + 2.0;
      }
      return;
    }
    if (sel == ApplyRow()) {
      ApplyBinds();
    } else if (sel == DefaultRow()) {
      for (size_t i = 0; i < KeyBindRows().size(); ++i) {
        pending_[i] = KeyBindRows()[i].default_keys;
      }
    } else {
      capturing_ = sel;
      capture_frame_ = ImGui::GetFrameCount();
    }
  }

  void Back() {
    if ((page_ == Page::kControls && !controls_only_) ||
        (page_ == Page::kLanguage && !language_only_)) {
      page_ = Page::kSettings;  // unapplied changes are dropped, as in Replicant
      return;
    }
    SetOpen(false);
  }

  void OpenLanguage() {
    languages_ = text::Languages();  // also picks up folders added since
    const std::string current = text::CurrentName();
    lang_selected_ = 0;
    for (size_t i = 0; i < languages_.size(); ++i) {
      if (languages_[i].name == current) lang_selected_ = int(i);
    }
    lang_scroll_ = 0;
    page_ = Page::kLanguage;
    EnsureVisible();
  }

  void OpenControls() {
    pending_.clear();
    for (const KeyBindRow& row : KeyBindRows()) {
      pending_.push_back(rex::cvar::GetFlagByName(row.cvar));
    }
    page_ = Page::kControls;
    bind_selected_ = 0;
    bind_scroll_ = 0;
  }

  bool BindsPending() const {
    for (size_t i = 0; i < pending_.size(); ++i) {
      if (pending_[i] != rex::cvar::GetFlagByName(KeyBindRows()[i].cvar)) return true;
    }
    return false;
  }

  void ApplyBinds() {
    for (size_t i = 0; i < pending_.size(); ++i) {
      rex::cvar::SetFlagByName(KeyBindRows()[i].cvar, pending_[i]);
    }
    Save();
    applied_until_ = ImGui::GetTime() + 2.0;
  }

  // The new key takes the row's first slot; a row that had it as its first key
  // gets this row's old one (a swap, as in Replicant).
  void SetPrimary(int row, const std::string& key) {
    std::string old = PrimaryKey(pending_[row]);
    for (size_t i = 0; i < pending_.size(); ++i) {
      if (int(i) != row && PrimaryKey(pending_[i]) == key) {
        pending_[i] = WithPrimaryKey(pending_[i], old);
      }
    }
    pending_[row] = WithPrimaryKey(pending_[row], key);
  }

  // Keys a bind can take, as ImGui key -> SDK keybind name. F keys and the
  // backtick stay with the overlays (F1 panel, F2 meter, console).
  static const std::vector<std::pair<ImGuiKey, std::string>>& CaptureKeys() {
    static const auto keys = [] {
      std::vector<std::pair<ImGuiKey, std::string>> k;
      for (int i = 0; i < 26; ++i) k.emplace_back(ImGuiKey(ImGuiKey_A + i), std::string(1, char('A' + i)));
      for (int i = 0; i < 10; ++i) k.emplace_back(ImGuiKey(ImGuiKey_0 + i), std::string(1, char('0' + i)));
      for (int i = 0; i < 10; ++i) k.emplace_back(ImGuiKey(ImGuiKey_Keypad0 + i), "Numpad" + std::to_string(i));
      const std::pair<ImGuiKey, const char*> named[] = {
          {ImGuiKey_Space, "Space"},        {ImGuiKey_Enter, "Return"},
          {ImGuiKey_Tab, "Tab"},            {ImGuiKey_Backspace, "Backspace"},
          {ImGuiKey_Delete, "Delete"},      {ImGuiKey_Insert, "Insert"},
          {ImGuiKey_Home, "Home"},          {ImGuiKey_End, "End"},
          {ImGuiKey_PageUp, "PageUp"},      {ImGuiKey_PageDown, "PageDown"},
          {ImGuiKey_LeftArrow, "Left"},     {ImGuiKey_RightArrow, "Right"},
          {ImGuiKey_UpArrow, "Up"},         {ImGuiKey_DownArrow, "Down"},
          {ImGuiKey_LeftShift, "Shift"},    {ImGuiKey_RightShift, "Shift"},
          {ImGuiKey_LeftCtrl, "Control"},   {ImGuiKey_RightCtrl, "Control"},
          {ImGuiKey_LeftAlt, "Alt"},        {ImGuiKey_RightAlt, "Alt"},
          {ImGuiKey_Semicolon, "Semicolon"}, {ImGuiKey_Apostrophe, "Quote"},
          {ImGuiKey_Comma, "Comma"},        {ImGuiKey_Period, "Period"},
          {ImGuiKey_Slash, "Slash"},        {ImGuiKey_Backslash, "Backslash"},
          {ImGuiKey_LeftBracket, "LBracket"}, {ImGuiKey_RightBracket, "RBracket"},
          {ImGuiKey_Minus, "Minus"},        {ImGuiKey_Equal, "Plus"},
          {ImGuiKey_CapsLock, "CapsLock"},
      };
      for (auto [key, name] : named) k.emplace_back(key, name);
      return k;
    }();
    return keys;
  }

  void CaptureKey() {
    if (ImGui::GetFrameCount() == capture_frame_) return;  // the click that started it
    std::string key;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      capturing_ = -1;
      ignore_pad_until_ = ImGui::GetTime() + 0.3;
      return;
    }
    for (const auto& [imgui_key, name] : CaptureKeys()) {
      if (ImGui::IsKeyPressed(imgui_key, false)) {
        key = name;
        break;
      }
    }
    if (key.empty()) {
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) key = "LMB";
      else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) key = "RMB";
      else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) key = "MMB";
    }
    if (key.empty()) return;
    SetPrimary(capturing_, key);
    capturing_ = -1;
    ignore_pad_until_ = ImGui::GetTime() + 0.3;
  }
  int CurrentIndex(const PcOption& o) const { return OptionValueIndex(o); }

  bool NeedsRestart(const PcOption& o) const {
    return o.needs_restart && !(o.live_on_vulkan && running_vulkan_);
  }

  void Move(int d) {
    Selected() = (Selected() + d + RowCount()) % RowCount();
    EnsureVisible();
  }

  void Change(int d) {
    if (page_ != Page::kSettings || IsLinkRow(selected_)) return;
    const PcOption& o = options_[selected_];
    const int n = int(o.values.size());
    SetIndex(o, o.slider ? std::clamp(CurrentIndex(o) + d, 0, n - 1) : (CurrentIndex(o) + d + n) % n);
  }

  void SetIndex(const PcOption& o, int i) {
    if (i == CurrentIndex(o) && rex::cvar::GetFlagByName(o.cvar) == o.values[i].first) return;
    if (rex::cvar::SetFlagByName(o.cvar, o.values[i].first)) {
      AfterOptionChange(o);
      dirty_ = true;
      if (NeedsRestart(o)) restart_pending_ = true;
    } else {
      REXLOG_WARN("OpÃ§Ãµes de PC: {} = {} rejeitado", o.cvar, o.values[i].first);
    }
  }

  void SetOpen(bool open) {
    auto& state = pc_options_state();
    if (state.open == open) return;
    state.open = open;
    state.Take();
    page_ = Page::kSettings;
    capturing_ = -1;
    // The panel needs the cursor: mouse look lets it go while open.
    rex::input::mnk::SetMouseLookActive(!open);
    if (rex::ui::Window* window = imgui_drawer()->window(); window && !open) {
      // Back to the window's own policy (mouse look then hides it again).
      window->SetCursorVisibility(window->IsFullscreen()
                                      ? rex::ui::Window::CursorVisibility::kAutoHidden
                                      : rex::ui::Window::CursorVisibility::kVisible);
    }
    if (!open && dirty_) {
      Save();
      dirty_ = false;
    }
  }

  // Writes only the panel's own settings into nier.toml, keeping every other
  // line. (cvar::SaveConfig would also persist command-line-only values such as
  // --headless or a temporary --game_data_root.)
  void Save() { SavePcSettings(config_path_); }

  // Textures from the game's menu art, created once it is decoded. A new
  // immediate drawer (presenter recreated) invalidates the old ones; they are
  // let go of rather than destroyed against a drawer that no longer exists.
  void EnsureTextures() {
    rex::ui::ImmediateDrawer* drawer = imgui_drawer()->immediate_drawer();
    if (drawer != texture_owner_) {
      for (auto& [name, texture] : textures_) texture.release();
      textures_.clear();
      texture_owner_ = drawer;
    }
    auto& art = menu_art();
    if (!drawer || !textures_.empty() || !art.ready) return;
    std::lock_guard lock(art.mutex);
    for (const auto& [name, image] : art.images) {
      textures_[name] = drawer->CreateTexture(uint32_t(image.width), uint32_t(image.height),
                                              rex::ui::ImmediateTextureFilter::kLinear, false,
                                              image.rgba.data());
    }
  }
  ImTextureID Texture(const char* name) const {
    auto it = textures_.find(name);
    return it == textures_.end() || !it->second ? ImTextureID{}
                                                : reinterpret_cast<ImTextureID>(it->second.get());
  }

  void DrawPanel(ImGuiIO& io) {
    EnsureTextures();
    const ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Laid out like the game's Game Settings screen, in 1920x1080 units
    // (measured on it) fitted to the window.
    const float scale = std::min(screen.x / 1920.0f, screen.y / 1080.0f);
    const float ox = (screen.x - 1920.0f * scale) * 0.5f, oy = (screen.y - 1080.0f * scale) * 0.5f;
    auto P = [&](float x, float y) { return ImVec2(ox + x * scale, oy + y * scale); };
    auto image = [&](const char* name, ImVec2 a, ImVec2 b, ImVec2 uv0 = {0, 0},
                     ImVec2 uv1 = {1, 1}, ImU32 tint = IM_COL32_WHITE) {
      if (ImTextureID id = Texture(name)) dl->AddImage(id, a, b, uv0, uv1, tint);
      return Texture(name) != ImTextureID{};
    };

    ImFont* font = pc_options_font() ? pc_options_font() : ImGui::GetFont();
    const float text_size = 40 * scale, small_size = 30 * scale;
    const ImU32 ink = IM_COL32(26, 30, 44, 255), paper = IM_COL32(214, 212, 204, 255);
    const ImU32 light = IM_COL32(236, 234, 228, 255);
    auto ink_text = [&](ImVec2 pos, float size, const char* text) {
      dl->AddText(font, size, {pos.x + 1.5f * scale, pos.y + 1.5f * scale},
                  IM_COL32(255, 255, 255, 110), text);  // the game's slight emboss
      dl->AddText(font, size, pos, ink, text);
    };
    const bool settings = page_ == Page::kSettings;
    const bool lang = page_ == Page::kLanguage;
    const int count = RowCount();
    const int visible = std::min(count, kVisibleRows);
    int& scroll = Scroll();
    scroll = std::clamp(scroll, 0, count - visible);

    // A capture that ended this frame owns this frame's clicks.
    const bool mouse_free = capturing_ < 0 && capture_frame_ != ImGui::GetFrameCount() &&
                            ImGui::GetTime() >= ignore_pad_until_;
    const bool mouse_moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    const bool clicked = mouse_free && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (mouse_free && io.MouseWheel != 0.0f) {
      scroll = std::clamp(scroll - int(io.MouseWheel), 0, count - visible);
    }

    // The book: crumpled backdrop, the page with its spine, the stacked page
    // edges on the right and the lace at the bottom. Plain fills stand in
    // until the art is decoded (or if the game data cannot be read).
    dl->AddRectFilled({0, 0}, screen, IM_COL32(60, 60, 60, 255));
    image("back_back_D1", {0, 0}, screen);
    if (!image("menu_base_$book_a", P(0, 0), P(1730, 1080))) {
      dl->AddRectFilled(P(0, 0), P(1730, 1080), paper);
    }
    image("menu_base_$book_b", P(1722, 0), P(1790, 1080));
    image("menu_base_$book_d", P(0, 1000), P(1760, 1064));
    image("back_moyo", P(1400, 520), P(1720, 840), {0, 0}, {1, 1}, IM_COL32(255, 255, 255, 70));

    // Key hints on the dark strip at the top left. With a pad, the game's
    // button icons; on the keyboard, key caps. "Voltar" is clickable.
    bool back_clicked = false;
    {
      const bool pad = rex::cvar::GetFlagByName("mnk_mode") != "true";
      float x = 150;
      // The dark strip reaches past the last hint, as on the game's screens.
      image("explanation_explanation_plate_D5", P(0, 95), P(settings ? 1000 : 860, 141));
      const float hy = 100;
      struct Hint { int icon_x, icon_y; const char* key; const char* action; };
      std::vector<Hint> hints = {{1, 5, "W/S", "Selecionar"}};
      if (settings) hints.push_back({2, 3, "A/D", "Mudar"});
      hints.push_back({0, 0, "Enter", settings || lang ? "Confirmar" : "Trocar tecla"});
      hints.push_back({1, 0, "Esc", "Voltar"});
      for (const Hint& h : hints) {
        float start = x;
        if (pad && Texture("bt_icon_360_bt_icon_D5")) {
          image("bt_icon_360_bt_icon_D5", P(x, hy), P(x + 36, hy + 36),
                {h.icon_x / 4.0f, h.icon_y / 6.0f}, {(h.icon_x + 1) / 4.0f, (h.icon_y + 1) / 6.0f});
          x += 42;
        } else {
          float w = KeyBoxWidth(font, small_size, h.key, scale) / scale;
          DrawKeyBox(dl, font, small_size, P(x, hy - 1), h.key, light, scale);
          x += w + 8;
        }
        dl->AddText(font, small_size, P(x, hy), light, h.action);
        x += font->CalcTextSizeA(small_size, FLT_MAX, 0, h.action).x / scale + 26;
        if (std::string_view(h.action) == "Voltar" && mouse_free &&
            ImGui::IsMouseHoveringRect(P(start, 95), P(x, 141), false) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          back_clicked = true;
        }
      }
    }

    // Ornament and the screen's instruction line.
    image("menu_com_title_icon", P(200, 148), P(246, 202));
    ink_text(P(258, 152), text_size,
             settings ? "VÃ­deo: cima/baixo escolhe, esquerda/direita ajusta."
             : lang   ? "Idioma: escolha e confirme. O texto muda na hora."
                      : "Controles: escolha uma aÃ§Ã£o e aperte a tecla nova.");

    // Rows, as on Game Settings: label left, value centered, a ruled line
    // under each, the white brush stroke behind the selected label.
    const float left = 381, right = 1351, value_center = 1131, row = 57, y0 = 262;
    int clicked_row = -1, clicked_dir = +1;
    for (int v = 0; v < visible; ++v) {
      const int i = scroll + v;
      const float y = y0 + v * row;
      const ImVec2 a = P(left - 20, y - 6), b = P(right, y + row - 8);
      const bool hover = mouse_free && ImGui::IsMouseHoveringRect(a, b, false);
      if (hover && mouse_moved) Selected() = i;
      const bool sel = i == Selected();
      // The game's ruled line is dark ink (menu_com_line is a light variant).
      dl->AddLine(P(left - 16, y + 48), P(right + 14, y + 48), ink, 2.5f * scale);
      if (sel && !image("decision_line_line", P(left - 18, y - 2), P(left + 372, y + 46))) {
        dl->AddRectFilled(P(left - 18, y), P(left + 372, y + 44), IM_COL32(250, 250, 250, 200));
      }

      std::string label, value;
      bool value_is_key = false;
      const PcOption* slider = settings && !IsLinkRow(i) && options_[i].slider ? &options_[i] : nullptr;
      if (settings && IsLinkRow(i)) {
        label = IsLanguageLink(i) ? "Idioma do texto" : "Controles do teclado";
        value = ">";
      } else if (lang) {
        label = languages_[i].name;
        if (languages_[i].name == text::CurrentName()) value = "Em uso";
      } else if (settings) {
        const PcOption& o = options_[i];
        label = std::string(o.label) + (NeedsRestart(o) ? " *" : "");
        value = o.values[CurrentIndex(o)].second;
        if (sel) value = "<   " + value + "   >";
      } else if (i == ApplyRow()) {
        label = "Aplicar mudanÃ§as";
      } else if (i == DefaultRow()) {
        label = "Restaurar padrÃ£o";
      } else {
        label = KeyBindRows()[i].label;
        if (capturing_ == i) {
          value = "Pressione uma tecla (Esc cancela)";
        } else {
          value = KeyDisplayName(PrimaryKey(pending_[i]));
          value_is_key = true;
        }
      }
      ink_text(P(left + 14, y), text_size, label.c_str());
      if (slider) {
        // A track with the game's ink, a knob at the value, the value at its right. Dragging
        // (or clicking) on the track sets it; left/right step it.
        const float t0 = 880, t1 = 1200, ty = y + 24;
        const int n = int(slider->values.size());
        const int cur = CurrentIndex(*slider);
        const float kx = t0 + (t1 - t0) * (n > 1 ? float(cur) / float(n - 1) : 0.0f);
        dl->AddRectFilled(P(t0, ty - 3), P(t1, ty + 3), IM_COL32(90, 92, 100, 255), 3 * scale);
        dl->AddRectFilled(P(t0, ty - 3), P(kx, ty + 3), ink, 3 * scale);
        dl->AddCircleFilled(P(kx, ty), (sel ? 13 : 10) * scale, ink);
        dl->AddCircleFilled(P(kx, ty), (sel ? 7 : 5) * scale, light);
        const std::string& shown = slider->values[cur].second;
        ink_text(P(t1 + 26, y + 6), small_size, shown.c_str());
        if (mouse_free && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            (dragging_ == i || (dragging_ < 0 && ImGui::IsMouseHoveringRect(P(t0 - 20, y - 6), P(t1 + 20, y + row - 8), false) &&
                                ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
          dragging_ = i;
          Selected() = i;
          const float f = std::clamp((io.MousePos.x - P(t0, 0).x) / (P(t1, 0).x - P(t0, 0).x), 0.0f, 1.0f);
          SetIndex(*slider, int(std::lround(f * float(n - 1))));
        }
      } else if (value_is_key) {
        float w = KeyBoxWidth(font, small_size, value.c_str(), scale) / scale;
        DrawKeyBox(dl, font, small_size, P(value_center - w * 0.5f, y + 4), value.c_str(), ink,
                   scale);
      } else if (!value.empty()) {
        float w = font->CalcTextSizeA(text_size, FLT_MAX, 0, value.c_str()).x / scale;
        ink_text(P(value_center - w * 0.5f, y), text_size, value.c_str());
        // Clicking the left half of a value steps it back, anywhere else forward.
        if (hover && io.MousePos.x < P(value_center, 0).x &&
            io.MousePos.x > P(value_center - w * 0.5f, 0).x) {
          clicked_dir = -1;
        }
      }
      if (hover && clicked && !slider) clicked_row = i;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging_ = -1;

    // Scroll arrows (clickable) and the vine scroll bar at the right.
    const float mid = 838;
    auto arrow = [&](float ty, bool up) {
      // Dark ink arrows like the game's (arrow_varrow is the white one).
      float d = up ? -1.0f : 1.0f;
      dl->AddTriangleFilled(P(mid - 14, ty - 6 * d), P(mid + 14, ty - 6 * d), P(mid, ty + 6 * d), ink);
      return clicked && ImGui::IsMouseHoveringRect(P(mid - 40, ty - 18), P(mid + 40, ty + 18));
    };
    if (scroll > 0 && arrow(y0 - 32, true)) --scroll;
    if (scroll + visible < count && arrow(y0 + visible * row + 6, false)) ++scroll;
    if (count > visible) {
      const float bar_top = 255, bar_bottom = 775, bx = 1381;
      image("scroll_scroll_top", P(bx - 9, bar_top - 50), P(bx + 9, bar_top));
      image("scroll_scroll_bottom", P(bx - 9, bar_bottom), P(bx + 9, bar_bottom + 50));
      dl->AddRectFilled(P(bx - 2, bar_top), P(bx + 2, bar_bottom), ink);
      float t0 = bar_top + (bar_bottom - bar_top) * scroll / count;
      float t1 = bar_top + (bar_bottom - bar_top) * (scroll + visible) / count;
      dl->AddRectFilled(P(bx - 2, t0), P(bx + 2, t1), IM_COL32(245, 245, 240, 255));
    }

    const char* help;
    const char* footer;
    bool footer_bright;
    if (lang) {
      help = "Idiomas do disco e cada pasta de TEXT. Veja TEXT\\EXAMPLE\\LEIA-ME.txt.";
      if (ImGui::GetTime() < applied_until_) {
        footer = "Idioma aplicado e salvo.";
        footer_bright = true;
      } else {
        footer = "Pastas novas de TEXT aparecem aqui quando esta tela abre.";
        footer_bright = false;
      }
    } else if (settings) {
      help = IsLanguageLink(selected_) ? "Escolhe o idioma do texto do jogo, ou uma traduÃ§Ã£o."
             : IsLinkRow(selected_)    ? "Troca as teclas do teclado e do mouse."
                                       : options_[selected_].help;
      footer = restart_pending_ ? "* Feche e abra o jogo para aplicar.        Esc: fechar e salvar"
                                : "* Vale quando o jogo for aberto de novo.   Esc: fechar e salvar";
      footer_bright = restart_pending_;
    } else {
      help = "Enter ou clique: escolher a tecla nova. Clique direito ou Esc: voltar.";
      if (ImGui::GetTime() < applied_until_) {
        footer = "Teclas aplicadas e salvas.";
        footer_bright = true;
      } else if (BindsPending()) {
        footer = "HÃ¡ mudanÃ§as: escolha \"Aplicar mudanÃ§as\" (voltar descarta).";
        footer_bright = true;
      } else {
        footer = "Fixas: Enter tambÃ©m confirma, Esc tambÃ©m abre o menu, Backspace tambÃ©m volta.";
        footer_bright = false;
      }
    }
    // Help box with the tape, as on the game's screens.
    if (!image("menu_base_$explanation", P(634, 845), P(1766, 1000))) {
      dl->AddRectFilled(P(634, 845), P(1766, 1000), IM_COL32(200, 204, 210, 255));
    }
    ink_text(P(706, 862), small_size, help);
    dl->AddText(font, small_size * 0.85f, P(706, 912),
                footer_bright ? ink : IM_COL32(70, 74, 84, 255), footer);
    (void)footer_bright;

    // Act after drawing, so a page switch never mixes two pages in one frame.
    if (clicked_row >= 0) {
      Selected() = clicked_row;
      if (settings && !IsLinkRow(clicked_row)) {
        Change(clicked_dir);
      } else {
        Activate();
      }
    } else if (back_clicked || (mouse_free && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
      Back();
    }
  }

  static float KeyBoxWidth(ImFont* font, float size, const char* text, float scale) {
    return font->CalcTextSizeA(size, FLT_MAX, 0, text).x + 16 * scale;
  }

  // A key cap like Replicant's: the name in a thin rounded box. Returns its width.
  static float DrawKeyBox(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, const char* text,
                          ImU32 color, float scale) {
    float w = KeyBoxWidth(font, size, text, scale);
    dl->AddRect(pos, {pos.x + w, pos.y + size + 8 * scale}, color, 4 * scale, 0, 1.5f * scale);
    dl->AddText(font, size, {pos.x + 8 * scale, pos.y + 3 * scale}, color, text);
    return w;
  }

  std::filesystem::path config_path_;
  std::vector<PcOption> options_;
  bool running_vulkan_;
  Page page_ = Page::kSettings;
  int selected_ = 0;
  int scroll_ = 0;
  int bind_selected_ = 0;
  int bind_scroll_ = 0;
  std::vector<std::string> pending_;  // controls page: bind values not applied yet
  int capturing_ = -1;                // bind row waiting for a key
  int dragging_ = -1;                 // slider row the mouse is dragging
  int capture_frame_ = -1;
  double ignore_pad_until_ = 0.0;
  double applied_until_ = 0.0;
  bool dirty_ = false;
  bool restart_pending_ = false;
  bool controls_only_ = false;  // opened from the game's Control Options
  bool language_only_ = false;  // opened from the game's Language
  std::vector<text::Language> languages_;  // language page, as of when it opened
  int lang_selected_ = 0;
  int lang_scroll_ = 0;
  std::map<std::string, std::unique_ptr<rex::ui::ImmediateTexture>> textures_;
  rex::ui::ImmediateDrawer* texture_owner_ = nullptr;
};

}  // namespace nier
