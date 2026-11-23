// nier - reads the next key the player presses, for rebinding in the game's own
// Control Options page. Names are the SDK's keybind syntax (keyboard_binds.h).

#include "key_capture.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <SDL3/SDL_keyboard.h>
#endif

namespace nier {

#ifdef _WIN32
namespace {

bool g_down[256];

const char* NamedKey(int vk) {
  switch (vk) {
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Return";
    case VK_TAB: return "Tab";
    case VK_BACK: return "Backspace";
    case VK_DELETE: return "Delete";
    case VK_INSERT: return "Insert";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "Shift";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "Control";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return "Alt";
    case VK_OEM_1: return "Semicolon";
    case VK_OEM_7: return "Quote";
    case VK_OEM_COMMA: return "Comma";
    case VK_OEM_PERIOD: return "Period";
    case VK_OEM_2: return "Slash";
    case VK_OEM_5: return "Backslash";
    case VK_OEM_4: return "LBracket";
    case VK_OEM_6: return "RBracket";
    case VK_OEM_MINUS: return "Minus";
    case VK_OEM_PLUS: return "Plus";
    case VK_CAPITAL: return "CapsLock";
    case VK_ESCAPE: return "Escape";
    default: return nullptr;
  }
}

}  // namespace

void BeginKeyCapture() {
  for (int vk = 1; vk < 256; ++vk) g_down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
}

std::string PollCapturedKey() {
  std::string found;
  for (int vk = 1; vk < 256; ++vk) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool fresh = down && !g_down[vk];
    g_down[vk] = down;
    if (!fresh || !found.empty()) continue;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
      found = std::string(1, char(vk));
    } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
      found = "Numpad" + std::to_string(vk - VK_NUMPAD0);
    } else if (const char* name = NamedKey(vk)) {
      found = name;
    }
  }
  return found;
}
#else
// A phone with a keyboard (2026-11-17): SDL's keyboard state, by scancode, in the same names.
namespace {
bool g_down[SDL_SCANCODE_COUNT];
const char* NamedScancode(int sc) {
  switch (sc) {
    case SDL_SCANCODE_SPACE: return "Space";
    case SDL_SCANCODE_RETURN: return "Return";
    case SDL_SCANCODE_TAB: return "Tab";
    case SDL_SCANCODE_BACKSPACE: return "Backspace";
    case SDL_SCANCODE_DELETE: return "Delete";
    case SDL_SCANCODE_INSERT: return "Insert";
    case SDL_SCANCODE_HOME: return "Home";
    case SDL_SCANCODE_END: return "End";
    case SDL_SCANCODE_PAGEUP: return "PageUp";
    case SDL_SCANCODE_PAGEDOWN: return "PageDown";
    case SDL_SCANCODE_LEFT: return "Left";
    case SDL_SCANCODE_RIGHT: return "Right";
    case SDL_SCANCODE_UP: return "Up";
    case SDL_SCANCODE_DOWN: return "Down";
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "Shift";
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return "Control";
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return "Alt";
    case SDL_SCANCODE_SEMICOLON: return "Semicolon";
    case SDL_SCANCODE_APOSTROPHE: return "Quote";
    case SDL_SCANCODE_COMMA: return "Comma";
    case SDL_SCANCODE_PERIOD: return "Period";
    case SDL_SCANCODE_SLASH: return "Slash";
    case SDL_SCANCODE_BACKSLASH: return "Backslash";
    case SDL_SCANCODE_LEFTBRACKET: return "LBracket";
    case SDL_SCANCODE_RIGHTBRACKET: return "RBracket";
    case SDL_SCANCODE_MINUS: return "Minus";
    case SDL_SCANCODE_EQUALS: return "Plus";
    case SDL_SCANCODE_CAPSLOCK: return "CapsLock";
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_AC_BACK: return "Escape";
    default: return nullptr;
  }
}
}  // namespace

void BeginKeyCapture() {
  int n = 0;
  const bool* keys = SDL_GetKeyboardState(&n);
  for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) g_down[i] = keys && i < n && keys[i];
}

std::string PollCapturedKey() {
  int n = 0;
  const bool* keys = SDL_GetKeyboardState(&n);
  if (!keys) return {};
  std::string found;
  for (int sc = 1; sc < SDL_SCANCODE_COUNT && sc < n; ++sc) {
    const bool down = keys[sc];
    const bool fresh = down && !g_down[sc];
    g_down[sc] = down;
    if (!fresh || !found.empty()) continue;
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
      found = std::string(1, char('A' + (sc - SDL_SCANCODE_A)));
    } else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) {
      found = std::string(1, sc == SDL_SCANCODE_0 ? '0' : char('1' + (sc - SDL_SCANCODE_1)));
    } else if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_0) {
      found = "Numpad" + std::to_string(sc == SDL_SCANCODE_KP_0 ? 0 : 1 + (sc - SDL_SCANCODE_KP_1));
    } else if (const char* name = NamedScancode(sc)) {
      found = name;
    }
  }
  return found;
}
#endif

}  // namespace nier
