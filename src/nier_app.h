// nier - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <filesystem>
#include <optional>
#include <string>
#include <fstream>
#include <utility>

#include <imgui.h>
#include <SDL3/SDL_video.h>

#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system.h>
#include <rex/system/gpu_plugin.h>

#include "cpu_sampler.h"
#include "crash_log.h"
#include "dlc.h"
#include "game_text.h"
#include "input_recorder.h"
#include "keyboard_binds.h"
#include "mods.h"
#include "perf_overlay.h"
#include "achievements_ui.h"
#include "jukebox/jukebox.h"
#include "jukebox/jukebox_ui.h"
#include "tutorials.h"
#include "setup_wizard.h"
#include "write_watch.h"

#if NIER_RENUT_RENDERER
#include <memory>
#include <stdexcept>

#include <rex/ui/immediate_drawer.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#endif

namespace nier {
void ApplyMinimum3DResolution(uint8_t* base);  // game_patches.cpp
}

class NierApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NierApp>(new NierApp(ctx, "nier",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // The game is unplayable without GPU emulation, so default to the Xenos
  // plugin; --gpu_plugin still overrides it.
  void OnPreSetup(rex::RuntimeConfig& config) override {
#if defined(_WIN32)
    // Dev aid: NIER_PIX=<WinPixGpuCapturer.dll> loads PIX's capture layer
    // before the D3D12 device exists, so `pixtool attach` can capture frames
    // mid-game for GPU profiling.
    // NIER_PIX_CAPTURE=<seconds>|<frames>|<file.wpix> then saves that many
    // host presents at that time (what pix3.h's PIXGpuCaptureNextFrames
    // does). The host presents several times per game frame, so one is not
    // enough to hold a whole game frame.
    if (const char* pix = std::getenv("NIER_PIX"); pix && *pix) {
      HMODULE module = LoadLibraryA(pix);
      REXLOG_INFO("PIX capturer: {}", module ? "loaded" : "FAILED");
      const char* spec = std::getenv("NIER_PIX_CAPTURE");
      if (module && spec && *spec) {
        using CaptureNextFrameFn = HRESULT(WINAPI*)(PCWSTR, UINT32);
        auto capture = reinterpret_cast<CaptureNextFrameFn>(GetProcAddress(module, "CaptureNextFrame"));
        std::string s(spec);
        auto bar1 = s.find('|'), bar2 = s.find('|', bar1 + 1);
        int seconds = std::atoi(s.c_str());
        UINT32 frames = UINT32(std::max(1, std::atoi(s.c_str() + bar1 + 1)));
        std::filesystem::path file = s.substr(bar2 + 1);
        std::thread([capture, seconds, frames, file] {
          std::this_thread::sleep_for(std::chrono::seconds(seconds));
          HRESULT hr = capture ? capture(file.wstring().c_str(), frames) : E_FAIL;
          REXLOG_INFO("PIX capture requested -> {} (hr {:08X})", file.string(), uint32_t(hr));
        }).detach();
      }
    }
#endif
    nier::InstallCrashLog();  // NieR (2026-11-16): Android, native crashes in the log (crash_log.cpp)
#if NIER_RENUT_RENDERER
    // nier_renderer = "renut": Renut draws NieR's Direct3D through plume, with no SDK graphics system or GPU plugin
    // (the SDK's detached mode; src/renut_engine/gpu/overlay/app_integration.h).
    if (banjo::renut_gpu::Active()) {
      renut::gpu::ConfigureRuntime(config);
      REXLOG_INFO("Renderer: Renut (NieR's Direct3D through plume; experimental)");
      config.input_factory = nier::CreateInputSystem;
      return;
    }
#endif
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    // Load the plugin with the chosen API. If the SDK was built without it
    // (or it has no usable device), the SDK loads the plugin's default
    // (whichever API it has compiled in).
    std::string api = rex::cvar::GetFlagByName("renderer");
    if (!config.graphics) {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, api);
      if (!config.graphics) {
        REXLOG_WARN("Renderer '{}' unavailable, using the plugin's default", api);
      }
    }
    REXLOG_INFO("Renderer: {}", config.graphics ? api : std::string("plugin default"));
    config.input_factory = nier::CreateInputSystem;
  }

  // NieR defaults, applied only when neither nier.toml nor the command line
  // chose a value. Set here because these flags live in the GPU plugin, which
  // is not loaded yet in OnPreSetup.
  //  - readback_resolve=fast + readback_resolve_lazy_addresses=1EC22000: eye
  //    adaptation reads one 4 KB resolve (the last of the luminance chain) on
  //    the CPU; without readback gameplay renders almost white. The game needs
  //    that frame's value: "fast" alone delivers it late (fine at 30 fps,
  //    white/oscillating at 60) and "full" stalls (60 -> ~53 fps). Lazy
  //    readback of just that surface measured 59.8 fps at 60 and equal or
  //    better than "fast" at 30, while copying 4 KB instead of ~13 MB a frame.
  //  - execute_unclipped_draw_vs_on_cpu: Xenia's default; without it fog and
  //    other full-screen effects go missing (confirmed by the player).
  //  - Keyboard/mouse with NieR Replicant ver.1.22's PC defaults
  //    (keyboard_binds.h). On only while no pad is connected (mnk_mode is
  //    switched by GateDriver in input_recorder.h).
  //    Shift/Ctrl/Alt are keys of their own, hence non-exact modifiers (WASD
  //    keeps working while one is held).
  void OnPostSetup() override {
    // The window's title (the SDK writes "nier <build stamp>"); "nier" stays the app's name for its folders and
    // nier.toml.
    if (window()) window()->SetTitle("NieR Gestalt");
    std::vector<std::pair<const char*, const char*>> defaults = {
        {"readback_resolve", "fast"},
        {"readback_resolve_lazy_addresses", "1EC22000"},
        {"execute_unclipped_draw_vs_on_cpu", "true"},
        {"mnk_mouse", "true"},
        {"mnk_sensitivity", "2.0"},
        {"mnk_exact_modifiers", "false"},
        {"keybind_lstick_press", ""},
    };
    for (const nier::KeyBindRow& row : nier::KeyBindRows()) {
      defaults.emplace_back(row.cvar, row.default_keys);
    }
    for (auto [name, value] : defaults) {
      if (!rex::cvar::HasNonDefaultValue(name)) {
        rex::cvar::SetFlagByName(name, value);
      }
      REXLOG_INFO("{} = {}", name, rex::cvar::GetFlagByName(name));
    }
    REXLOG_INFO("Startup: settings applied");
    FollowResolutionChanges();
    REXLOG_INFO("Startup: resolution watch ready");
    // Text languages and translations (TEXT next to the game). The saved
    // choice is read now, before the game asks for its language; the
    // TEXT\EXAMPLE copy of the game's text is made once, in the background.
    nier::text::Init(config_path_.parent_path() / "TEXT");
    REXLOG_INFO("Startup: texts ready");
    std::thread([] { nier::text::ExtractExample(); }).detach();
    // The PC screens are drawn with the game's own menu art, read from the
    // game data (never shipped).
    std::thread([] {
      auto images = nier::LoadGameImages(nier::MenuArtWanted());
      auto& art = nier::menu_art();
      std::lock_guard lock(art.mutex);
      art.images = std::move(images);
      art.ready = true;
    }).detach();
    // The Jukebox (src/jukebox): the game's music from the game data, the Music folder next to the settings.
    nier::jukebox::Init(config_path_.parent_path());
    StartMemoryDumpTimer();
    nier::StartCpuSampler();
    nier::StartWriteWatch(runtime()->virtual_membase());
    REXLOG_INFO("Startup: setup done");
  }

  // Startup steps in the log (info level, flushed at once): a phone that stops on a black screen shows the last one
  // reached (a Poco C75 stopped after the menu art, 2026-11-15).
  void LaunchModule() override {
    REXLOG_INFO("Startup: game launch queued for the UI thread");
    rex::ReXApp::LaunchModule();
  }
  void OnPreLaunchModule() override { REXLOG_INFO("Startup: UI thread launching the game"); }
  void OnPostLaunchModule(rex::system::XThread*) override { REXLOG_INFO("Startup: game thread prepared, starting"); }

  void OnConfigureFonts(ImFontAtlas* atlas) override {
    // Georgia is a stock Windows serif close to NieR's menu typeface; it is
    // read from the system, never shipped.
    const char* path = "C:\\Windows\\Fonts\\georgia.ttf";
    if (std::filesystem::exists(path)) {
      ImFontConfig cfg;
      cfg.OversampleH = cfg.OversampleV = 2;
      nier::pc_options_title_font() = atlas->AddFontFromFileTTF(path, 48.0f, &cfg);
      // The game's menus use a bold serif: Georgia Bold when present.
      const char* bold = "C:\\Windows\\Fonts\\georgiab.ttf";
      nier::pc_options_font() =
          atlas->AddFontFromFileTTF(std::filesystem::exists(bold) ? bold : path, 40.0f, &cfg);
    }
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    pc_options_ =std::make_unique<nier::PcOptionsDialog>(drawer, config_path_);
    achievements_ = std::make_unique<nier::AchievementsDialog>(drawer);  // Grimoire Weiss's last page
    tutorial_ = std::make_unique<nier::tutorials::TutorialDialog>(drawer);  // the port's own tutorials (tutorials.h)
    jukebox_ = std::make_unique<nier::jukebox::JukeboxDialog>(drawer);   // F6 (Android: its icon)
    rex::ui::RegisterBind("bind_jukebox", "F6", "Jukebox", [] {
      auto& s = nier::pc_options_state();
      s.jukebox_open = !s.jukebox_open;
      s.TakeJukebox();
    });
    touch_ = std::make_unique<nier::touch::TouchDialog>(drawer);  // on-screen buttons
    // The title menu as big cards (title_carousel.h): the phone's; on the PC only with NIER_TITLE_CAROUSEL=1.
    if (nier::title_carousel::Enabled()) title_cards_ = nier::title_carousel::MakeDialog(drawer);
    // NieR (2026-11-14, the user): no developer keys in the released game. The SDK's F3 (debug overlay), F4
    // (settings) and ` (console) go, and our F1 panel too: the settings live in the game's own Options.
    rex::ui::UnregisterBind("bind_debug_overlay");
    rex::ui::UnregisterBind("bind_settings");
    rex::ui::UnregisterBind("bind_console");
    // Performance meter: exists only while enabled (an ImGui dialog keeps the
    // UI repainting). Created/destroyed later on the UI thread, never from
    // inside the dialog drawing that changed the setting.
    perf_drawer_ = drawer;
    // Nier Plus (2026-11-15): code mods' panels (mods.cpp).
    nier::mods::InitUi(drawer, [this](std::function<void()> fn) { app_context().CallInUIThreadDeferred(std::move(fn)); });
    if (rex::cvar::GetFlagByName("perf_overlay") == "true") {
      perf_overlay_ = std::make_unique<nier::PerfOverlayDialog>(drawer);
    }
    rex::cvar::RegisterChangeCallback("perf_overlay", [this](std::string_view, std::string_view v) {
      bool on = v == "true";
      app_context().CallInUIThreadDeferred([this, on] {
        if (on && !perf_overlay_) {
          perf_overlay_ = std::make_unique<nier::PerfOverlayDialog>(perf_drawer_);
        } else if (!on) {
          perf_overlay_.reset();
        }
      });
    });
    // "Quit to Desktop" in the pause menu's Options: closes the window as its X button does.
    nier::pc_options_state().quit_game = [this] {
      app_context().CallInUIThreadDeferred([this] {
        if (window()) window()->RequestClose();
      });
    };
    rex::ui::RegisterBind("bind_perf_overlay", "F2", "Medidor de desempenho", [] {
      rex::cvar::SetFlagByName("perf_overlay",
                               rex::cvar::GetFlagByName("perf_overlay") == "true" ? "false" : "true");
    });
    // The SDK only reads fullscreen at startup; follow later changes live.
    // Settings change on the UI thread, which owns the window.
    rex::cvar::RegisterChangeCallback("fullscreen", [this](std::string_view, std::string_view v) {
      if (window()) window()->SetFullscreen(v == "true");
      if (v != "true") app_context().CallInUIThreadDeferred([] { FitWindow(); });
    });
    app_context().CallInUIThreadDeferred([] { FitWindow(); });  // the first window too
  }

  // NieR draws a fixed 1280x720 frame, so a "resolution" only adds real pixels
  // through the SDK's integer draw scale. At 100% or more, pick the smallest
  // scale that covers the chosen resolution times render_scale_percent; the
  // presenter then resamples to the window (e.g. 1080p renders at 1440p and
  // downsamples). Below 100%, render fewer pixels than the window (nearest
  // scale, e.g. 4K at 67% renders 1440p) and let the presenter upscale it
  // (present_effect, FSR) - the cost of everything, effects included, follows
  // the pixel count. Runs after the GPU plugin is loaded (its flags exist)
  // and before the render target caches are created.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
#if NIER_RENUT_RENDERER
    // Renut's host device, once the window exists (app_integration.h).
    if (banjo::renut_gpu::Active() &&
        !renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); })) {
      throw std::runtime_error("NieR: failed to start Renut's renderer");
    }
#endif
    ApplyRenderScale();
#if defined(_WIN32)
    // NieR (2026-11-15): the first-run window (setup_wizard.h) when nier.toml names no copy of the game that
    // exists; the game starts once the player picked one. Later starts take the disc tile it saved as the
    // window's icon.
    nier::setup::SetWindowIconFromFile(nier::setup::IconPngPath(defaults.config_path));
    if (nier::setup::Needed(defaults.game_data_root)) {
      setup_wizard_ = std::make_unique<nier::setup::SetupWizard>(
          imgui_drawer(), defaults, [this, resume](rex::PathConfig paths) {
            app_context().CallInUIThreadDeferred([this, resume, paths = std::move(paths)] {
              setup_wizard_.reset();
              resume(paths);
            });
          });
      return std::nullopt;
    }
#endif
    return ReXApp::OnFinalizePaths(defaults, std::move(resume));
  }

#if NIER_RENUT_RENDERER
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (banjo::renut_gpu::Active()) return renut::gpu::CreateOverlayDrawer();
    return ReXApp::OnCreateImmediateDrawer();
  }

  // Alt+Enter (2026-11-23): fullscreen <-> window, kept in nier.toml like the Video Options row. The SDK's binds
  // take single keys only, so the combination is caught here before them.
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (e.virtual_key() == rex::ui::VirtualKey::kReturn && e.is_alt_pressed() && !e.prev_state()) {
      const bool full = rex::cvar::GetFlagByName("fullscreen") == "true";
      rex::cvar::SetFlagByName("fullscreen", full ? "false" : "true");  // the change callback resizes the window
      if (!config_path_.empty()) nier::SavePcSettings(config_path_);
      e.set_handled(true);
      return;
    }
    rex::ui::ProcessKeyEvent(e);
  }

  void OnWindowPixelSizeChanged(uint32_t width, uint32_t height) override {
    ReXApp::OnWindowPixelSizeChanged(width, height);
    if (banjo::renut_gpu::Active()) renut::gpu::OnWindowResized();
  }

  void OnShutdown() override {
    if (banjo::renut_gpu::Active()) renut::gpu::ShutdownRenderer();
  }
#endif

  static void ApplyRenderScale() {
    int height = ResolutionHeight(rex::cvar::GetFlagByName("resolution"));
    int percent = std::clamp(std::atoi(rex::cvar::GetFlagByName("render_scale_percent").c_str()),
                             25, 400);
    int scale = percent >= 100 ? (height * percent + 72000 - 1) / 72000  // ceil(h*p/100/720)
                               : (height * percent + 36000) / 72000;     // round(h*p/100/720)
    scale = std::clamp(scale, 1, 8);
    // X before Y: the Vulkan backend applies the change when Y is set.
    rex::cvar::SetFlagByName("resolution_scale", std::to_string(scale));
    rex::cvar::SetFlagByName("draw_resolution_scale_x", std::to_string(scale));
    rex::cvar::SetFlagByName("draw_resolution_scale_y", std::to_string(scale));
    REXLOG_INFO("Render: {}p x {}% -> internal scale {}x ({}x{})", height, percent, scale,
                1280 * scale, 720 * scale);
  }

  // A window (not fullscreen) is 16:9 at the chosen resolution's height, but never above 1080 nor taller than 90% of
  // the display's usable area (2026-11-23, the player: at 4K the window covered the whole screen; the frame is still
  // rendered at 4K and scaled to the window). Centred on its display.
  static void FitWindow() {
#if defined(__ANDROID__)
    return;  // the phone's window is the screen
#endif
    if (rex::cvar::GetFlagByName("fullscreen") == "true") return;
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (!windows) return;
    if (count > 0) {
      SDL_Window* w = windows[0];
      int height = std::min(ResolutionHeight(rex::cvar::GetFlagByName("resolution")), 1080);
      SDL_Rect usable{};
      if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(w), &usable) && usable.h > 0)
        height = std::min(height, usable.h * 9 / 10);
      height = std::max(height, 360);
      SDL_SetWindowSize(w, height * 16 / 9, height);
      SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    SDL_free(windows);
  }

  // Resolution and scale changes while running: the Vulkan backend rebuilds
  // its scaled resources at the end of the frame (D3D12 needs a restart), and
  // a window (not fullscreen, which always covers the display) takes the new
  // size.
  void FollowResolutionChanges() {
    auto on_change = [this](std::string_view name, std::string_view) {
      ApplyRenderScale();
      if (name != "resolution") return;
      app_context().CallInUIThreadDeferred([] { FitWindow(); });
    };
    rex::cvar::RegisterChangeCallback("resolution", on_change);
    rex::cvar::RegisterChangeCallback("render_scale_percent", on_change);
    // Dev aid: NIER_SCALE_SWITCH=<seconds>|<percent>[;...] changes
    // render_scale_percent at those times, to test live changes unattended.
    if (const char* spec = std::getenv("NIER_SCALE_SWITCH"); spec && *spec) {
      std::thread([s = std::string(spec)] {
        auto start = std::chrono::steady_clock::now();
        for (size_t pos = 0; pos < s.size();) {
          size_t end = s.find(';', pos);
          std::string item = s.substr(pos, end - pos);
          pos = end == std::string::npos ? s.size() : end + 1;
          size_t bar = item.find('|');
          if (bar == std::string::npos) continue;
          std::this_thread::sleep_until(start + std::chrono::seconds(std::atoi(item.c_str())));
          REXLOG_INFO("NIER_SCALE_SWITCH: render_scale_percent = {}", item.substr(bar + 1));
          rex::cvar::SetFlagByName("render_scale_percent", item.substr(bar + 1));
        }
      }).detach();
    }
  }

  static int ResolutionHeight(const std::string& r) {
    if (r == "1080p") return 1080;
    if (r == "1440p") return 1440;
    if (r == "4k" || r == "2160p") return 2160;
    if (auto x = r.find('x'); x != std::string::npos) return std::max(720, std::atoi(r.c_str() + x + 1));
    return 720;
  }

 private:
  std::unique_ptr<nier::PcOptionsDialog> pc_options_;
  std::unique_ptr<nier::AchievementsDialog> achievements_;
  std::unique_ptr<nier::jukebox::JukeboxDialog> jukebox_;
  std::unique_ptr<nier::tutorials::TutorialDialog> tutorial_;
  std::unique_ptr<rex::ui::ImGuiDialog> title_cards_;  // title_carousel.h
  std::unique_ptr<nier::PerfOverlayDialog> perf_overlay_;
#if defined(_WIN32)
  std::unique_ptr<nier::setup::SetupWizard> setup_wizard_;
#endif
  std::unique_ptr<nier::touch::TouchDialog> touch_;
  rex::ui::ImGuiDrawer* perf_drawer_ = nullptr;
  std::filesystem::path config_path_;

 public:
  // void OnLoadXexImage(std::string& xex_image) override {}

  // Dev aid: NIER_DUMP_MEMORY=<file>|<seconds>[,<seconds>...] writes every
  // committed page of guest memory after each <seconds> as records [u32 guest
  // address][u32 size][data], to find game data structures (e.g. menu tables)
  // while a screen is open. Several times write <file>-<seconds>s.bin.
  // NIER_DUMP_RANGE=<hex lo>-<hex hi> limits the addresses (e.g. the image).
  void StartMemoryDumpTimer() {
#if defined(_WIN32)
    const char* spec = std::getenv("NIER_DUMP_MEMORY");
    if (!spec || !*spec) return;
    std::string s(spec);
    auto bar = s.rfind('|');
    std::string path = s.substr(0, bar);
    std::vector<int> times;
    // NIER_DUMP_MEMORY=<file>|key (2026-11-15, the trainer's search): Ctrl+F11 writes <file>-1.bin, -2.bin, ...
    const bool on_key = bar != std::string::npos && s.substr(bar + 1) == "key";
    if (on_key) {
      times.push_back(-1);
    } else if (bar == std::string::npos) {
      times.push_back(30);
    } else {
      for (const char* p = s.c_str() + bar + 1; *p;) {
        times.push_back(std::atoi(p));
        while (*p && *p != ',') ++p;
        if (*p == ',') ++p;
      }
    }
    uint64_t lo = 0x00010000, hi = 0xC0000000ull;
    if (const char* range = std::getenv("NIER_DUMP_RANGE"); range && std::strchr(range, '-')) {
      lo = std::strtoull(range, nullptr, 16) & ~0xFFFFull;
      hi = std::strtoull(std::strchr(range, '-') + 1, nullptr, 16);
    }
    uint8_t* base = runtime()->virtual_membase();
    std::thread([path, times, base, lo, hi, on_key] {
     auto start = std::chrono::steady_clock::now();
     int presses = 0;
     for (size_t round = 0; on_key || round < times.size(); ++round) {
      const int seconds = on_key ? 0 : times[round];
      size_t dot = path.rfind('.');
      if (dot == std::string::npos || dot < path.find_last_of("/\\") + 1) dot = path.size();
      std::string file;
      if (on_key) {
        // Wait for Ctrl+F11 (pressed, then released).
        while (!((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F11) & 0x8000)))
          std::this_thread::sleep_for(std::chrono::milliseconds(30));
        while (GetAsyncKeyState(VK_F11) & 0x8000) std::this_thread::sleep_for(std::chrono::milliseconds(30));
        file = path.substr(0, dot) + "-" + std::to_string(++presses) + path.substr(dot);
      } else {
        std::this_thread::sleep_until(start + std::chrono::seconds(seconds));
        file = times.size() == 1 ? path
                                 : path.substr(0, dot) + "-" + std::to_string(seconds) + "s" + path.substr(dot);
      }
      std::ofstream out(file, std::ios::binary);
      uint64_t total = 0;
      // 0xC0000000+ only mirrors the physical memory already seen at 0xA0000000.
      for (uint64_t a = lo; a < hi; a += 0x10000) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(base + a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mbi.Protect == 0) {
          continue;
        }
        uint32_t addr = uint32_t(a), size = 0x10000;
        // Some committed ranges cannot be handed to WriteFile directly; copy
        // through ReadProcessMemory, which fails safely, and skip what fails.
        static std::vector<char> chunk(0x10000);
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), base + a, chunk.data(), size, &got) ||
            got != size) {
          continue;
        }
        out.write(reinterpret_cast<const char*>(&addr), 4);
        out.write(reinterpret_cast<const char*>(&size), 4);
        out.write(chunk.data(), size);
        if (!out) {
          REXLOG_ERROR("Memory dump: write failed at {:08X} after {} MB", addr, total >> 20);
          break;
        }
        total += size;
      }
      out.close();
      REXLOG_INFO("Dumped {} MB of guest memory to {} (stream ok: {})", total >> 20, file,
                  !out.fail());
     }
    }).detach();
#endif
  }

  // Dev aid: NIER_DUMP_IMAGE=<file> writes the loaded guest image (the XEX is
  // compressed on disk) so tools/find-vcall-targets can scan it offline.
  void OnPostLoadXexImage() override {
    nier::ApplyMinimum3DResolution(runtime()->virtual_membase());
    nier::pc_options_state().guest_base = runtime()->virtual_membase();
    nier::text::SetRoot(config_path_.parent_path() / "TEXT");  // MODS is found next to it (before text::Init)
    nier::mods::Load(runtime()->virtual_membase());  // Nier Plus: code mods in MODS\<Mod>\ (mods.cpp)
    nier::dlc::InstallFrom(config_path_.parent_path() / "DLC");  // the player's DLC packages (dlc.h)
    const char* path = std::getenv("NIER_DUMP_IMAGE");
    if (!path || !*path) {
      return;
    }
    constexpr uint32_t kImageBase = 0x82000000;
    constexpr uint32_t kImageEnd = 0x86090000;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(runtime()->virtual_membase() + kImageBase),
              kImageEnd - kImageBase);
    REXLOG_INFO("Dumped guest image {:08X}-{:08X} to {}", kImageBase, kImageEnd, path);
  }
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // The SDK reads --game_data_root before it loads nier.toml, so a path saved
  // there by setup (an extracted folder or the .iso itself) would be ignored.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // NIER_CONFIG=<file>: use another nier.toml (measurement runs must not read
    // or overwrite the player's settings).
    if (const char* config = std::getenv("NIER_CONFIG"); config && *config) {
      paths.config_path = config;
    }
#if defined(__ANDROID__)
    // Everything in the app's external files directory (see the SDK's
    // SDL_main): nier.toml, and the game as Nier.iso or an extracted "game".
    std::filesystem::path android_files;
    if (const char* files = std::getenv("REX_ANDROID_FILES"); files && *files) {
      android_files = files;
      paths.config_path = android_files / "nier.toml";
    }
#endif
    config_path_ = paths.config_path;
    {  // The on-screen buttons' layout, next to nier.toml (touch_controls.h).
      auto& touch = nier::touch::state();
      std::lock_guard lock(touch.mutex);
      touch.file = config_path_.parent_path() / "touch_controls.txt";
      touch.loaded = false;
    }
    if (!paths.game_data_root.empty()) {
      return;
    }
    rex::cvar::LoadConfig(paths.config_path);
    std::string saved = rex::cvar::GetFlagByName("game_data_root");
    if (!saved.empty()) {
      paths.game_data_root = saved;
      return;
    }
#if defined(__ANDROID__)
    if (!android_files.empty()) {
      paths.game_data_root = std::filesystem::exists(android_files / "Nier.iso")
                                 ? android_files / "Nier.iso"
                                 : android_files / "game";
      return;
    }
#endif
    // Windows: the first-run window asks for the copy (OnFinalizePaths, setup_wizard.h).
  }
};
