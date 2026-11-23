// nier - code changes applied to the recompiled game
//
// Recompiled functions are weak symbols, so defining one here replaces it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

REXCVAR_DEFINE_BOOL(fps_60, false, "Nier", "Unlock 60 fps gameplay (experimental)");
REXCVAR_DEFINE_INT32(dynamic_resolution_level, -1, "Nier",
                     "3D resolution: -1 = automatic (the game's own choice), 0 = 1280x720, "
                     "1 = 1024x576, 2 = 768x432");
REXCVAR_DEFINE_INT32(min_3d_resolution_percent, 60, "Nier",
                     "Size of the lowest 3D resolution level (dynamic_resolution_level 2) in "
                     "percent of 1280x720: 60 = 768x432 (the game's own), 50 = 640x360, "
                     "40 = 512x288. Read at startup.")
    .range(25, 80);

REXCVAR_DEFINE_DOUBLE(lod_distance_scale, 1.0, "Nier",
                      "Multiplies the distances where models with detail levels switch to "
                      "their less detailed versions")
    .range(0.25, 8.0);

extern "C" void __imp__sub_8256BB90(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82191D60(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82594EB0(PPCContext& ctx, uint8_t* base);

// 60 FPS (ported from xenia-canary/game-patches "535107E8 - Nier", which NOPs
// the instruction at 0x8256BB90). sub_8256BB90 is a one-instruction setter,
// `stw r4,13804(r3)`, storing the presentation interval the renderer waits
// for; skipping the store leaves the per-vblank default.
REX_HOOK_RAW(sub_8256BB90) {
  if (!REXCVAR_GET(fps_60)) {
    __imp__sub_8256BB90(ctx, base);
  }
}

// Model LOD. When gathering a model's meshes to draw (sub_8259BF48), the game
// picks each mesh group's detail level by comparing its distance with
// sub_82594EB0(0) and (1) (7 and 12 m, set by the area, scaled by the field
// of view; sub_8259BE48). The getter is only read there. Few models have
// mesh groups (characters don't).
REX_HOOK_RAW(sub_82594EB0) {
  __imp__sub_82594EB0(ctx, base);
  ctx.f1.f64 = float(ctx.f1.f64 * REXCVAR_GET(lod_distance_scale));
}

// TEMP NIER_MESHLOG=1: when the number of meshes sub_8259BF48 gathers for a
// model (r3) changes, log it with the model and the forced level argument.
extern "C" void __imp__sub_8259BF48(PPCContext& ctx, uint8_t* base);
// TEMP NIER_MESHLOG_DUMP=<hex model>,<n>,<file prefix>: on that model's n-th
// "hidden" and the "shown again" after it, write guest memory (the
// NIER_DUMP_MEMORY record format) to <prefix>-hidden.bin / -shown.bin, from
// the thread that noticed it, so the snapshot is of that moment.
static void DumpGuestMemory(uint8_t* base, const std::string& file) {
#if defined(_WIN32)
  std::FILE* out = std::fopen(file.c_str(), "wb");
  if (!out) {
    return;
  }
  static std::vector<char> chunk(0x10000);
  uint64_t total = 0;
  for (uint64_t a = 0x00010000; a < 0xC0000000ull; a += 0x10000) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mbi.Protect == 0) {
      continue;
    }
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), base + a, chunk.data(), 0x10000, &got) ||
        got != 0x10000) {
      continue;
    }
    uint32_t header[2] = {uint32_t(a), 0x10000};
    std::fwrite(header, 4, 2, out);
    std::fwrite(chunk.data(), 1, 0x10000, out);
    total += 0x10000;
  }
  std::fclose(out);
  REXLOG_INFO("MESHLOG dumped {} MB to {}", total >> 20, file);
#endif
}

static uint8_t* g_mesh_base = nullptr;
static uint32_t g_dump_model = 0, g_dump_at = 0, g_hidden_events = 0;
static int g_dump_stage = 0;  // 0 waiting for the n-th hidden, 1 waiting for shown, 2 done
static std::string g_dump_prefix;

static std::mutex g_mesh_mutex;
static uint32_t g_mesh_frame = 0;
static std::map<uint32_t, std::pair<uint32_t, bool>> g_mesh_seen;  // model -> {last frame, hidden}
REX_HOOK_RAW(sub_8259BF48) {
  uint32_t model = ctx.r3.u32;
  uint32_t pass = ctx.r10.u32;
  uint32_t groups = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + model + 48));
  uint32_t always = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + model + 40));
  __imp__sub_8259BF48(ctx, base);
  static const bool log = std::getenv("NIER_MESHLOG") != nullptr;
  if (!log) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mesh_mutex);
  static std::map<uint64_t, uint32_t> last;
  uint64_t key = (uint64_t(model) << 8) | (pass & 0xFF);
  auto it = last.find(key);
  if (it == last.end() || it->second != ctx.r3.u32) {
    REXLOG_INFO("MESHLOG model={:08X} pass={} always={} groups={} meshes {} -> {}", model, pass,
                always, groups, it == last.end() ? -1 : int(it->second), ctx.r3.u32);
    last[key] = ctx.r3.u32;
  }
  if (pass == 0) {
    auto& seen = g_mesh_seen[model];
    if (seen.second) {
      REXLOG_INFO("MESHLOG model={:08X} shown again after {} frames", model,
                  g_mesh_frame - seen.first);
      if (model == g_dump_model && g_dump_stage == 1) {
        DumpGuestMemory(base, g_dump_prefix + "-shown.bin");
        g_dump_stage = 2;
      }
    }
    seen = {g_mesh_frame, false};
  }
}

// Once a frame (from the dynamic resolution hook): models not gathered for
// 10 frames are logged as hidden.
void MeshLogFrame() {
  std::lock_guard<std::mutex> lock(g_mesh_mutex);
  ++g_mesh_frame;
  for (auto& [model, seen] : g_mesh_seen) {
    if (!seen.second && g_mesh_frame - seen.first > 10) {
      REXLOG_INFO("MESHLOG model={:08X} hidden", model);
      seen.second = true;
      if (model == g_dump_model && g_dump_stage == 0 && ++g_hidden_events == g_dump_at) {
        DumpGuestMemory(g_mesh_base, g_dump_prefix + "-hidden.bin");
        g_dump_stage = 1;
      }
    }
  }
}

// TEMP NIER_ENTLOG=<hex entity>: every 15th distance update of that entity
// (sub_821AEE40), its child, world matrix position, logic position and
// distance to the camera.
extern "C" void __imp__sub_821AEE40(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_821AEE40) {
  uint32_t entity = ctx.r3.u32;
  __imp__sub_821AEE40(ctx, base);
  static const uint32_t watched = [] {
    const char* env = std::getenv("NIER_ENTLOG");
    return env ? uint32_t(std::strtoul(env, nullptr, 16)) : 0u;
  }();
  if (!watched || entity != watched) {
    return;
  }
  static uint32_t n = 0;
  if (n++ % 15) {
    return;
  }
  auto f = [&](uint32_t off) {
    uint32_t bits = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + entity + off));
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
  };
  auto u = [&](uint32_t off) {
    return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + entity + off));
  };
  REXLOG_INFO(
      "ENTLOG n={} child={:08X} mat=({:.2f},{:.2f},{:.2f}) pos=({:.2f},{:.2f},{:.2f}) cam={:.2f} "
      "anim={:08X} w58={:X} wC0={:X}",
      n, u(0x24), f(0x90), f(0x94), f(0x98), f(0x750), f(0x754), f(0x758), f(2040), u(0x7C4),
      u(0x58), u(0xC0));
}

namespace {

uint32_t LoadGuestU32(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

void StoreGuestU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

float LoadGuestF32(uint8_t* base, uint32_t address) {
  uint32_t bits = LoadGuestU32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// The game's own section timer (the object at [0x84841AF8]): the length in
// seconds of the named section in the last finished frame, as sub_825A8660
// returns it.
float SectionSeconds(uint8_t* base, const char* name) {
  uint32_t timer = LoadGuestU32(base, 0x84841AF8);
  if (!timer) {
    return 0.0f;
  }
  uint32_t frame = timer + 80 + LoadGuestU32(base, timer + 76) * 41088;
  int32_t threads = int32_t(LoadGuestU32(base, timer + 36));
  for (int32_t t = 0; t < threads; ++t) {
    uint32_t thread = frame + t * 5136;
    int32_t sections = int32_t(LoadGuestU32(base, thread));
    for (int32_t i = 0; i < sections; ++i) {
      uint32_t section = thread + 16 + i * 80;
      if (!std::strcmp(reinterpret_cast<const char*>(base + section), name)) {
        return LoadGuestF32(base, section + 68) - LoadGuestF32(base, section + 64);
      }
    }
  }
  return 0.0f;
}

}  // namespace

// The last frame's work in ms, without the frame limiter's wait (Renut's pacing writes it;
// negative: unknown, as with Xenos).
std::atomic<double> g_nier_frame_work_ms{-1.0};

namespace {

// NieR (2026-11-05): "Automatic" 3D sharpness that follows the frame rate the player chose. The game's own rule drops
// a level after 31 frames over a 31.2 ms GPU budget (its 30 fps), so at 60 fps or more it never lowered it, and it
// never raised it back for a lower target. Here, every half second: when the frames come slower than the target
// (more than 6% late), one level lighter; when the work of a frame (without the limiter's wait) leaves a third of the
// target free, one level sharper, unless that level was too heavy in the last 8 seconds.
int32_t AutomaticLevel() {
  using Clock = std::chrono::steady_clock;
  static int32_t level = 0;
  static Clock::time_point last{}, window_start{}, dropped_at{};
  static int32_t dropped_from = -1;
  static uint32_t frames = 0;
  static double work_sum = 0.0, target_ms = 1000.0 / 60.0;
  const auto now = Clock::now();
  static double longest_ms = 0.0;
  if (last == Clock::time_point{}) {
    last = window_start = now;
    return level;
  }
  longest_ms = std::max(longest_ms, std::chrono::duration<double, std::milli>(now - last).count());
  last = now;
  ++frames;
  const double work = g_nier_frame_work_ms.load(std::memory_order_relaxed);
  work_sum += work;
  const double window_ms = std::chrono::duration<double, std::milli>(now - window_start).count();
  if (window_ms < 500.0) {
    return level;
  }
  // The target: the frame rate limit (0, unlimited: 60), at most 30 without fps_60.
  int limit = std::atoi(rex::cvar::GetFlagByName("frame_rate_limit").c_str());
  if (limit <= 0) limit = 60;
  if (!REXCVAR_GET(fps_60)) limit = std::min(limit, 30);
  target_ms = 1000.0 / limit;
  const double frame_ms = window_ms / frames;
  const double work_ms = work >= 0.0 ? work_sum / frames : frame_ms;
  const int32_t before = level;
  if (longest_ms > 250.0) {
    // A loading screen's or a menu's stall: says nothing about the 3D's cost.
  } else if (frame_ms > target_ms * 1.06 && level < 2) {
    dropped_from = level;
    dropped_at = now;
    ++level;
  } else if (level > 0 && work_ms < target_ms * 0.66 &&
             !(level - 1 == dropped_from && now - dropped_at < std::chrono::seconds(8))) {
    --level;
  }
  static const bool log = std::getenv("NIER_DYNRES_LOG") != nullptr;
  if (log && level != before) {
    REXLOG_INFO("DYNRES auto: level {} -> {} (frame {:.1f} ms, work {:.1f} ms, target {:.1f} ms)", before, level,
                frame_ms, work_ms, target_ms);
  }
  frames = 0;
  work_sum = 0.0;
  longest_ms = 0.0;
  window_start = now;
  return level;
}

}  // namespace

namespace nier {

// The 3D resolution levels are 1280x720 times the floats {1.0, 0.8, 0.6} at
// 0x8209E894; the renderer builds its three sets of 3D render targets from
// them once (sub_825928B8 / sub_82592958). Changing the last one before that
// gives the lowest level another size, below what the game offers.
void ApplyMinimum3DResolution(uint8_t* base) {
  constexpr uint32_t kLevelScales = 0x8209E894;
  if (LoadGuestF32(base, kLevelScales) != 1.0f || LoadGuestF32(base, kLevelScales + 8) != 0.6f) {
    REXLOG_WARN("3D resolution level table not found; min_3d_resolution_percent ignored");
    return;
  }
  int32_t percent = REXCVAR_GET(min_3d_resolution_percent);
  if (percent == 60) {
    return;
  }
  float scale = float(percent) / 100.0f;
  uint32_t bits;
  std::memcpy(&bits, &scale, sizeof(bits));
  // Read-only data: open the page for this one store.
  rex::memory::PageAccess old_access;
  if (!rex::memory::Protect(base + kLevelScales + 8, 4, rex::memory::PageAccess::kReadWrite,
                            &old_access)) {
    REXLOG_WARN("3D resolution level table not writable; min_3d_resolution_percent ignored");
    return;
  }
  StoreGuestU32(base, kLevelScales + 8, bits);
  rex::memory::Protect(base + kLevelScales + 8, 4, old_access);
  REXLOG_INFO("Lowest 3D resolution level: {}x{} ({}%)", int(1280 * scale), int(720 * scale),
              percent);
}

}  // namespace nier

// Dynamic resolution. Once a frame the game compares the "GPU" section time
// with a 31.2 ms budget. After 31 frames in a row over it (and the GPU slower
// than the "Main" CPU section) it drops the 3D resolution one level; after 31
// frames under it with enough slack it raises it again. Levels: 0 = 1280x720,
// 1 = 1024x576, 2 = 768x432. The state is {level, frames over, frames under}
// at 0x848416E8; the renderer reads the level at 0x827CE7E4.
// dynamic_resolution_level fixes the level instead.
// NIER_DYNRES_LOG=1 (dev aid) logs the measured times every 120 frames and
// every level change.
REX_HOOK_RAW(sub_82191D60) {
  constexpr uint32_t kState = 0x848416E8;
  constexpr uint32_t kRendererLevel = 0x827CE7E4;
  uint32_t level_before = LoadGuestU32(base, kState);
  __imp__sub_82191D60(ctx, base);
  static const bool mesh_log = [&] {
    if (const char* d = std::getenv("NIER_MESHLOG_DUMP")) {
      // <hex model>,<n>,<prefix>
      std::string s(d);
      size_t c1 = s.find(','), c2 = s.find(',', c1 + 1);
      g_dump_model = uint32_t(std::strtoul(s.substr(0, c1).c_str(), nullptr, 16));
      g_dump_at = uint32_t(std::atoi(s.substr(c1 + 1, c2 - c1 - 1).c_str()));
      g_dump_prefix = s.substr(c2 + 1);
    }
    g_mesh_base = base;
    return std::getenv("NIER_MESHLOG") != nullptr;
  }();
  if (mesh_log) {
    MeshLogFrame();
  }
  int32_t forced = REXCVAR_GET(dynamic_resolution_level);
  if (forced < 0) {
    forced = AutomaticLevel();  // the game's own rule only knows its 30 fps budget
  }
  if (forced >= 0 && forced <= 2) {
    StoreGuestU32(base, kState, uint32_t(forced));
    StoreGuestU32(base, kState + 4, 0);
    StoreGuestU32(base, kState + 8, 0);
    StoreGuestU32(base, kRendererLevel, uint32_t(forced));
  }

  static const bool log = [] {
    const char* env = std::getenv("NIER_DYNRES_LOG");
    return env && *env && *env != '0';
  }();
  if (!log) {
    return;
  }
  static uint32_t frames = 0, over = 0, streak = 0, max_streak = 0;
  static double gpu_sum = 0, main_sum = 0, gpu_min = 1e9, gpu_max = 0;
  double gpu_ms = SectionSeconds(base, "GPU") * 1000.0;
  double main_ms = SectionSeconds(base, "Main") * 1000.0;
  ++frames;
  gpu_sum += gpu_ms;
  main_sum += main_ms;
  gpu_min = std::min(gpu_min, gpu_ms);
  gpu_max = std::max(gpu_max, gpu_ms);
  if (gpu_ms >= 31.167) {
    ++over;
    max_streak = std::max(max_streak, ++streak);
  } else {
    streak = 0;
  }
  uint32_t level_after = LoadGuestU32(base, kState);
  if (level_after != level_before && forced < 0) {
    REXLOG_INFO("DYNRES level {} -> {} (gpu {:.1f} ms, main {:.1f} ms)", level_before,
                level_after, gpu_ms, main_ms);
  }
  if (frames == 120) {
    REXLOG_INFO(
        "DYNRES level={} gpu avg={:.1f} min={:.1f} max={:.1f} ms, main avg={:.1f} ms, "
        "over budget {}/120 (longest run {})",
        level_after, gpu_sum / frames, gpu_min, gpu_max, main_sum / frames, over, max_streak);
    frames = over = max_streak = 0;
    gpu_sum = main_sum = 0;
    gpu_min = 1e9;
    gpu_max = 0;
  }
}

// The game's memory pools (2026-11-16): at start sub_82496830 makes them from a table at 0x826DA8D0 of 22
// (size, kind) pairs (kind 1: physical, through XPhysicalAlloc). Entries 12 and 15 are 5 MB (0x500000), the room a
// character's .MDP
// unpacks into: a bigger body (2B's 6.5 MB) had its header read and was dropped (the player then crashed reading
// 0x30). nier_model_pool_mb makes those pools bigger (0 = the game's 5 MB).
REXCVAR_DEFINE_INT32(nier_model_pool_mb, 16, "NieR/Mods",
                     "Size of the game's two 5 MB character model pools, in MB (0: the game's own). Lets bigger "
                     "body mods load (a player model's unpacked .MDP must fit). Takes effect after a restart.")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
extern "C" void __imp__sub_82496830(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_82496830) {
  constexpr uint32_t kTable = 0x826DA8D0, kEntries = 22;
  const int32_t mb = REXCVAR_GET(nier_model_pool_mb);
  std::string all;
  for (uint32_t i = 0; i < kEntries; ++i) {
    uint8_t* e = base + kTable + i * 8;
    const uint32_t size = (uint32_t(e[0]) << 24) | (uint32_t(e[1]) << 16) | (uint32_t(e[2]) << 8) | e[3];
    const uint32_t kind = (uint32_t(e[4]) << 24) | (uint32_t(e[5]) << 16) | (uint32_t(e[6]) << 8) | e[7];
    all += fmt::format(" {}:{:X}/{}", i, size, kind);
    if (mb > 5 && size == 0x500000u) {
      const uint32_t want = uint32_t(mb) << 20;
      e[0] = uint8_t(want >> 24); e[1] = uint8_t(want >> 16); e[2] = uint8_t(want >> 8); e[3] = uint8_t(want);
    }
  }
  REXLOG_INFO("Pools de memoria do jogo:{}{}", all, mb > 5 ? fmt::format(" (as de 5 MB agora com {} MB)", mb) : "");
  __imp__sub_82496830(ctx, base);
}

// The game's heap reports a failed allocation (DHeap::Malloc "MemoryOver!!") to a debug print that
// goes nowhere: it goes to the log, with the heap's name (+60) and the size asked for.
extern "C" void __imp__sub_82120950(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_82120950) {
  const uint32_t heap = ctx.r3.u32, size = ctx.r4.u32;
  char name[33] = {};
  if (heap) std::memcpy(name, base + heap + 60, 32);
  REXLOG_WARN("Game heap '{}' out of memory: {} bytes asked", name, size);
  __imp__sub_82120950(ctx, base);
}
