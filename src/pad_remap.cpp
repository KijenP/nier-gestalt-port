// nier - the controller's buttons remapped (pad_remap.h).

#include "pad_remap.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(nier_pad_map, "", "Nier",
                      "Controller buttons: the physical button of A,B,X,Y,LB,RB,LT,RT,L3,R3,Back (empty: as printed)");

// Options of the controls that live with no file of their own (weapon_switch.h, tutorials.h).
REXCVAR_DEFINE_STRING(nier_weapon_switch, "tap", "Nier", "Weapon buttons: tap (a tap equips, holding opens the menu) or hold");
REXCVAR_DEFINE_STRING(nier_tutorials_seen, "", "Nier", "The port's tutorials already read (ids, comma separated)");

namespace nier::pad_remap {
namespace {

// The XInput bits of the inputs (the triggers have none: read from their axes).
constexpr uint16_t kBits[kCount] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0, 0, 0x0040, 0x0080, 0x0020};
constexpr const char* kIds[kCount] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3", "R3", "Back"};
constexpr uint8_t kTriggerOn = 64;

std::atomic<int> g_capture{-1};  // -1 not capturing, -2 waiting, >= 0 the press taken
uint32_t g_held_at_start = 0;   // the capture's thread only (the pad's)
bool g_release_wait = false;     // the pad's thread: a capture's press still held
std::mutex g_mutex;
std::string g_cached_value = "\x01";
std::array<int, kCount> g_cached{};

bool Down(const rex::input::X_INPUT_GAMEPAD& g, int input) {
  if (input == kLT) return g.left_trigger >= kTriggerOn;
  if (input == kRT) return g.right_trigger >= kTriggerOn;
  return (g.buttons & kBits[input]) != 0;
}

std::array<int, kCount> Parse(const std::string& value) {
  std::array<int, kCount> map;
  for (int i = 0; i < kCount; ++i) map[size_t(i)] = i;
  std::stringstream in(value);
  std::string item;
  for (int i = 0; i < kCount && std::getline(in, item, ','); ++i) {
    for (int p = 0; p < kCount; ++p)
      if (item == kIds[p]) map[size_t(i)] = p;
  }
  return map;
}

void Store(const std::array<int, kCount>& map) {
  std::string v;
  bool identity = true;
  for (int i = 0; i < kCount; ++i) {
    v += (i ? "," : "") + std::string(kIds[map[size_t(i)]]);
    identity &= map[size_t(i)] == i;
  }
  rex::cvar::SetFlagByName("nier_pad_map", identity ? "" : v);
}

}  // namespace

const char* ActionName(int input) {
  static const char* const kActions[kCount] = {"Jump",  "Action",          "Attack", "Heavy Attack", "Magic 1", "Magic 2",
                                               "Guard", "Dodge", "Left Stick Press", "Recenter Camera", "Large Map"};
  return input >= 0 && input < kCount ? kActions[input] : "";
}

std::string ButtonName(int input) {
  static const char* const kPs3[kCount] = {"Cross", "Circle", "Square", "Triangle", "L1", "R1",
                                           "L2",    "R2",     "L3",     "R3",       "Select"};
  if (input < 0 || input >= kCount) return "-";
  return rex::cvar::GetFlagByName("nier_button_icons") == "ps3" ? kPs3[input] : kIds[input];
}

std::array<int, kCount> Map() {
  const std::string v = REXCVAR_GET(nier_pad_map);
  std::lock_guard lock(g_mutex);
  if (v != g_cached_value) {
    g_cached = Parse(v);
    g_cached_value = v;
  }
  return g_cached;
}

void Assign(int input, int physical) {
  if (input < 0 || input >= kCount || physical < 0 || physical >= kCount) return;
  auto map = Map();
  for (int i = 0; i < kCount; ++i)
    if (i != input && map[size_t(i)] == physical) map[size_t(i)] = map[size_t(input)];  // a swap
  map[size_t(input)] = physical;
  Store(map);
  REXLOG_INFO("Controle: {} agora no botao {}", kIds[input], kIds[physical]);
}

void RestoreDefaults() { rex::cvar::SetFlagByName("nier_pad_map", ""); }

void BeginCapture() {
  g_held_at_start = ~0u;  // the next state read fills it (what is down then does not count)
  g_capture = -2;
}
int TakeCaptured() {
  int v = g_capture.load();
  if (v >= 0 && g_capture.compare_exchange_strong(v, -1)) return v;
  return -1;
}
void CancelCapture() { g_capture = -1; }
bool Capturing() { return g_capture.load() == -2; }

void Apply(rex::input::X_INPUT_GAMEPAD& g) {
  // After a capture the press that was taken stays the page's until every button is up (2026-11-17: the A taken
  // for Jump reached the page as its A and asked again; B closed the page).
  if (g_capture.load() >= 0 || g_release_wait) {
    bool any = (g.buttons & 0xF3E0) != 0 || g.left_trigger >= kTriggerOn || g.right_trigger >= kTriggerOn;
    if (g_capture.load() >= 0 || any) {
      g_release_wait = true;
      g.buttons = uint16_t(g.buttons & 0x001F);  // the d-pad and Start pass
      g.left_trigger = g.right_trigger = 0;
      if (!any && g_capture.load() < 0) g_release_wait = false;
      return;
    }
    g_release_wait = false;
  }
  if (g_capture.load() == -2) {
    uint32_t down = 0;
    for (int p = 0; p < kCount; ++p)
      if (Down(g, p)) down |= 1u << p;
    if (g_held_at_start == ~0u) g_held_at_start = down;
    g_held_at_start &= down;  // released ones count again
    const uint32_t fresh = down & ~g_held_at_start;
    for (int p = 0; p < kCount; ++p)
      if (fresh & (1u << p)) {
        // Held until every button is up from this very poll: the page may take the capture before the next one,
        // and the press, already the new button of its action, reached the page as its A (a new capture, again
        // and again, 2026-11-17).
        g_release_wait = true;
        int expected = -2;
        g_capture.compare_exchange_strong(expected, p);
        break;
      }
    const uint32_t packetless_buttons = g.buttons & 0x0010;  // Start still reaches the menus
    g.buttons = uint16_t(packetless_buttons);
    g.left_trigger = g.right_trigger = 0;
    return;
  }
  const auto map = Map();
  bool identity = true;
  for (int i = 0; i < kCount; ++i) identity &= map[size_t(i)] == i;
  if (identity) return;
  const rex::input::X_INPUT_GAMEPAD raw = g;
  uint16_t buttons = raw.buttons;
  for (int i = 0; i < kCount; ++i) buttons &= uint16_t(~kBits[i]);
  uint8_t lt = 0, rt = 0;
  for (int i = 0; i < kCount; ++i) {
    const int p = map[size_t(i)];
    // A trigger read from a trigger keeps its pressure; from a button, all or nothing.
    const uint8_t level = p == kLT ? raw.left_trigger : p == kRT ? raw.right_trigger : (Down(raw, p) ? 255 : 0);
    if (i == kLT) lt = level;
    else if (i == kRT) rt = level;
    else if (Down(raw, p)) buttons |= kBits[i];
  }
  g.buttons = buttons;
  g.left_trigger = lt;
  g.right_trigger = rt;
}

}  // namespace nier::pad_remap
