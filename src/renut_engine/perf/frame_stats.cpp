// banjo-nx: the frame statistics Renut's renderer reports into (renut_engine/perf/frame_stats.h, copied from Renut by
// tools/renut_gpu_import.py). Renut's own frame_stats.cpp also hooks the game's main loop (appMainTickPreDraw /
// appMainDraw), which banjo-nx's FPS code hooks already, so this keeps the numbers without those hooks: a game frame is
// one D3DDevice_Swap, its length the time between two.
#include "renut_engine/perf/frame_stats.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <rex/platform.h>

// Counter 0 of the console profiler: presented frames, which the Status Monitor overlay's FPS (SaltyNX) and the
// profiler's "game N fps" are made from (sdk/src/ui/switch_perf.cpp; a stub on the PC, me_pc_stubs.cpp).
#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchPerfCount(unsigned id);
#endif

#include "profiler/profiler.h"                      // the on-screen profiler's GPU, present and render thread rows
#include "renut_engine/gpu/banjo_renut_tuning.h"  // the [renut] report and its in-run A/B

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cmath>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(nier_adpf, true, "NieR",
                    "Android (2026-11-19, heat): tell the system each frame how long the game thread and the render "
                    "thread worked against the frame's time (the performance hint API, Android 13 and later), so it "
                    "can run their cores slower when they have time to spare. Off: the system's own guess, as before");

extern std::atomic<double> g_nier_frame_work_ms;  // the game thread's work, Swap to Swap (sa2_renut_pacing.cpp)
#endif

namespace renut::frame_stats {

namespace {

#if defined(__ANDROID__)
// NieR (2026-11-19, the Poco at 30 fps: skin 44 C with the cores far from busy). The performance hint session: the
// game thread and the render thread, the target the frame's time with a margin, each frame the longer of their two
// works. Loaded by name from libandroid (API 33; minSdk is 30), nothing where it is missing.
namespace adpf {
using GetManager = void *(*)();
using CreateSession = void *(*)(void *, const int32_t *, size_t, int64_t);
using Report = int (*)(void *, int64_t);
using UpdateTarget = int (*)(void *, int64_t);

struct Api {
  GetManager get_manager = nullptr;
  CreateSession create = nullptr;
  Report report = nullptr;
  UpdateTarget update = nullptr;
  bool ok = false;
};

const Api &Load() {
  static const Api api = [] {
    Api a;
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return a;
    a.get_manager = reinterpret_cast<GetManager>(dlsym(lib, "APerformanceHint_getManager"));
    a.create = reinterpret_cast<CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
    a.report = reinterpret_cast<Report>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    a.update = reinterpret_cast<UpdateTarget>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
    a.ok = a.get_manager && a.create && a.report && a.update;
    return a;
  }();
  return api;
}

std::atomic<int32_t> g_game_tid{0}, g_render_tid{0};
std::atomic<double> g_render_ms{0.0};
void *g_session = nullptr;  // game thread only
bool g_failed = false;
double g_period_ms = 0.0, g_target_ms = 0.0;
uint64_t g_reports = 0;

// The game thread, each frame ('period_ms' the time since the last one).
void GameFrame(double period_ms) {
  if (g_failed || !REXCVAR_GET(nier_adpf)) return;
  if (!g_game_tid.load(std::memory_order_relaxed)) g_game_tid = int32_t(gettid());
  if (period_ms <= 0.0 || period_ms > 200.0) return;  // a load, a pause: not a frame's time
  g_period_ms = g_period_ms <= 0.0 ? period_ms : g_period_ms * 0.95 + period_ms * 0.05;
  const Api &api = Load();
  if (!g_session) {
    const int32_t render = g_render_tid.load(std::memory_order_relaxed);
    if (!render) return;
    if (!api.ok) {
      g_failed = true;
      REXLOG_INFO("[adpf] performance hint API missing (Android below 13): the system's own clocks");
      return;
    }
    void *manager = api.get_manager();
    const int32_t tids[2] = {g_game_tid.load(), render};
    g_target_ms = g_period_ms * 0.8;
    g_session = manager ? api.create(manager, tids, 2, int64_t(g_target_ms * 1e6)) : nullptr;
    if (!g_session) {
      g_failed = true;
      REXLOG_INFO("[adpf] no performance hint session (the system declined): the system's own clocks");
      return;
    }
    REXLOG_INFO("[adpf] performance hint session: game thread {} and render thread {}, target {:.1f} ms", tids[0],
                tids[1], g_target_ms);
  }
  // The frame's time moved (another frame rate limit): the target follows.
  const double want = g_period_ms * 0.8;
  if (std::fabs(want - g_target_ms) > g_target_ms * 0.15) {
    g_target_ms = want;
    api.update(g_session, int64_t(g_target_ms * 1e6));
    REXLOG_INFO("[adpf] target {:.1f} ms", g_target_ms);
  }
  const double work = std::max(g_nier_frame_work_ms.load(std::memory_order_relaxed),
                               g_render_ms.load(std::memory_order_relaxed));
  if (work > 0.0) {
    api.report(g_session, int64_t(work * 1e6));
    if (++g_reports % 3000 == 1) REXLOG_INFO("[adpf] reporting: {:.1f} ms of work against {:.1f} ms", work, g_target_ms);
  }
}

void RenderFrame(double busy_ms) {
  if (!g_render_tid.load(std::memory_order_relaxed)) g_render_tid = int32_t(gettid());
  g_render_ms.store(busy_ms, std::memory_order_relaxed);
}
}  // namespace adpf
#endif

using Clock = std::chrono::steady_clock;

struct State {
  std::mutex mutex;
  Snapshot snap;
  Clock::time_point last_frame{};
  Clock::time_point window_start{};
  int window_frames = 0;
  double window_worst = 0.0;
  double gpu_sum = 0.0, render_sum = 0.0, wait_sum = 0.0;
  int gpu_n = 0, render_n = 0;
};

State& S() {
  static State s;
  return s;
}

}  // namespace

void OnGameFrame() {
  banjo::renut_gpu::StatsGameFrame();
  State& s = S();
  std::lock_guard lock(s.mutex);
  const auto now = Clock::now();
  if (s.last_frame != Clock::time_point{}) {
    const double ms = std::chrono::duration<double, std::milli>(now - s.last_frame).count();
#if defined(__ANDROID__)
    adpf::GameFrame(ms);
#endif
    Snapshot& snap = s.snap;
    if (snap.history_count < Snapshot::kHistory) {
      snap.history[snap.history_count++] = float(ms);
    } else {
      std::copy(snap.history + 1, snap.history + Snapshot::kHistory, snap.history);
      snap.history[Snapshot::kHistory - 1] = float(ms);
    }
    s.window_worst = std::max(s.window_worst, ms);
    ++s.window_frames;
  } else {
    s.window_start = now;
  }
  s.last_frame = now;
  const double window = std::chrono::duration<double>(now - s.window_start).count();
  if (window >= 1.0 && s.window_frames) {
    Snapshot& snap = s.snap;
    snap.fps = s.window_frames / window;
    snap.frame_ms = window * 1000.0 / s.window_frames;
    snap.worst_ms = s.window_worst;
    snap.cpu_ms = std::max(0.0, snap.frame_ms - s.wait_sum / s.window_frames);
    if (s.gpu_n) {
      snap.gpu_ms = s.gpu_sum / s.gpu_n;
      snap.gpu_known = true;
    }
    if (s.render_n) {
      snap.render_thread_ms = s.render_sum / s.render_n;
      snap.render_thread_known = true;
    }
    s.window_start = now;
    s.window_frames = 0;
    s.window_worst = 0.0;
    s.gpu_sum = s.render_sum = s.wait_sum = 0.0;
    s.gpu_n = s.render_n = 0;
  }
}

void OnGpuFrame(double ms) {
  banjo::renut_gpu::StatsGpuFrame(ms);
  banjo::profiler::SetGpuFrameMs(ms);  // the profiler window's "GPU Frame" (package 27: nobody fed it before)
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.gpu_sum += ms;
  ++s.gpu_n;
}

void OnRenderThreadFrame(double busy_ms) {
  banjo::renut_gpu::StatsRenderThreadFrame(busy_ms);
#if defined(__ANDROID__)
  adpf::RenderFrame(busy_ms);
#endif
  // The profiler window (package 27): the render thread's frame ends with its present, so this is also the time
  // between two host presents ("Present"); one thread, as RecordHostPresent needs.
  banjo::profiler::SetRenderThreadMs(busy_ms);
  banjo::profiler::RecordHostPresent();
  // Package 27c: the presented frame for the console's FPS. Only banjo-nx's own renderer (and the SDK's) counted
  // them, so with Renut's renderer the Status Monitor kept an old value (19.7) and the profile said "game 0.0 fps".
#if REX_PLATFORM_SWITCH
  RexSwitchPerfCount(0);
#endif
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.render_sum += busy_ms;
  ++s.render_n;
}

void OnGameThreadWait(double ms) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.wait_sum += ms;
}

Snapshot Get() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  return s.snap;
}

}  // namespace renut::frame_stats
