// nier - the controller's buttons remapped (Options > Control Options > Controller, 2026-11-17): each of the game's
// buttons (A jump, B action, X attack, Y heavy attack, LB / RB magic, LT guard, RT dodge, L3, R3 camera, Back map)
// read from the physical button the player chose. Only the physical pads (the SDL driver); the keyboard has its own
// keys and the on-screen buttons their own places. The d-pad, the sticks and Start stay as they are (Start always
// reaches the menus). Kept in nier.toml as nier_pad_map: the physical button of each game button, in that order.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <memory>
#include <vector>

#include <rex/input/input_driver.h>

namespace nier::pad_remap {

using rex::X_RESULT;
using rex::X_STATUS;

enum Input : int { kA, kB, kX, kY, kLB, kRB, kLT, kRT, kLS, kRS, kBack, kCount };

// The game's button (as kEnBindLabels' actions) of each input.
const char* ActionName(int input);
// A physical button's name in the icon style chosen (Xbox 360 or PS3).
std::string ButtonName(int input);
// The physical button each game button reads now.
std::array<int, kCount> Map();
// Gives `input` the physical button `physical` (the one that had it gets the old one: a swap).
void Assign(int input, int physical);
void RestoreDefaults();

// The next physical button pressed (a new press) is taken for the page: Begin, then Take until it is >= 0
// (-1 still waiting). While waiting the pad gives the game nothing.
void BeginCapture();
int TakeCaptured();
void CancelCapture();
bool Capturing();

// The SDL driver's state, remapped in place (and the capture's press noted).
void Apply(rex::input::X_INPUT_GAMEPAD& g);

// Wraps the physical pads' driver.
class RemapDriver final : public rex::input::InputDriver {
 public:
  explicit RemapDriver(std::unique_ptr<rex::input::InputDriver> inner)
      : InputDriver(nullptr, 0), inner_(std::move(inner)) {}
  rex::X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override { inner_->EnumerateDevices(out); }
  void OnWindowAvailable(rex::ui::Window* window) override { inner_->OnWindowAvailable(window); }
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    rex::X_RESULT r = inner_->GetDeviceState(id, out);
    if (r == X_ERROR_SUCCESS && out) Apply(out->gamepad);
    return r;
  }
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                      rex::input::X_INPUT_CAPABILITIES* out) override {
    return inner_->GetDeviceCapabilities(id, flags, out);
  }
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION* v) override {
    return inner_->SetDeviceVibration(id, v);
  }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                                   rex::input::X_INPUT_KEYSTROKE* out) override {
    return inner_->GetDeviceKeystroke(id, flags, out);
  }

 private:
  std::unique_ptr<rex::input::InputDriver> inner_;
};

}  // namespace nier::pad_remap
