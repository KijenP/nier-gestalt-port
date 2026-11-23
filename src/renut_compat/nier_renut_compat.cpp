// NieR: the SA2 port's app-side pieces Renut's renderer calls (src/renut_compat/*.h, copied from mgr-nx-deko), reduced
// to what a PC build of NieR needs: the enhancement registry (Renut registers its own switches through it), a fixed
// 720p render mode, no video recorder and no late-input read hook.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "cpu/sa2_enhancements.h"
#include "me/sa2_render_mode.h"
#include "rex/input/read_hook.h"
#include "sa2_capture.h"

namespace sa2::enh {
namespace {
std::vector<Enhancement*>& Registry() {
  static std::vector<Enhancement*> registry;
  return registry;
}
}  // namespace

bool Register(Enhancement* e) {
  Registry().push_back(e);
  return true;
}

void Resolve() {
  for (Enhancement* e : Registry())
    e->active = e->own();
}

bool CpuMaster() { return true; }
bool NonCpuMaster() { return true; }
void StartReport() {}
void VerifyCheck(uint64_t) {}
void VerifyDifference() {}
bool VerifyMode() { return false; }
void CodegenChecksInit() {}
void CodegenChecksReport(int) {}
void FormsSelfCheck() {}
void ApplyGpuBoost() {}
bool GuestCodeMatches(const uint8_t*, uint32_t, uint32_t, uint64_t, const char*) { return true; }

}  // namespace sa2::enh

namespace sa2::render {

uint32_t WantedScaleQuarters() { return 4; }
uint32_t ScaleQuartersOf(bool) { return 4; }
bool Docked() { return true; }
// The PC menu's resolution times its scale. Any height: one off Renut's table of scales uses its
// custom scale (render_scale.h). Below 720p ("180p", "360p", "640p") only the rendering is smaller;
// the window keeps at least 720 lines (nier_app.h).
int32_t RenderHeightOf(bool) {
  const std::string resolution = rex::cvar::GetFlagByName("resolution");
  int base = 720;
  if (resolution == "4k" || resolution == "2160p") base = 2160;
  else if (!resolution.empty() && std::atoi(resolution.c_str()) > 0) base = std::atoi(resolution.c_str());
  base = std::clamp(base, 90, 4320);
  const int percent = std::clamp(std::atoi(rex::cvar::GetFlagByName("render_scale_percent").c_str()), 10, 400);
  return std::max(base * percent / 100, 36);
}
void ToggleDocked() {}
void OnPresented(uint64_t) {}
uint32_t MsaaSamples() { return 1; }
void NoteRescale() {}
void NoteMsaaUse(uint64_t) {}
void RegisterKeys() {}

}  // namespace sa2::render

std::atomic<uint64_t> g_sa2_game_frames{0};

namespace sa2::capture {

void Start(std::function<rex::ui::Presenter*()>) {}
void StartWithSource(std::function<bool(rex::ui::RawImage&)>) {}
bool RenutLockstepOn() { return false; }
void RenutLockstepFrame(uint64_t, const rex::ui::RawImage*) {}
void Stop() {}
void BeforeFlip() {}

}  // namespace sa2::capture

namespace rex::input {

void SetReadHook(ReadHook) {}

}  // namespace rex::input

#if defined(_WIN32)
// Crash report: where a host access violation happens (the SDK's handler logs only the address read): the
// faulting instruction and the return addresses on the stack inside nier.exe, as offsets from its base, for
// llvm-symbolizer. First chance, logged once per thread, then left to the SDK.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace {
LONG CALLBACK NierRenutCrashProbe(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  // Only a guest null-pointer read (the guest's virtual memory starts at host 0x100000000): the SDK's own faults
  // (MMIO, write watches) are elsewhere.
  const uintptr_t address = info->ExceptionRecord->ExceptionInformation[1];
  // NIER_CRASH_AT=<guest hex>: also that one guest address (a crash seen in the log).
  static const uintptr_t extra = [] {
    const char* v = std::getenv("NIER_CRASH_AT");
    return v ? uintptr_t(0x100000000ull + std::strtoull(v, nullptr, 16)) : uintptr_t(0);
  }();
  if ((address < 0x100000000ull || address >= 0x100010000ull) && (!extra || address != extra))
    return EXCEPTION_CONTINUE_SEARCH;
  thread_local bool logged = false;
  if (logged)
    return EXCEPTION_CONTINUE_SEARCH;
  logged = true;
  const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
  const uintptr_t end = module + nt->OptionalHeader.SizeOfImage;
  const uintptr_t rip = info->ContextRecord->Rip;
  std::string trail;
  const auto* sp = reinterpret_cast<const uintptr_t*>(info->ContextRecord->Rsp);
  int found = 0;
  for (int i = 0; i < 512 && found < 12; ++i) {
    const uintptr_t v = sp[i];
    if (v >= module && v < end) {
      trail += fmt::format(" 0x{:X}", v - module);
      ++found;
    }
  }
  REXLOG_ERROR("[renut-crash] access violation at {} (exe+0x{:X}) reading 0x{:X}; stack:{}",
               rip >= module && rip < end ? "nier.exe" : "outside", rip - module,
               info->ExceptionRecord->ExceptionInformation[1], trail);
  return EXCEPTION_CONTINUE_SEARCH;
}
const bool g_crash_probe = [] {
  AddVectoredExceptionHandler(1, NierRenutCrashProbe);
  return true;
}();
}  // namespace
#endif
