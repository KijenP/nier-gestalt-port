// nier - Nier Plus code mods (mods/sdk/nier_mod.h): libraries in MODS/<Mod>/ loaded at startup, their panels
// (a key on the PC, an icon on Android) and the game events they hook.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rex::ui {
class ImGuiDrawer;
}

namespace nier::mods {

// Loads every code mod (once; after the game's image is in memory, before it runs).
void Load(uint8_t* guest_base);

// The UI side: the drawer for the panels and a way to run work on the UI thread. Hotkeys are registered once both
// this and Load have run.
void InitUi(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> run_on_ui_thread);

// The panels, for the on-screen icons (Android) and the game's menus.
struct PanelInfo {
  int index;
  std::string title;
  std::string icon_png;  // full path, empty = default
  int menus;             // NIER_MENU_* bits
};
std::vector<PanelInfo> Panels();

// The menus' "Mods" page (video_options_menu.cpp): one row per loaded mod, each opening its own page: on/off, its
// panels' keys (PC), then its settings.
struct ModInfo {
  int index;
  std::string folder, title;  // title: its first panel's
  bool enabled;
};
std::vector<ModInfo> Mods();
void SetModEnabled(int mod, bool enabled);  // off: its events, panels and icons do nothing (saved)

struct HotkeyInfo {
  int panel;
  std::string title, key;
};
std::vector<HotkeyInfo> Hotkeys(int mod);
void SetHotkey(int panel, const std::string& key);  // saved in the mod's settings.txt

// A mod's settings as rows of the game's settings list. act: 0 = A, -1 / +1 = left / right.
struct MenuRow {
  bool header = false;
  std::string label;
  std::function<std::string()> value;  // none: an action row
  std::function<void(int)> act;
};
std::vector<MenuRow> ModRows(int mod);
void TogglePanel(int index);
bool AnyPanelOpen();

// The controller the game is about to get (input_recorder.h): the mods' on_pad may change it.
void OnPad(uint16_t& buttons, uint8_t& left_trigger, uint8_t& right_trigger, int16_t& lx, int16_t& ly, int16_t& rx,
           int16_t& ry);

}  // namespace nier::mods
