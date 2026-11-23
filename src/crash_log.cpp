/**
 * @file    crash_log.cpp
 * @brief   Android: a native crash written to the game's log (signal, address, registers, the call stack with each
 *          frame's library and offset), then handed on to the previous handler.
 *
 * NieR (2026-11-16): an Adreno 619 closed during start with nothing in the log, and the friends who test have no adb
 * for logcat's tombstone. One APK with this tells where. The SDK installs its own SIGSEGV/SIGILL handler for the
 * guest's memory later (exception_handler_posix.cpp); this one stays for the other signals and the start.
 */
#include "crash_log.h"

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <signal.h>
#include <ucontext.h>
#include <unwind.h>

#include <atomic>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>

namespace nier {
namespace {

struct sigaction g_old[32];
std::atomic<int> g_in_crash{0};
const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};

struct Walk {
  uintptr_t pcs[48];
  int n = 0;
};

_Unwind_Reason_Code Step(_Unwind_Context *ctx, void *arg) {
  auto *w = static_cast<Walk *>(arg);
  uintptr_t pc = _Unwind_GetIP(ctx);
  if (pc && w->n < 48)
    w->pcs[w->n++] = pc;
  return w->n < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

std::string Where(uintptr_t pc) {
  Dl_info info{};
  if (pc && dladdr(reinterpret_cast<void *>(pc), &info) && info.dli_fname) {
    const char *lib = std::strrchr(info.dli_fname, '/');
    lib = lib ? lib + 1 : info.dli_fname;
    std::string s = fmt::format("{}+0x{:X}", lib, pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
    if (info.dli_sname)
      s += fmt::format(" ({}+0x{:X})", info.dli_sname, pc - reinterpret_cast<uintptr_t>(info.dli_saddr));
    return s;
  }
  return fmt::format("0x{:X}", pc);
}

const char *Name(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV (memoria invalida)";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL (instrucao invalida)";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT (abort, muitas vezes do driver)";
    case SIGTRAP: return "SIGTRAP";
    default: return "?";
  }
}

void Handler(int sig, siginfo_t *info, void *uc_void) {
  if (g_in_crash.exchange(1) == 0) {
    auto *uc = static_cast<ucontext_t *>(uc_void);
    REXLOG_ERROR("[crash] ======== o jogo caiu: sinal {} {} codigo {} endereco 0x{:X}", sig, Name(sig),
                 info ? info->si_code : 0, info ? reinterpret_cast<uintptr_t>(info->si_addr) : 0);
#if defined(__aarch64__)
    const auto &m = uc->uc_mcontext;
    REXLOG_ERROR("[crash] pc {}", Where(m.pc));
    REXLOG_ERROR("[crash] lr {}", Where(m.regs[30]));
    REXLOG_ERROR("[crash] sp 0x{:X} x0 0x{:X} x1 0x{:X} x2 0x{:X} x3 0x{:X} x8 0x{:X} x19 0x{:X} x20 0x{:X}", m.sp,
                 m.regs[0], m.regs[1], m.regs[2], m.regs[3], m.regs[8], m.regs[19], m.regs[20]);
#else
    (void)uc;
#endif
    Walk w;
    _Unwind_Backtrace(Step, &w);
    for (int i = 0; i < w.n; ++i)
      REXLOG_ERROR("[crash] #{:02} {}", i, Where(w.pcs[i]));
    REXLOG_ERROR("[crash] ======== fim");
  }
  // The previous handler (Android's debuggerd writes the tombstone), then the default.
  const struct sigaction &old = g_old[sig];
  sigaction(sig, &old, nullptr);
  if (old.sa_flags & SA_SIGINFO) {
    if (old.sa_sigaction) {
      old.sa_sigaction(sig, info, uc_void);
      return;
    }
  } else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
    old.sa_handler(sig);
    return;
  }
  raise(sig);
}

}  // namespace

void InstallCrashLog() {
  static bool done = false;
  if (done)
    return;
  done = true;
  // Its own stack: a stack overflow still gets logged.
  static char stack[64 * 1024];
  stack_t ss{};
  ss.ss_sp = stack;
  ss.ss_size = sizeof(stack);
  sigaltstack(&ss, nullptr);
  struct sigaction sa{};
  sa.sa_sigaction = Handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  for (int sig : kSignals)
    sigaction(sig, &sa, &g_old[sig]);
  REXLOG_INFO("[crash] registro de crash ligado (sinais SEGV BUS ILL FPE ABRT TRAP)");
}

}  // namespace nier
#else
namespace nier {
void InstallCrashLog() {}
}  // namespace nier
#endif
