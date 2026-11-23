// nier - in-game performance meter (Video Options > Medidor de desempenho, F2).
//
// External overlays count host presents, which the SDK issues independently
// of the game (several per game frame in menus), so they report numbers the
// game never reaches. This counts the game's own frames: every call of its
// present function (sub_8257AB38, the one that calls VdSwap), timed on the
// host. GPU % is Windows' "GPU Engine" 3D utilization of this process, CPU %
// is this process's CPU time over all logical processors.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>

namespace nier {

// Game frame timestamps (written by the present hook in perf_overlay.cpp).
struct GameFrameClock {
  std::mutex mutex;
  std::deque<std::chrono::steady_clock::time_point> frames;  // last ~2 s
  void OnFrame() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    frames.push_back(now);
    while (!frames.empty() && now - frames.front() > std::chrono::seconds(2)) {
      frames.pop_front();
    }
  }
};
GameFrameClock& game_frame_clock();

// GPU and CPU usage sampled once per second on its own thread.
class UsageSampler {
 public:
  UsageSampler();
  ~UsageSampler();
  float gpu_percent() const { return gpu_percent_; }
  float cpu_percent() const { return cpu_percent_; }
  // Android (2026-11-13): the fastest core's clock, the phone's memory in use (and the game's own), the
  // temperature (the CPU's when the system lets it be read, else the battery's). < 0: unknown.
  float cpu_ghz() const { return cpu_ghz_; }
  float ram_percent() const { return ram_percent_; }
  float ram_game_gb() const { return ram_game_gb_; }
  float temperature() const { return temperature_; }
  bool temperature_is_battery() const { return temperature_battery_; }

 private:
  void Run();
  std::atomic<bool> running_{true};
  std::atomic<float> gpu_percent_{-1.0f};
  std::atomic<float> cpu_percent_{-1.0f};
  std::atomic<float> cpu_ghz_{-1.0f};
  std::atomic<float> ram_percent_{-1.0f};
  std::atomic<float> ram_game_gb_{-1.0f};
  std::atomic<float> temperature_{-1.0f};
  std::atomic<bool> temperature_battery_{false};
  std::thread thread_;
};

class PerfOverlayDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit PerfOverlayDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  UsageSampler usage_;
};

}  // namespace nier
