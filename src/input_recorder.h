// nier - controller input recording and replay
//
// NIER_INPUT_RECORD=<file>  wraps the SDL driver and logs every change of the
//                           pad state the game reads, plus keystrokes.
// NIER_INPUT_REPLAY=<file>  replaces all real input with the recorded stream.
//
// Events are keyed by the number of state polls the game has made (it polls
// once per game frame), so replay follows the game's own pace instead of wall
// time. Wall time is written alongside to measure drift between runs.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <SDL3/SDL_keyboard.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "menu_mouse.h"
#include "title_carousel.h"
#include "mods.h"
#include "pc_options.h"
#include "pad_remap.h"
#include "weapon_switch.h"
#include "touch_controls.h"

void MenuDebugTick();  // TEMP (MENUDBG), video_options_menu.cpp

namespace nier {

using rex::X_RESULT;
using rex::X_STATUS;

struct PadSample {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;

  static PadSample From(const rex::input::X_INPUT_GAMEPAD& g) {
    return {g.buttons, g.left_trigger, g.right_trigger, g.thumb_lx, g.thumb_ly, g.thumb_rx, g.thumb_ry};
  }
  void To(rex::input::X_INPUT_GAMEPAD& g) const {
    g.buttons = buttons;
    g.left_trigger = lt;
    g.right_trigger = rt;
    g.thumb_lx = lx;
    g.thumb_ly = ly;
    g.thumb_rx = rx;
    g.thumb_ry = ry;
  }
  bool operator==(const PadSample&) const = default;
};

class InputClock {
 public:
  uint64_t ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start_)
        .count();
  }

 private:
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// Forwards to a real driver and logs what the game reads from it.
class RecordingDriver final : public rex::input::InputDriver {
 public:
  RecordingDriver(std::unique_ptr<rex::input::InputDriver> inner, const std::string& path)
      : InputDriver(nullptr, 0), inner_(std::move(inner)) {
    file_ = std::fopen(path.c_str(), "w");
    if (file_) {
      std::fputs("# nier input recording v1\n# S,poll,ms,buttons,lt,rt,lx,ly,rx,ry\n"
                 "# K,poll,ms,virtual_key,unicode,flags,hid_code\n", file_);
    }
    REXLOG_INFO("Input: recording to {} ({})", path, file_ ? "ok" : "FAILED to open");
  }
  ~RecordingDriver() override {
    if (file_) {
      std::fprintf(file_, "E,%llu,%llu\n", (unsigned long long)polls_, (unsigned long long)clock_.ms());
      std::fclose(file_);
    }
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }  // inner is set up by the caller
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    inner_->EnumerateDevices(out);
  }
  void OnWindowAvailable(rex::ui::Window* window) override { inner_->OnWindowAvailable(window); }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    X_RESULT r = inner_->GetDeviceState(id, out);
    std::lock_guard lock(mutex_);
    ++polls_;
    PadSample s = (r == X_ERROR_SUCCESS && out) ? PadSample::From(out->gamepad) : PadSample{};
    if (file_ && (polls_ == 1 || !(s == last_))) {
      std::fprintf(file_, "S,%llu,%llu,%04X,%u,%u,%d,%d,%d,%d\n", (unsigned long long)polls_,
                   (unsigned long long)clock_.ms(), s.buttons, s.lt, s.rt, s.lx, s.ly, s.rx, s.ry);
      std::fflush(file_);  // survive a crash
      last_ = s;
    } else if (file_ && polls_ % 300 == 0) {
      // Heartbeat: poll rate and a reference for replay drift.
      std::fprintf(file_, "H,%llu,%llu\n", (unsigned long long)polls_, (unsigned long long)clock_.ms());
      std::fflush(file_);
    }
    return r;
  }
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 rex::input::X_INPUT_CAPABILITIES* out) override {
    return inner_->GetDeviceCapabilities(id, flags, out);
  }
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION* v) override {
    return inner_->SetDeviceVibration(id, v);
  }
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              rex::input::X_INPUT_KEYSTROKE* out) override {
    X_RESULT r = inner_->GetDeviceKeystroke(id, flags, out);
    if (r == X_ERROR_SUCCESS && out && file_) {
      std::lock_guard lock(mutex_);
      std::fprintf(file_, "K,%llu,%llu,%u,%u,%u,%u\n", (unsigned long long)polls_,
                   (unsigned long long)clock_.ms(), (unsigned)out->virtual_key,
                   (unsigned)out->unicode, (unsigned)out->flags, (unsigned)out->hid_code);
      std::fflush(file_);
    }
    return r;
  }

 private:
  std::unique_ptr<rex::input::InputDriver> inner_;
  std::FILE* file_ = nullptr;
  std::mutex mutex_;
  uint64_t polls_ = 0;
  PadSample last_;
  InputClock clock_;
};

// Sits between a real pad and the game: Back+Start toggles the PC options
// panel, and while it is open the game sees an idle pad and the presses go to
// the panel. After closing, input stays muted until every button is released
// so the closing press never reaches the game.
// The key hints bar (0x82757AB0: +1 shown with a list menu; 8 slots of 76 bytes from +0, icon at +20, the text in
// UTF-16 from +24) holds our "RB Jukebox" in its fourth slot (video_options_menu.cpp adds it on the pause's cross).
inline bool JukeboxHintShown(const uint8_t* gb) {
  if (!gb) return false;
  constexpr uint32_t kBar = 0x82757AB0u, kSlot = kBar + 3 * 76;
  auto be32 = [gb](uint32_t a) {
    return uint32_t(gb[a]) << 24 | uint32_t(gb[a + 1]) << 16 | uint32_t(gb[a + 2]) << 8 | gb[a + 3];
  };
  return gb[kBar + 1] != 0 && be32(kSlot + 20) == 128 && gb[kSlot + 24] == 0 && gb[kSlot + 25] == 'J';
}

class GateDriver final : public rex::input::InputDriver {
 public:
  // auto_keyboard: this gate holds the real pads; keyboard/mouse (mnk_mode)
  // turns off while one is connected (USB or Bluetooth) and back on without.
  explicit GateDriver(std::unique_ptr<rex::input::InputDriver> inner, bool auto_keyboard = false,
                      bool keyboard = false)
      : InputDriver(nullptr, 0), inner_(std::move(inner)), auto_keyboard_(auto_keyboard), keyboard_(keyboard) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    size_t before = out.size();
    inner_->EnumerateDevices(out);
    if (auto_keyboard_) {
      int has_pad = out.size() > before ? 1 : 0;
#if defined(_WIN32)
      touch::state().physical_pad = has_pad == 1;  // the PC's on-screen buttons hide with a pad (touch_controls.h)
#endif
      if (has_pad != had_pad_) {
        had_pad_ = has_pad;
        // 2026-11-17: a device that comes and goes by itself every few seconds (the user's PC: a pad asleep, a
        // headset?) turned the keyboard off while it was "there" (WASD dead). Now a pad only turns the keyboard and
        // mouse off once it is used (GetDeviceState); gone, they are back at once.
        std::string names;
        for (size_t i = before; i < out.size(); ++i) names += (names.empty() ? "" : ", ") + out[i].name;
        if (has_pad) {
          pad_used_ = false;
          REXLOG_INFO("Input: controle conectado ({}); teclado e mouse ficam ligados ate o controle ser usado", names);
        } else {
          rex::cvar::SetFlagByName("mnk_mode", "true");
          REXLOG_INFO("Input: sem controle -> teclado e mouse ligados");
        }
      }
    }
  }
  void OnWindowAvailable(rex::ui::Window* window) override { inner_->OnWindowAvailable(window); }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    auto& state = pc_options_state();
    MenuDebugTick();  // TEMP (MENUDBG)
    // Mouse look only in gameplay with the panel closed: menus get the cursor,
    // as in a native PC game. Before the inner poll, which applies it.
    const int menu_reason = state.MenuReason();
    bool in_menu = menu_reason != 0;
    {  // TEMP (MENUFLAG): NIER_MENUFLAGDBG logs the menu flag candidates when they change.
      static const bool dbg = std::getenv("NIER_MENUFLAGDBG") != nullptr;
      const uint8_t* gb = state.guest_base.load();
      if (dbg && gb) {
        auto be = [gb](uint32_t a) {
          const uint8_t* p = gb + a;
          return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        };
        static uint32_t last[4] = {~0u, ~0u, ~0u, ~0u};
        (void)be;
        // e643 (a fade) left out of the change test; eb0d8 and e2b54 (the gameplay marks) in.
        const uint32_t now4[4] = {gb[0x82757AB1], gb[0x827587DD], gb[0x827EB0D8], gb[0x827E2B54]};
        if (std::memcmp(now4, last, sizeof(last)) != 0) {
          std::memcpy(last, now4, sizeof(last));
          REXLOG_INFO("MENUFLAG 7ab1={} 87dd={} eb0d8={} e2b54={} menu={}", now4[0], now4[1], now4[2], now4[3],
                      state.GameInMenu());
        }
      }
    }
    static std::atomic<int> logged_menu{-1};
    if (logged_menu.exchange(menu_reason) != menu_reason) {
      static constexpr const char* kWhy[7] = {"", "menu do jogo", "listas do titulo", "janela de escolha",
                                              "lista de saves", "sem gameplay desde o titulo", "jogo carregando"};
      if (in_menu)
        REXLOG_INFO("Jogo: menu (cursor livre) - {}", kWhy[std::clamp(menu_reason, 0, 6)]);
      else
        REXLOG_INFO("Jogo: gameplay (mouse na cÃ¢mera)");
    }
    bool look = !state.open && !state.mod_panel_open && !state.jukebox_open && !state.tutorial_open && !in_menu;
    if (look != rex::input::mnk::IsMouseLookActive()) {
      rex::input::mnk::SetMouseLookActive(look);
    }
    // In menus the mouse clicks the menu: left = A (and Start on the screens
    // before the title menu, which only take Start), right = B, wheel = up/down.
    rex::input::mnk::SetMenuMouse(in_menu, state.BeforeTitleMenu());
    X_RESULT r = inner_->GetDeviceState(id, out);
    if (r != X_ERROR_SUCCESS || !out) return r;
    {  // The device last used (the buttons' icons follow it: game_text_hooks.cpp ButtonIconsTick).
      const auto& g = out->gamepad;
      auto tilted = [](int16_t v) { return v > 12000 || v < -12000; };
      const bool used = g.buttons != 0 || g.left_trigger > 64 || g.right_trigger > 64 || tilted(int16_t(g.thumb_lx)) ||
                        tilted(int16_t(g.thumb_ly)) || tilted(int16_t(g.thumb_rx)) || tilted(int16_t(g.thumb_ry));
      // A finger's presses come through the pad's gate too: touch while a finger was down lately.
      const bool touching = nier::menu_mouse::NowMs() - state.touch_ms.load() < 500;
      if (touching) state.input_kind = 2;
      else if (used) state.input_kind = keyboard_ ? 1 : 0;
      // A real pad used: the keyboard and mouse go off now (EnumerateDevices).
      if (auto_keyboard_ && had_pad_ == 1 && !pad_used_ && used && !touching) {
        pad_used_ = true;
        rex::cvar::SetFlagByName("mnk_mode", "false");
        REXLOG_INFO("Input: controle usado -> teclado e mouse desligados");
      }
    }
#if defined(_WIN32)
    // A PC's touch screen moves and clicks the mouse too (Windows does): while a finger is on it the mouse's camera
    // and its clicks' buttons (the binds on LMB / RMB / MMB) are not the game's; the on-screen buttons are.
    if (keyboard_ && !in_menu && nier::menu_mouse::NowMs() - state.touch_ms.load() < 400) {
      out->gamepad.thumb_rx = 0;
      out->gamepad.thumb_ry = 0;
      static const std::pair<const char*, uint16_t> kMouseBinds[] = {
          {"keybind_x", 0x4000}, {"keybind_y", 0x8000}, {"keybind_a", 0x1000}, {"keybind_b", 0x2000},
          {"keybind_rstick_press", 0x0080}, {"keybind_right_trigger", 0}, {"keybind_left_trigger", 0}};
      for (auto [cvar, bit] : kMouseBinds)
        if (bit && rex::cvar::GetFlagByName(cvar).find("MB") != std::string::npos) out->gamepad.buttons = uint16_t(out->gamepad.buttons & ~bit);
    }
#endif
    if (keyboard_ && in_menu) KeyboardInMenu(out->gamepad);
    // The mouse points at the rows of the game's list menus (menu_mouse.h).
    // The pointer's gate: the keyboard and mouse's on the PC; on Android the pad's, where the touches come in
    // (touch_controls.h: the finger is the pointer in menus).
#if defined(__ANDROID__)
    const bool pointer_gate = auto_keyboard_;
#else
    const bool pointer_gate = keyboard_;
#endif
    // Not under the Jukebox page: the pointer moving over it pressed up / down toward the pause's rows, and the
    // page took them as its list's (it ran down as the mouse moved, 2026-11-17).
    if (pointer_gate && in_menu && !state.open && !state.jukebox_open && !state.BeforeTitleMenu())
      menu_mouse_.Apply(out->gamepad, state.guest_base.load(), state.BookOpen());
    // The quick weapon switch (weapon_switch.h): the weapon buttons' taps and holds, in gameplay only.
    weapon_switch_.Apply(out->gamepad, state.guest_base.load(),
                         weapon_switch::InPlay(state.guest_base.load()) && !nier::menu_mouse::MenuSeenRecently() && !state.open &&
                             !state.jukebox_open && !state.mod_panel_open && !state.tutorial_open);
    // The title's cards (title_carousel.cpp): a chosen card is the game's A.
    if (nier::title_carousel::WantA()) out->gamepad.buttons = uint16_t(out->gamepad.buttons | 0x1000);
    uint16_t now = Buttons(out->gamepad);

    std::lock_guard lock(mutex_);
    {  // TEMP (PADDBG): NIER_PADDBG logs every change of the buttons the game gets.
      static const bool dbg = std::getenv("NIER_PADDBG") != nullptr;
      static uint16_t logged = 0;
      if (dbg && now != logged) {
        logged = now;
        const auto& g = out->gamepad;
        REXLOG_INFO("PADDBG {:04X} raw={:04X} lx={} ly={} rx={} ry={} lt={} rt={}", now, uint16_t(g.buttons),
                    int(int16_t(g.thumb_lx)), int(int16_t(g.thumb_ly)), int(int16_t(g.thumb_rx)),
                    int(int16_t(g.thumb_ry)), int(g.left_trigger), int(g.right_trigger));
      }
    }
    uint16_t pressed = now & ~last_;
    last_ = now;
    const uint16_t combo = kPadBack | kPadStart;
    // Quit to Desktop in the pause menu: becomes the game's Quit Game, whose dialog asks first.
    if (!state.open) state.ArmQuitDesktop(pressed);
    // "Video Options" / "Control Options" row of the game's own Options menu
    // is selected: A opens our panel on that page instead of reaching the game.
    const int on_game_item = state.GameItemSelected();
    // Grimoire Weiss's book first opens on Maps and then on the page it was closed on; RB / LB turn
    // its 6 pages round (Notes -> Maps), so the gate counts them (the headings of the middle pages
    // correct it, video_options_menu.cpp). Our Achievements page opens from Notes' sixth row
    // (memo_achievements.cpp).
    {
      const bool book_open = state.BookOpen();
      if (state.book_tab < 0) state.book_tab = 0;
      if (book_open && !state.open && !state.ach_open) {
        if (pressed & kPadRB) {
          state.book_tab = (state.book_tab + 1) % 6;
        } else if (pressed & kPadLB) {
          state.book_tab = (state.book_tab + 5) % 6;
        }
      }
    }
    // B with a page of ours showing: the game backs out (it still gets B) and the page closes.
    // The pause menu's Options object keeps its state after closing, so the state alone can't tell.
    // In the pause menu's Options the game's B would leave Options for the pause menu, so there B is
    // ours alone and goes back to the Options list (as Return does).
    if (!state.open && state.native_page && (pressed & kPadB)) {
      state.native_events |= PcOptionsState::kEventB;
      // In the pause's Options, and on the Control Options' own pages (their B is back to that list), the B is ours.
      const int page = state.native_page.load();
      if (state.native_page_pause || page == 7 || page == 8 || page == 9) muted_ = true;
    }
    if (!state.open && pressed) state.NotePausePress(pressed);
    if (pressed & kPadStart) state.start_press_ms = nier::menu_mouse::NowMs();
    // Start on the title's Options goes back to the title screen with no call our hooks see, and
    // the stale "on screen" then took A on the title menu (Continue) for a row of ours: the page
    // opened over a menu already gone and the game crashed reading 0x834 (2026-11-12). On a page
    // of ours Start (Esc on the keyboard) is B: back to the Options list; on that list it ends
    // "on screen".
    if (!state.open && (pressed & kPadStart) && !(now & kPadBack) && !state.native_page_pause &&
        !state.PauseListShowing()) {
      if (state.native_page) {
        state.native_events |= PcOptionsState::kEventB;
        start_as_b_ = true;
      } else {
        state.game_options_on_screen = false;
      }
    }
    static const bool dev_panel = rex::cvar::GetFlagByName("nier_dev_panel") == "true";  // off in the public build
    if (dev_panel && (now & combo) == combo && (pressed & combo)) {
      state.open_page = 1;
      state.toggle_requested = true;
      muted_ = true;
    } else if (!state.open && state.native_page &&
               (pressed & (kPadA | kPadLeft | kPadRight))) {
      // A page of ours is in the game's settings list: the game scrolls it and
      // handles B/Return, but A and left/right are ours (it would change values
      // by itself).
      uint32_t events = 0;
      if (pressed & kPadA) events |= PcOptionsState::kEventA;
      if (pressed & kPadLeft) events |= PcOptionsState::kEventLeft;
      if (pressed & kPadRight) events |= PcOptionsState::kEventRight;
      state.native_events |= events;
      muted_ = true;
    } else if (!state.open && on_game_item && (pressed & kPadA)) {
      state.native_open_request = on_game_item;
      muted_ = true;
    } else if (!state.open && !state.jukebox_open && (pressed & kPadRB) && JukeboxHintShown(state.guest_base.load()) &&
               nier::menu_mouse::tracker().CrossOnTop(nier::menu_mouse::NowMs())) {
      // RB on the pause's cross, whose key hints say "RB Jukebox" (video_options_menu.cpp, sub_823F1690): the page.
      state.jukebox_open = true;
      state.TakeJukebox();
      muted_ = true;
    } else if (!state.open && state.tutorial_open) {
      // A tutorial of ours (tutorials.h) over the game: its presses close it, none reaches the game.
      if (pressed) state.tutorial_presses |= pressed;
      muted_ = true;
    } else if (!state.open && state.jukebox_open) {
      // The Jukebox page is over the game (jukebox/jukebox_ui.h): its presses, none reaches the game.
      if (pressed) state.PushJukebox(pressed);
      muted_ = true;
    } else if (!state.open && state.ach_open) {
      // Our Achievements page is over the book: every press is its, none reaches the game.
      if (pressed) state.PushAch(pressed);
      muted_ = true;
    } else if (state.open && pressed) {
      state.Push(pressed);
    } else if (pressed & kPadB) {
      // B backs out of the Options screen without a transition call; left
      // on, the stale "on screen" made A on the title menu open the panel.
      state.game_options_on_screen = false;
    }
    {  // Nier Plus: the pad as the game would read it, for the mods (get_pad).
      const auto& g = out->gamepad;
      state.pad_buttons = uint32_t(uint16_t(g.buttons)) | uint32_t(g.left_trigger) << 16 | uint32_t(g.right_trigger) << 24;
      state.pad_sticks = uint64_t(uint16_t(g.thumb_lx)) | uint64_t(uint16_t(g.thumb_ly)) << 16 |
                         uint64_t(uint16_t(g.thumb_rx)) << 32 | uint64_t(uint16_t(g.thumb_ry)) << 48;
    }
    {  // Nier Plus: the mods' on_pad (before the game acts on it).
      auto& g = out->gamepad;
      uint16_t b = uint16_t(g.buttons);
      uint8_t lt = g.left_trigger, rt = g.right_trigger;
      int16_t lx = int16_t(g.thumb_lx), ly = int16_t(g.thumb_ly), rx = int16_t(g.thumb_rx), ry = int16_t(g.thumb_ry);
      nier::mods::OnPad(b, lt, rt, lx, ly, rx, ry);
      g.buttons = b;
      g.left_trigger = lt;
      g.right_trigger = rt;
      g.thumb_lx = lx;
      g.thumb_ly = ly;
      g.thumb_rx = rx;
      g.thumb_ry = ry;
      now = Buttons(g);
    }
    if (state.open || state.toggle_requested || state.native_capturing || state.mod_panel_open || state.jukebox_open ||
        state.tutorial_open || state.mod_input_blocked)
      muted_ = true;
    if (muted_ && !state.open && !state.mod_panel_open && !state.jukebox_open && !state.tutorial_open && !state.mod_input_blocked && !state.toggle_requested &&
        now == 0)
      muted_ = false;
    if (muted_) {
      uint32_t packet = out->packet_number;
      std::memset(out, 0, sizeof(*out));
      out->packet_number = packet;
    }
    // While that Start stays held the game gets B instead.
    if (start_as_b_) {
      if (now & kPadStart) {
        out->gamepad.buttons = uint16_t((out->gamepad.buttons & ~kPadStart) | kPadB);
      } else {
        start_as_b_ = false;
      }
    }
    return r;
  }
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 rex::input::X_INPUT_CAPABILITIES* out) override {
    return inner_->GetDeviceCapabilities(id, flags, out);
  }
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION* v) override {
    return inner_->SetDeviceVibration(id, v);
  }
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              rex::input::X_INPUT_KEYSTROKE* out) override {
    if (pc_options_state().open || pc_options_state().mod_panel_open || pc_options_state().jukebox_open ||
        pc_options_state().tutorial_open)
      return X_ERROR_EMPTY;
    return inner_->GetDeviceKeystroke(id, flags, out);
  }

 private:
  // In the game's menus the keys mean what a PC game's do: Esc goes back (B; the Start it is in
  // gameplay confirms in menus), the arrow keys move the cursor (the d-pad; in gameplay they turn
  // the camera) and WASD does nothing. Before the title menu, whose screens take only Start, Esc
  // stays Start.
  static void KeyboardInMenu(rex::input::X_INPUT_GAMEPAD& g) {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (!keys || pc_options_state().BeforeTitleMenu()) return;
    uint16_t b = g.buttons;
    if (keys[SDL_SCANCODE_ESCAPE] && !keys[SDL_SCANCODE_P]) {
      b = uint16_t(b & ~kPadStart);
      b |= kPadB;
    }
    if (keys[SDL_SCANCODE_BACKSPACE]) b |= kPadB;
    const int rx = int16_t(g.thumb_rx), ry = int16_t(g.thumb_ry);
    constexpr int kDead = 16000;
    if (ry > kDead) b |= kPadUp;
    if (ry < -kDead) b |= kPadDown;
    if (rx < -kDead) b |= kPadLeft;
    if (rx > kDead) b |= kPadRight;
    g.thumb_lx = 0;
    g.thumb_ly = 0;
    g.thumb_rx = 0;
    g.thumb_ry = 0;
    g.buttons = b;
  }

  // D-pad plus both sticks as a d-pad, for menu navigation (the keyboard's
  // arrow keys are the right stick).
  static uint16_t Buttons(const rex::input::X_INPUT_GAMEPAD& g) {
    uint16_t b = g.buttons;
    constexpr int16_t kDead = 16000;
    for (auto [x, y] : {std::pair{g.thumb_lx, g.thumb_ly}, std::pair{g.thumb_rx, g.thumb_ry}}) {
      if (y > kDead) b |= kPadUp;
      if (y < -kDead) b |= kPadDown;
      if (x < -kDead) b |= kPadLeft;
      if (x > kDead) b |= kPadRight;
    }
    return b;
  }

  std::unique_ptr<rex::input::InputDriver> inner_;
  std::mutex mutex_;
  uint16_t last_ = 0;
  bool muted_ = false;
  bool start_as_b_ = false;  // Start pressed on a page of ours: the game gets B while it is held
  menu_mouse::MenuMouse menu_mouse_;
  weapon_switch::Filter weapon_switch_;
  bool auto_keyboard_;
  bool keyboard_;  // the keyboard and mouse's gate (its keys change meaning in menus)
  int had_pad_ = -1;
  bool pad_used_ = false;  // the connected pad was used (the keyboard and mouse are off)
};

// NIER_INPUT_LIVE=<file>: remote control for tests without anyone at the
// device (adb on the Android handheld). The file holds "buttons_hex [lx ly rx
// ry lt rt]", re-read every 50 ms; its buttons are added to the real pad's and
// its non-zero sticks/triggers replace the real ones. The pad keeps working.
class LiveDriver final : public rex::input::InputDriver {
 public:
  LiveDriver(std::unique_ptr<rex::input::InputDriver> inner, const std::string& path)
      : InputDriver(nullptr, 0), inner_(std::move(inner)), path_(path) {
    REXLOG_INFO("Input: live control from {}", path);
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    const size_t before = out.size();
    inner_->EnumerateDevices(out);
    // No real pad (a test on the PC): the file's buttons still need a device to arrive through.
    if (out.size() == before) {
      rex::input::DeviceInfo info;
      info.id = kDevice;
      info.name = "Input live";
      out.push_back(info);
    }
  }
  void OnWindowAvailable(rex::ui::Window* window) override { inner_->OnWindowAvailable(window); }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    X_RESULT r = X_ERROR_SUCCESS;
    if (id == kDevice) {
      if (out) std::memset(out, 0, sizeof(*out));
    } else {
      r = inner_->GetDeviceState(id, out);
    }
    if (r != X_ERROR_SUCCESS || !out) return r;
    std::lock_guard lock(mutex_);
    uint64_t now = clock_.ms();
    if (now - last_read_ms_ >= 50) {
      last_read_ms_ = now;
      if (std::FILE* f = std::fopen(path_.c_str(), "r")) {
        unsigned b = 0;
        int v[6] = {};
        int n = std::fscanf(f, "%X %d %d %d %d %d %d", &b, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        std::fclose(f);
        if (n >= 1) {
          live_ = {uint16_t(b), uint8_t(v[4]), uint8_t(v[5]), int16_t(v[0]), int16_t(v[1]),
                   int16_t(v[2]), int16_t(v[3])};
        }
      }
    }
    auto& g = out->gamepad;
    g.buttons = uint16_t(uint16_t(g.buttons) | live_.buttons);
    if (live_.lx || live_.ly) { g.thumb_lx = live_.lx; g.thumb_ly = live_.ly; }
    if (live_.rx || live_.ry) { g.thumb_rx = live_.rx; g.thumb_ry = live_.ry; }
    if (live_.lt) g.left_trigger = live_.lt;
    if (live_.rt) g.right_trigger = live_.rt;
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
  static constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x4C495645);  // "LIVE"
  static constexpr auto kNopId = static_cast<rex::input::DeviceId>(0x4E4F5000);   // NopInputDriver's
  std::unique_ptr<rex::input::InputDriver> inner_;
  std::string path_;
  std::mutex mutex_;
  uint64_t last_read_ms_ = 0;
  PadSample live_;
  InputClock clock_;
  rex::input::nop::NopInputDriver nop_{nullptr, 0};
};

// A single synthetic pad that plays a recording back.
class ReplayDriver final : public rex::input::InputDriver {
 public:
  explicit ReplayDriver(const std::string& path) : InputDriver(nullptr, 0) {
    Load(path);
    std::string trace = path + ".replay.csv";
    trace_ = std::fopen(trace.c_str(), "w");
    if (trace_) std::fputs("# applied event: kind,poll,recorded_ms,replay_ms\n", trace_);
  }
  ~ReplayDriver() override {
    if (trace_) std::fclose(trace_);
  }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "Input replay";
    out.push_back(info);
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    // Never run ahead of the recording: a faster setup (lower resolution,
    // loading screens at hundreds of fps) would otherwise press buttons
    // before the game is ready for them, as loading takes wall time.
    if (anchor_poll_ && polls_ + 1 == anchor_poll_) {
      // Realign the recording's clock here (start of gameplay), so a loading
      // that took a different time doesn't leave the rest ahead or behind.
      anchor_offset_ms_ = int64_t(clock_.ms()) - int64_t(RecordedMs(anchor_poll_));
      REXLOG_INFO("Input: replay anchored at poll {} (offset {} ms)", anchor_poll_,
                  anchor_offset_ms_);
    }
    if (paced_ && polls_ < paced_until_poll_) {
      uint64_t recorded_ms = uint64_t(std::max<int64_t>(
          0, int64_t(RecordedMs(polls_ + 1)) +
                 (anchor_poll_ && polls_ + 1 >= anchor_poll_ ? anchor_offset_ms_ : 0)));
      uint64_t now_ms = clock_.ms();
      if (recorded_ms > now_ms) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min<uint64_t>(recorded_ms - now_ms, 250)));
      }
    }
    std::lock_guard lock(mutex_);
    ++polls_;
    if (trace_ && polls_ % 300 == 0) {
      std::fprintf(trace_, "H,%llu,0,%llu\n", (unsigned long long)polls_, (unsigned long long)clock_.ms());
    }
    while (next_state_ < states_.size() && states_[next_state_].poll <= polls_) {
      current_ = states_[next_state_].pad;
      Trace('S', states_[next_state_]);
      ++next_state_;
    }
    if (!finished_ && next_state_ == states_.size() && keys_.empty() && polls_ >= end_poll_) {
      finished_ = true;
      REXLOG_INFO("Input: replay finished at poll {} ({} ms)", polls_, clock_.ms());
    }
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->packet_number = static_cast<uint32_t>(polls_);
      current_.To(out->gamepad);
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 rex::input::X_INPUT_CAPABILITIES* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    return nop_.GetDeviceCapabilities(kNopId, flags, out);
  }
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                              rex::input::X_INPUT_KEYSTROKE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lock(mutex_);
    if (keys_.empty() || keys_.front().poll > polls_) return X_ERROR_EMPTY;
    const Key& k = keys_.front();
    if (out) {
      std::memset(out, 0, sizeof(*out));
      out->virtual_key = k.vk;
      out->unicode = k.unicode;
      out->flags = k.flags;
      out->hid_code = k.hid;
    }
    trace_key(k);
    keys_.pop_front();
    return X_ERROR_SUCCESS;
  }

 private:
  static constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x52504C59);  // "RPLY"
  static constexpr auto kNopId = static_cast<rex::input::DeviceId>(0x4E4F5000);   // NopInputDriver's
  struct State { uint64_t poll, ms; PadSample pad; };
  struct Key { uint64_t poll, ms; uint16_t vk, unicode, flags; uint8_t hid; };

  void Load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
      REXLOG_ERROR("Input: cannot open replay {}", path);
      return;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
      unsigned long long poll = 0, ms = 0;
      if (line[0] == 'S') {
        unsigned b, lt, rt;
        int lx, ly, rx, ry;
        if (std::sscanf(line, "S,%llu,%llu,%X,%u,%u,%d,%d,%d,%d", &poll, &ms, &b, &lt, &rt, &lx, &ly,
                        &rx, &ry) == 9) {
          states_.push_back({poll, ms,
                             {uint16_t(b), uint8_t(lt), uint8_t(rt), int16_t(lx), int16_t(ly),
                              int16_t(rx), int16_t(ry)}});
          times_.emplace_back(poll, ms);
        }
      } else if (line[0] == 'H') {
        if (std::sscanf(line, "H,%llu,%llu", &poll, &ms) == 2) {
          times_.emplace_back(poll, ms);
        }
      } else if (line[0] == 'K') {
        unsigned vk, uc, fl, hid;
        if (std::sscanf(line, "K,%llu,%llu,%u,%u,%u,%u", &poll, &ms, &vk, &uc, &fl, &hid) == 6) {
          keys_.push_back({poll, ms, uint16_t(vk), uint16_t(uc), uint16_t(fl), uint8_t(hid)});
        }
      } else if (line[0] == 'E') {
        std::sscanf(line, "E,%llu", &end_poll_);
      }
    }
    std::fclose(f);
    std::sort(times_.begin(), times_.end());
    // NIER_REPLAY_PACED=0: follow only the game's polls, as fast as it runs.
    // NIER_REPLAY_PACED=<poll>: pace only up to that poll (through the loading
    // screens), then run freely - pacing caps the frame rate at the
    // recording's, so measure faster setups this way.
    const char* paced = std::getenv("NIER_REPLAY_PACED");
    paced_ = !(paced && paced[0] == '0') && !times_.empty();
    if (paced && std::atoll(paced) > 0) paced_until_poll_ = uint64_t(std::atoll(paced));
    // NIER_REPLAY_ANCHOR=<poll>: from that poll on, pace against the
    // recording's clock counted from that poll (see GetDeviceState).
    if (const char* anchor = std::getenv("NIER_REPLAY_ANCHOR")) {
      anchor_poll_ = uint64_t(std::atoll(anchor));
    }
    REXLOG_INFO("Input: replaying {} ({} state changes, {} keystrokes, ends at poll {}, paced {} "
                "until poll {})",
                path, states_.size(), keys_.size(), end_poll_, paced_, paced_until_poll_);
  }
  // Wall time of a poll in the recording, interpolated between the logged
  // points (state changes and heartbeats).
  uint64_t RecordedMs(uint64_t poll) const {
    auto it = std::lower_bound(times_.begin(), times_.end(), std::make_pair(poll, uint64_t(0)));
    if (it == times_.end()) return times_.back().second;
    if (it == times_.begin() || it->first == poll) return it->second;
    auto prev = std::prev(it);
    return prev->second + (it->second - prev->second) * (poll - prev->first) /
                              (it->first - prev->first);
  }
  void Trace(char kind, const State& s) {
    if (trace_) {
      std::fprintf(trace_, "%c,%llu,%llu,%llu\n", kind, (unsigned long long)s.poll,
                   (unsigned long long)s.ms, (unsigned long long)clock_.ms());
      std::fflush(trace_);
    }
  }
  void trace_key(const Key& k) {
    if (trace_) {
      std::fprintf(trace_, "K,%llu,%llu,%llu\n", (unsigned long long)k.poll,
                   (unsigned long long)k.ms, (unsigned long long)clock_.ms());
      std::fflush(trace_);
    }
  }

  rex::input::nop::NopInputDriver nop_{nullptr, 0};
  std::vector<State> states_;
  std::vector<std::pair<uint64_t, uint64_t>> times_;  // poll, recorded ms
  bool paced_ = false;
  uint64_t paced_until_poll_ = UINT64_MAX;
  uint64_t anchor_poll_ = 0;
  int64_t anchor_offset_ms_ = 0;
  std::deque<Key> keys_;
  unsigned long long end_poll_ = 0;
  size_t next_state_ = 0;
  uint64_t polls_ = 0;
  PadSample current_;
  bool finished_ = false;
  std::mutex mutex_;
  std::FILE* trace_ = nullptr;
  InputClock clock_;
};

// Input factory. Normal play: SDL pad behind the options-panel gate, plus
// keyboard/mouse. NIER_INPUT_RECORD additionally logs what the game reads (the
// gate sits inside, so recordings never contain panel navigation);
// NIER_INPUT_REPLAY replaces all real input with the recording.
inline std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
  if (tool_mode) {
    return rex::input::CreateDefaultInputSystem(tool_mode);
  }
  const char* record = std::getenv("NIER_INPUT_RECORD");
  const char* replay = std::getenv("NIER_INPUT_REPLAY");

  auto input = std::make_unique<rex::input::InputSystem>(nullptr);
  if (replay && *replay) {
    // A recording is a physical pad's state: through the controller's mapping too (pad_remap.h).
    std::unique_ptr<rex::input::InputDriver> pad =
        std::make_unique<pad_remap::RemapDriver>(std::make_unique<ReplayDriver>(replay));
    // NIER_REPLAY_GATED: treat the recording as raw pad input, so the options
    // panel's own controls (A on Video Options, B to close) can be replayed.
    if (const char* gated = std::getenv("NIER_REPLAY_GATED"); gated && *gated) {
      pad = std::make_unique<GateDriver>(std::move(pad));
    }
    input->AddDriver(std::move(pad));
  } else {
    auto sdl = std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 0);
    if (sdl->Setup() == X_STATUS_SUCCESS) {
      // The controller's buttons as the player mapped them (pad_remap.h): the physical pads only.
      std::unique_ptr<rex::input::InputDriver> pad = std::make_unique<pad_remap::RemapDriver>(std::move(sdl));
      // Live control inside the gate: it drives the game's menus (A on our rows) as a real pad does.
      if (const char* live = std::getenv("NIER_INPUT_LIVE"); live && *live) {
        pad = std::make_unique<LiveDriver>(std::move(pad), live);
      }
#if !defined(_WIN32)
      // The on-screen buttons (touch_controls.h): inside the gate, so they also drive our pages and panel.
      pad = std::make_unique<touch::TouchDriver>(std::move(pad));
#endif
      pad = std::make_unique<GateDriver>(std::move(pad), /*auto_keyboard=*/true);
      if (record && *record) {
        pad = std::make_unique<RecordingDriver>(std::move(pad), record);
      }
      input->AddDriver(std::move(pad));
    } else {
      REXLOG_ERROR("Input: SDL driver setup failed");
    }
    // Keyboard/mouse also behind the gate: its pad buttons drive the panel
    // (W/S rows, A/D values, Space/F) and never reach the game while it is open.
    auto mnk = std::make_unique<rex::input::mnk::MnkInputDriver>(nullptr, 0);
    if (mnk->Setup() == X_STATUS_SUCCESS) {
#if defined(_WIN32)
      // A PC's touch screen (touch_controls.h): its buttons ride on the keyboard and mouse's device. Its own device, as
      // on the phone, took the game's first slot whenever no pad was connected and the keyboard went unread
      // (2026-11-17: keyboard and mouse dead).
      std::unique_ptr<rex::input::InputDriver> keys = std::make_unique<touch::TouchDriver>(std::move(mnk));
      input->AddDriver(std::make_unique<GateDriver>(std::move(keys), false, /*keyboard=*/true));
#else
      input->AddDriver(std::make_unique<GateDriver>(std::move(mnk), false, /*keyboard=*/true));
#endif
    }
  }
  input->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 1));
  input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
  return input;
}

}  // namespace nier
