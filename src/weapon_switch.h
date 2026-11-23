// nier - quick weapon switch (2026-11-17, the user's request, as in the remake's "Weapon Switch: Tap / Hold").
//
// The weapon buttons (D-pad right one-handed sword, left two-handed, down spear; the keyboard's 2 / 1 / 3; the
// on-screen weapon popup). Option nier_weapon_switch (Options > Control Options):
//   tap  - a tap equips at once the weapon chosen for that type (the player's +140: one per type), holding opens
//          the menu;
//   hold - holding equips at once, a tap opens the menu.
// Only in gameplay, and only once Nier has a weapon of every type (before, the buttons are the game's own).
// The input threads decide (Filter, in each gate: the pads' and the keyboard's); the game thread equips (EquipTick,
// from the present hook) as the weapon menu's A does. As the game's own sides: down one-handed, left two-handed, right
// spear; the menu opened is the game's own for the side held (2026-11-20: before every kind, down was its one list).
#pragma once

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>

#include "menu_mouse.h"

namespace nier::weapon_switch {

constexpr uint32_t kPlayer = 0x84CC6BA0u, kPlayerWeapons = 140;  // one-handed, two-handed, spear (-1: none)
constexpr uint16_t kRight = 0x0008, kLeft = 0x0004, kDown = 0x0002, kAll = kRight | kLeft | kDown;
constexpr uint64_t kHoldMs = 300;

// The kind to equip (0 one-handed, 1 two-handed, 2 spear; -1 none): the game thread equips it (EquipTick).
inline std::atomic<int>& Request() {
  static std::atomic<int> r{-1};
  return r;
}

inline uint32_t Be32(const uint8_t* gb, uint32_t a) {
  return uint32_t(gb[a]) << 24 | uint32_t(gb[a + 1]) << 16 | uint32_t(gb[a + 2]) << 8 | gb[a + 3];
}
// Nier has a weapon of every type.
inline bool Unlocked(const uint8_t* gb) {
  if (!gb) return false;
  static const bool test = std::getenv("NIER_WEAPON_SWITCH_TEST") != nullptr;  // tests: a save without every kind
  if (test) return true;
  for (uint32_t k = 0; k < 3; ++k)
    if (Be32(gb, kPlayer + kPlayerWeapons + 4 * k) == 0xFFFFFFFFu) return false;
  return true;
}
// Plain gameplay: no list menu (0x82757AB1), Nier's object there (its vtable at +3472) and his controls free
// (0x827CD450: 0 in talks and cutscenes). The game's own gameplay marks stay off in some areas (Weiss's cave).
inline bool InPlay(const uint8_t* gb) {
  return gb && gb[0x82757AB1] == 0 && gb[0x827CD450] != 0 && Be32(gb, 0x848BE4B0u + 3472) == 0x82740048u;
}
inline bool HoldMode() { return rex::cvar::GetFlagByName("nier_weapon_switch") == "hold"; }
// The game's own sides (2026-11-20, the player after the timeskip): down one-handed, left two-handed, right spear.
inline int TypeOf(uint16_t bit) { return bit == kDown ? 0 : bit == kLeft ? 1 : 2; }

class Filter {
 public:
  // g: what the game is about to get; gameplay: no menu of any kind is up.
  void Apply(rex::input::X_INPUT_GAMEPAD& g, const uint8_t* gb, bool gameplay) {
    uint16_t b = uint16_t(g.buttons);
    const uint16_t d = uint16_t(b & kAll);
    if (!held_ && d && gameplay && inject_polls_ == 0 && Unlocked(gb)) {
      held_ = (d & kRight) ? kRight : (d & kLeft) ? kLeft : kDown;
      down_ms_ = nier::menu_mouse::NowMs();
      decided_ = false;
    }
    if (held_) {
      const bool down = (b & held_) != 0;
      const uint64_t now = nier::menu_mouse::NowMs();
      const bool hold_mode = HoldMode();
      if (down && !decided_ && now - down_ms_ >= kHoldMs) {
        decided_ = true;
        if (hold_mode) Quick(held_);  // held: the quick switch
        else Menu(held_);              // held: the game's weapon menu of that side
      }
      if (!down) {
        if (!decided_) {  // a tap
          if (hold_mode) Menu(held_);
          else Quick(held_);
        }
        held_ = 0;
      } else {
        b = uint16_t(b & ~held_);  // the press is ours (the menu gets its own)
      }
    }
    if (inject_polls_ > 0) {  // a short press given to the game (its menu opens)
      --inject_polls_;
      b |= inject_;
    }
    g.buttons = b;
  }
 private:
  void Quick(uint16_t bit) { Request() = TypeOf(bit); }
  // The game's weapon menu: a short press of the same side given to it. 2026-11-20 (the player, after the timeskip):
  // with every kind each side opens its own list; this gave D-pad down always, the one-handed swords' list.
  void Menu(uint16_t bit) {
    inject_ = bit;
    inject_polls_ = 6;
  }

  uint16_t held_ = 0, inject_ = 0;
  int inject_polls_ = 0;
  uint64_t down_ms_ = 0;
  bool decided_ = false;
};

}  // namespace nier::weapon_switch
