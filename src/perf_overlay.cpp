// nier - in-game performance meter. See perf_overlay.h.

#include "perf_overlay.h"

#include <algorithm>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "pc_options.h"

REXCVAR_DEFINE_BOOL(perf_overlay, false, "Nier", "Show the in-game performance meter");

#if defined(__ANDROID__)
#include <jni.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL3/SDL_system.h>
#endif

#if NIER_RENUT_RENDERER
#include "renut_engine/perf/frame_stats.h"
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/banjo_shared_hooks.h"
#endif

extern "C" void __imp__sub_8257AB38(PPCContext& ctx, uint8_t* base);  // game present (VdSwap)

namespace nier::weapon_switch {
void EquipTick(PPCContext& ctx, uint8_t* base);  // video_options_menu.cpp
}

REX_HOOK_RAW(sub_8257AB38) {
  nier::game_frame_clock().OnFrame();
  nier::weapon_switch::EquipTick(ctx, base);  // the quick weapon switch's equip, on the game's thread
  nier::menu_mouse::game_frame().fetch_add(1);  // the menus' pointer steps a frame each (menu_mouse.h)
#if NIER_RENUT_RENDERER
  // Renut's renderer, when chosen (nier_renderer = "renut"), takes the present (D3DDevice_Swap).
  BANJO_RENUT_YIELD_SHARED(sub_8257AB38);
#endif
  __imp__sub_8257AB38(ctx, base);
}

namespace nier {

GameFrameClock& game_frame_clock() {
  static GameFrameClock clock;
  return clock;
}

UsageSampler::UsageSampler() : thread_([this] { Run(); }) {}

UsageSampler::~UsageSampler() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

#if defined(__ANDROID__)
namespace {
bool ReadFile(const char* path, char* out, size_t size) {
  std::FILE* f = std::fopen(path, "r");
  if (!f) return false;
  const size_t n = std::fread(out, 1, size - 1, f);
  std::fclose(f);
  out[n] = 0;
  return n > 0;
}
long long ReadNumber(const char* path) {
  char buf[64];
  return ReadFile(path, buf, sizeof(buf)) ? std::atoll(buf) : -1;
}
// The process's CPU time (utime + stime, clock ticks): /proc/self/stat's 14th and 15th fields, after the name.
long long ProcessTicks() {
  char buf[1024];
  if (!ReadFile("/proc/self/stat", buf, sizeof(buf))) return -1;
  const char* p = std::strrchr(buf, ')');
  if (!p) return -1;
  long long utime = 0, stime = 0;
  // After ")": state(3) ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime(14) stime(15).
  if (std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lld %lld", &utime, &stime) != 2) return -1;
  return utime + stime;
}
// The thermistor by the CPU ("cpu_therm"), when the system lets the app read it (else the battery's, through Java).
// Not the cores' own sensors: their hottest read 91 C in play (true of a core, not of the phone).
std::vector<std::string> CpuThermalZones() {
  std::vector<std::string> zones;
  for (int i = 0; i < 128; ++i) {
    char path[96], type[64];
    std::snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", i);
    if (!ReadFile(path, type, sizeof(type))) continue;
    if (std::strncmp(type, "cpu_therm", 9) != 0) continue;
    std::snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
    if (ReadNumber(path) > 0) zones.push_back(path);
  }
  return zones;
}
float BatteryTemperature() {
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return -1.0f;
  float result = -1.0f;
  jclass cls = env->GetObjectClass(activity);
  if (jmethodID m = env->GetMethodID(cls, "nierBatteryTemperature", "()I")) {
    const jint tenths = env->CallIntMethod(activity, m);
    if (tenths > 0) result = float(tenths) / 10.0f;
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  return result;
}
}  // namespace
#endif

void UsageSampler::Run() {
#if defined(__ANDROID__)
  const long ticks_per_s = sysconf(_SC_CLK_TCK);
  const long cores = sysconf(_SC_NPROCESSORS_CONF);
  const long page = sysconf(_SC_PAGESIZE);
  const std::vector<std::string> zones = CpuThermalZones();
  REXLOG_INFO("Medidor: {} zonas termicas da CPU legiveis{}", zones.size(), zones.empty() ? " (usa a bateria)" : "");
  long long last_ticks = ProcessTicks();
  auto last_wall = std::chrono::steady_clock::now();
  while (running_) {
    for (int i = 0; i < 10 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto now = std::chrono::steady_clock::now();
    const long long ticks = ProcessTicks();
    const double wall = std::chrono::duration<double>(now - last_wall).count();
    if (ticks >= 0 && last_ticks >= 0 && wall > 0 && ticks_per_s > 0 && cores > 0)
      cpu_percent_ = float(std::min(100.0, 100.0 * double(ticks - last_ticks) / double(ticks_per_s) / wall / double(cores)));
    last_ticks = ticks;
    last_wall = now;
    long long khz = -1;
    for (long c = 0; c < cores; ++c) {
      char path[96];
      std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpufreq/scaling_cur_freq", c);
      khz = std::max(khz, ReadNumber(path));
    }
    cpu_ghz_ = khz > 0 ? float(double(khz) / 1e6) : -1.0f;
    char mem[4096];
    if (ReadFile("/proc/meminfo", mem, sizeof(mem))) {
      long long total = 0, avail = 0;
      if (const char* t = std::strstr(mem, "MemTotal:")) total = std::atoll(t + 9);
      if (const char* a = std::strstr(mem, "MemAvailable:")) avail = std::atoll(a + 13);
      if (total > 0) ram_percent_ = float(100.0 * double(total - avail) / double(total));
    }
    char statm[128];
    long long size_pages = 0, resident = 0;
    if (ReadFile("/proc/self/statm", statm, sizeof(statm)) &&
        std::sscanf(statm, "%lld %lld", &size_pages, &resident) == 2)
      ram_game_gb_ = float(double(resident) * double(page) / (1024.0 * 1024.0 * 1024.0));
    long long milli = -1;
    for (const std::string& z : zones) milli = std::max(milli, ReadNumber(z.c_str()));
    if (milli > 0) {
      temperature_ = float(milli >= 1000 ? double(milli) / 1000.0 : double(milli));
      temperature_battery_ = false;
    } else {
      temperature_ = BatteryTemperature();
      temperature_battery_ = true;
    }
#if NIER_RENUT_RENDERER
    // The GPU's busy share: its time a frame (the renderer's timestamp queries) times the frames a second.
    const renut::frame_stats::Snapshot snap = renut::frame_stats::Get();
    if (snap.gpu_known) gpu_percent_ = float(std::min(100.0, snap.gpu_ms * snap.fps / 10.0));
#endif
  }
#elif !defined(_WIN32)
  // GPU/CPU usage counters are Windows-only for now (shown as 0).
  while (running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
#else
  // GPU: 3D engine utilization of this process (all adapters/engines summed).
  PDH_HQUERY query = nullptr;
  PDH_HCOUNTER gpu_counter = nullptr;
  if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
    std::wstring path = L"\\GPU Engine(pid_" + std::to_wstring(GetCurrentProcessId()) +
                        L"_*engtype_3D)\\Utilization Percentage";
    if (PdhAddEnglishCounterW(query, path.c_str(), 0, &gpu_counter) != ERROR_SUCCESS) {
      gpu_counter = nullptr;
    }
    PdhCollectQueryData(query);
  }
  // CPU: process time over wall time and logical processors.
  SYSTEM_INFO system_info;
  GetSystemInfo(&system_info);
  auto process_time = [] {
    FILETIME creation, exit, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    auto to_u64 = [](FILETIME t) { return uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime; };
    return to_u64(kernel) + to_u64(user);  // 100 ns units
  };
  uint64_t last_cpu = process_time();
  auto last_wall = std::chrono::steady_clock::now();
  std::vector<uint8_t> buffer;

  while (running_) {
    for (int i = 0; i < 10 && running_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    auto now = std::chrono::steady_clock::now();
    uint64_t cpu = process_time();
    double wall_100ns = std::chrono::duration<double>(now - last_wall).count() * 1e7;
    if (wall_100ns > 0) {
      cpu_percent_ = float(100.0 * double(cpu - last_cpu) /
                           (wall_100ns * system_info.dwNumberOfProcessors));
    }
    last_cpu = cpu;
    last_wall = now;

    if (query && gpu_counter && PdhCollectQueryData(query) == ERROR_SUCCESS) {
      DWORD size = 0, count = 0;
      if (PdhGetFormattedCounterArrayW(gpu_counter, PDH_FMT_DOUBLE, &size, &count, nullptr) ==
          PDH_MORE_DATA) {
        buffer.resize(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(gpu_counter, PDH_FMT_DOUBLE, &size, &count, items) ==
            ERROR_SUCCESS) {
          double sum = 0;
          for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus == ERROR_SUCCESS) sum += items[i].FmtValue.doubleValue;
          }
          gpu_percent_ = float(std::min(sum, 100.0));
        }
      }
    }
  }
  if (query) PdhCloseQuery(query);
#endif
}

void PerfOverlayDialog::OnDraw(ImGuiIO& io) {
  // Last second of game frames.
  int frames = 0;
  double avg_ms = 0, worst_ms = 0;
  {
    auto& clock = game_frame_clock();
    std::lock_guard lock(clock.mutex);
    auto now = std::chrono::steady_clock::now();
    auto window_start = now - std::chrono::seconds(1);
    std::chrono::steady_clock::time_point prev{};
    for (auto t : clock.frames) {
      if (t < window_start) {
        prev = t;
        continue;
      }
      ++frames;
      if (prev.time_since_epoch().count()) {
        double ms = std::chrono::duration<double, std::milli>(t - prev).count();
        avg_ms += ms;
        worst_ms = std::max(worst_ms, ms);
      }
      prev = t;
    }
    if (frames) avg_ms /= frames;
  }

#if defined(__ANDROID__)
  // A panel of cards (2026-11-13, the user's plan): FPS with the frame time, then CPU, GPU, RAM and the temperature,
  // each with its value and a bar coloured by how high it is. Placed inside the picture, clear of the screen's
  // rounded corner and its camera.
  {
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const float s = H / 1080.0f;
    ImFont* font = pc_options_font() ? pc_options_font() : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float label_px = 19 * s, value_px = 30 * s, small_px = 17 * s;
    const ImU32 kText = IM_COL32(236, 232, 220, 255), kDim = IM_COL32(170, 166, 156, 255);
    auto level = [](float f) {  // 0..1 -> green, amber, red
      return f < 0.6f ? IM_COL32(120, 200, 130, 255) : f < 0.85f ? IM_COL32(230, 185, 90, 255)
                                                      : IM_COL32(230, 100, 90, 255);
    };
    struct Card {
      const char* label;
      std::string value, detail;
      float fill;  // < 0: no bar
      ImU32 colour;
    };
    const float cpu = usage_.cpu_percent(), gpu = usage_.gpu_percent(), ghz = usage_.cpu_ghz();
    const float ram = usage_.ram_percent(), game_gb = usage_.ram_game_gb(), temp = usage_.temperature();
    char buf[64];
    auto pct = [&](float v) { return v < 0 ? std::string("--") : std::to_string(int(v + 0.5f)) + "%"; };
    Card cards[5];
    std::snprintf(buf, sizeof(buf), "%.1f ms", avg_ms);
    const int target = std::max(1, std::atoi(rex::cvar::GetFlagByName("frame_rate_limit").c_str()));
    const float fps_share = std::min(1.0f, float(frames) / float(target));
    cards[0] = {"FPS", std::to_string(frames), buf, -1.0f,
                fps_share >= 0.95f ? IM_COL32(120, 200, 130, 255) : fps_share >= 0.8f ? IM_COL32(230, 185, 90, 255)
                                                                                   : IM_COL32(230, 100, 90, 255)};
    if (ghz > 0) std::snprintf(buf, sizeof(buf), "%.2f GHz", ghz); else buf[0] = 0;
    cards[1] = {"CPU", pct(cpu), buf, cpu < 0 ? -1.0f : cpu / 100.0f, level(cpu / 100.0f)};
    cards[2] = {"GPU", pct(gpu), "", gpu < 0 ? -1.0f : gpu / 100.0f, level(gpu / 100.0f)};
    if (game_gb > 0) std::snprintf(buf, sizeof(buf), "jogo %.1f GB", game_gb); else buf[0] = 0;
    cards[3] = {"RAM", pct(ram), buf, ram < 0 ? -1.0f : ram / 100.0f, level(ram / 100.0f)};
    std::string t = temp < 0 ? std::string("--") : std::to_string(int(temp + 0.5f)) + "\xC2\xB0" "C";
    const float heat = temp < 0 ? 0.0f : std::clamp((temp - 30.0f) / 20.0f, 0.0f, 1.0f);  // 30 C cool, 50 C hot
    cards[4] = {"TEMP", t, "", temp < 0 ? -1.0f : heat, level(heat)};

    const float pad = 12 * s, gap = 8 * s, card_w = 112 * s, card_h = 92 * s;
    // The picture's left edge (present.cpp's FitFrame trims a wide screen's top and bottom, the rest are bars), and
    // 4% of the width more for the corner.
    const float game = 1280.0f / 720.0f, aspect = W / std::max(H, 1.0f);
    const std::string fit = rex::cvar::GetFlagByName("nier_screen_fit");  // present.cpp
    const float trim = fit == "stretch" || fit == "wide" || fit == "widereal" ? 1.0f : fit == "bars" ? 0.0f : 0.15f;
    const float shown = aspect > game ? game / std::max(game / aspect, 1.0f - trim) : game;
    const float bar_x = aspect > shown ? (W - H * shown) * 0.5f : 0.0f;
    // Dragged anywhere (touch_controls.h), kept on screen; a tap shrinks it to the FPS card and back.
    auto& panel = nier::menu_mouse::perf_panel();
    const int shown_cards = panel.mini.load() ? 1 : 5;
    const float panel_w = pad * 2 + card_w * shown_cards + gap * (shown_cards - 1), panel_h = pad * 2 + card_h;
    const float home_x = bar_x + 0.035f * W, home_y = 0.035f * H;
    const float x0 = std::clamp(home_x + panel.dx.load() * W, 0.0f, std::max(0.0f, W - panel_w));
    const float y0 = std::clamp(home_y + panel.dy.load() * H, 0.0f, std::max(0.0f, H - panel_h));
    panel.dx = (x0 - home_x) / W;  // held at the edge (a drag past it does not have to come back first)
    panel.dy = (y0 - home_y) / H;
    panel.x0 = x0 / W;
    panel.y0 = y0 / H;
    panel.x1 = (x0 + panel_w) / W;
    panel.y1 = (y0 + panel_h) / H;
    panel.drawn_ms = nier::menu_mouse::NowMs();
    dl->AddRectFilled({x0, y0}, {x0 + panel_w, y0 + panel_h}, IM_COL32(16, 16, 20, 190), 14 * s);
    dl->AddRect({x0, y0}, {x0 + panel_w, y0 + panel_h}, IM_COL32(255, 255, 255, 40), 14 * s, 0, 1.5f * s);
    for (int i = 0; i < shown_cards; ++i) {
      const Card& c = cards[i];
      const float cx = x0 + pad + i * (card_w + gap), cy = y0 + pad;
      dl->AddRectFilled({cx, cy}, {cx + card_w, cy + card_h}, IM_COL32(255, 255, 255, 14), 9 * s);
      dl->AddText(font, label_px, {cx + 10 * s, cy + 7 * s}, kDim, c.label);
      dl->AddText(font, value_px, {cx + 10 * s, cy + 28 * s}, i == 0 || c.fill >= 0 ? c.colour : kText,
                  c.value.c_str());
      if (!c.detail.empty())
        dl->AddText(font, small_px, {cx + 10 * s, cy + 62 * s}, kDim, c.detail.c_str());
      if (c.fill >= 0) {  // a thin bar along the card's bottom
        const float bx0 = cx + 10 * s, bx1 = cx + card_w - 10 * s, by = cy + card_h - 9 * s;
        dl->AddRectFilled({bx0, by}, {bx1, by + 4 * s}, IM_COL32(255, 255, 255, 30), 2 * s);
        dl->AddRectFilled({bx0, by}, {bx0 + (bx1 - bx0) * std::clamp(c.fill, 0.02f, 1.0f), by + 4 * s}, c.colour,
                          2 * s);
      }
    }
    return;
  }
#endif
  char text[256];
  float gpu = usage_.gpu_percent(), cpu = usage_.cpu_percent();
  std::snprintf(text, sizeof(text), "%d fps  |  %.1f ms (pior %.1f)  |  GPU %s  |  CPU %s", frames,
                avg_ms, worst_ms,
                gpu < 0 ? "--" : (std::to_string(int(gpu + 0.5f)) + "%").c_str(),
                cpu < 0 ? "--" : (std::to_string(int(cpu + 0.5f)) + "%").c_str());

  const float scale = io.DisplaySize.y / 1080.0f;
  ImFont* font = pc_options_font() ? pc_options_font() : ImGui::GetFont();
  const float size = 24 * scale;
  ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0, text);
  ImVec2 pos{14 * scale, 10 * scale};
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled({pos.x - 8 * scale, pos.y - 4 * scale},
                    {pos.x + text_size.x + 8 * scale, pos.y + text_size.y + 4 * scale},
                    IM_COL32(0, 0, 0, 150));
  dl->AddText(font, size, pos, IM_COL32(230, 226, 214, 255), text);
}

}  // namespace nier
