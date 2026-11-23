// nier - keyboard/mouse binds: NieR Replicant ver.1.22's PC defaults mapped
// onto the Xbox 360 buttons NieR Gestalt reads (the SDK's keybind_* cvars).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nier {

struct KeyBindRow {
  const char* label;  // as on Replicant's "Custom (Keyboard)" screen
  const char* cvar;
  // Comma-separated: the first key is the one the Controls screen edits, the
  // rest are fixed extras the menus rely on (Enter confirms, Esc backs out).
  const char* default_keys;
};

// Replicant -> 360: jump A, action B, attack X, heavy attack Y, evade RT,
// defend LT, magic LB/RB, menu Start, map Back, items D-pad up, weapons D-pad
// left/right/down. Gestalt has no lock-on: End recenters the camera (R3).
inline const std::vector<KeyBindRow>& KeyBindRows() {
  static const std::vector<KeyBindRow> rows = {
      {"Mover: frente", "keybind_lstick_up", "W"},
      {"Mover: trás", "keybind_lstick_down", "S"},
      {"Mover: esquerda", "keybind_lstick_left", "A"},
      {"Mover: direita", "keybind_lstick_right", "D"},
      {"Pular", "keybind_a", "Space,Return"},
      {"Ação", "keybind_b", "F,Backspace"},
      {"Ataque", "keybind_x", "LMB"},
      {"Ataque pesado", "keybind_y", "RMB"},
      {"Centralizar câmera", "keybind_rstick_press", "End,MMB"},
      {"Esquiva", "keybind_right_trigger", "Control"},
      {"Magia 1", "keybind_left_shoulder", "Q"},
      {"Defender", "keybind_left_trigger", "Shift"},
      {"Magia 2", "keybind_right_shoulder", "E"},
      {"Câmera: cima", "keybind_rstick_up", "Up"},
      {"Câmera: baixo", "keybind_rstick_down", "Down"},
      {"Câmera: esquerda", "keybind_rstick_left", "Left"},
      {"Câmera: direita", "keybind_rstick_right", "Right"},
      {"Andar", "keybind_walk_toggle", "Alt"},
      {"Menu principal", "keybind_start", "P,Escape"},
      {"Mapa grande", "keybind_back", "M"},
      {"Atalho de itens", "keybind_dpad_up", "Tab"},
      {"Espada de uma mão", "keybind_dpad_right", "2"},
      {"Espada de duas mãos", "keybind_dpad_left", "1"},
      {"Lança", "keybind_dpad_down", "3"},
  };
  return rows;
}

// The first key of a bind value, and the value with that key replaced.
inline std::string PrimaryKey(std::string_view value) {
  return std::string(value.substr(0, value.find(',')));
}
inline std::string WithPrimaryKey(std::string_view value, std::string_view key) {
  size_t comma = value.find(',');
  return std::string(key) + (comma == std::string_view::npos ? "" : std::string(value.substr(comma)));
}

// Key name (SDK keybind syntax) as shown on screen.
inline std::string KeyDisplayName(std::string_view key) {
  static const std::pair<std::string_view, const char*> kNames[] = {
      {"Space", "Espaço"},      {"Return", "Enter"},       {"Escape", "Esc"},
      {"Backspace", "Backspace"}, {"Control", "Ctrl"},     {"LMB", "Clique esq."},
      {"RMB", "Clique dir."},   {"MMB", "Botão do meio"},  {"Up", "Seta cima"},
      {"Down", "Seta baixo"},   {"Left", "Seta esq."},     {"Right", "Seta dir."},
      {"Semicolon", ";"},       {"Quote", "'"},            {"Comma", ","},
      {"Period", "."},          {"Slash", "/"},            {"Backslash", "\\"},
      {"LBracket", "["},        {"RBracket", "]"},         {"Minus", "-"},
      {"Plus", "="},            {"PageUp", "Page Up"},     {"PageDown", "Page Down"},
  };
  for (auto [name, shown] : kNames) {
    if (key == name) return shown;
  }
  if (key.empty()) return "-";
  return std::string(key);
}

}  // namespace nier
