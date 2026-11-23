// nier - in-process CPU sampler (dev aid, Windows only)
//
// NIER_CPU_SAMPLE=<start s>|<duration s>|<report file>[|<thread name>]
// About 1000 times a second, suspends the named thread (default "GPU
// Commands"; "busiest" = the one using the most CPU at the start), unwinds its
// stack, and resumes it. The report lists functions by self time (where the
// thread was) and by inclusive time (on the stack), named from the PDBs next
// to the modules, or as module+offset without one (the recompiled game: see
// tools/cpu-sample-map.ps1).

#pragma once

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#endif

namespace nier {

#if defined(_WIN32)
namespace cpu_sampler_detail {

inline uint64_t ThreadCpuTime(HANDLE thread) {
  FILETIME creation, exit, kernel, user;
  if (!GetThreadTimes(thread, &creation, &exit, &kernel, &user)) return 0;
  return (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
         (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
}

inline std::string Describe(HANDLE thread) {
  PWSTR description = nullptr;
  std::string narrow;
  if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
    for (const wchar_t* c = description; *c; ++c) narrow += char(*c);
    LocalFree(description);
  }
  return narrow;
}

// The thread of this process that used the most CPU over a second (other than
// the caller), logging the busiest ones.
inline HANDLE FindBusiestThread() {
  std::vector<std::pair<DWORD, HANDLE>> threads;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
  THREADENTRY32 entry = {sizeof(entry)};
  DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_LIMITED_INFORMATION,
                               FALSE, entry.th32ThreadID);
    if (thread) threads.emplace_back(entry.th32ThreadID, thread);
  }
  CloseHandle(snapshot);
  std::vector<uint64_t> before(threads.size());
  for (size_t i = 0; i < threads.size(); ++i) before[i] = ThreadCpuTime(threads[i].second);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  std::vector<std::pair<uint64_t, size_t>> used;
  for (size_t i = 0; i < threads.size(); ++i) {
    used.emplace_back(ThreadCpuTime(threads[i].second) - before[i], i);
  }
  std::sort(used.rbegin(), used.rend());
  for (size_t i = 0; i < used.size() && i < 8; ++i) {
    REXLOG_INFO("CPU sampler: thread {} '{}' {:.0f}% of a core", threads[used[i].second].first,
                Describe(threads[used[i].second].second), used[i].first / 1.0e5);
  }
  HANDLE busiest = used.empty() ? nullptr : threads[used[0].second].second;
  for (auto& [id, thread] : threads) {
    if (thread != busiest) CloseHandle(thread);
  }
  return busiest;
}

inline HANDLE FindThreadByName(const std::wstring& name) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
  THREADENTRY32 entry = {sizeof(entry)};
  HANDLE found = nullptr;
  DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snapshot, &entry); ok && !found;
       ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid) continue;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_LIMITED_INFORMATION,
                               FALSE, entry.th32ThreadID);
    if (!thread) continue;
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
      std::wstring_view text(description);
      if (text.find(name) != std::wstring_view::npos) found = thread;
      std::string narrow;
      for (wchar_t c : text) narrow += char(c);
      REXLOG_INFO("CPU sampler: thread {} '{}'", entry.th32ThreadID, narrow);
      LocalFree(description);
    }
    if (!found) CloseHandle(thread);
  }
  CloseHandle(snapshot);
  return found;
}

constexpr int kMaxFrames = 48;

// Unwinds a suspended thread's stack without allocating (the thread may hold
// the heap lock).
inline int Unwind(CONTEXT context, DWORD64* frames) {
  int count = 0;
  while (count < kMaxFrames && context.Rip) {
    frames[count++] = context.Rip;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!function) {
      // Leaf function: the return address is on top of the stack.
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += 8;
    } else {
      PVOID handler_data = nullptr;
      DWORD64 establisher_frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                       &handler_data, &establisher_frame, nullptr);
    }
  }
  return count;
}

inline std::string Name(DWORD64 address) {
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 511;
  DWORD64 displacement = 0;
  if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
    return symbol->Name;
  }
  // No PDB (e.g. the recompiled game): module and offset, to look up in the
  // linker map (tools/cpu-sample-map.ps1).
  HMODULE module = nullptr;
  char module_name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module)) {
    GetModuleFileNameA(module, module_name, MAX_PATH);
    const char* slash = std::strrchr(module_name, '\\');
    char offset[32];
    std::snprintf(offset, sizeof(offset), "+0x%llX",
                  (unsigned long long)(address - reinterpret_cast<DWORD64>(module)));
    return std::string(slash ? slash + 1 : module_name) + offset;
  }
  return "?";
}

}  // namespace cpu_sampler_detail
#endif

inline void StartCpuSampler() {
#if defined(_WIN32)
  const char* spec = std::getenv("NIER_CPU_SAMPLE");
  if (!spec || !*spec) return;
  std::string s(spec);
  auto bar1 = s.find('|');
  auto bar2 = s.find('|', bar1 + 1);
  auto bar3 = s.find('|', bar2 + 1);
  if (bar1 == std::string::npos || bar2 == std::string::npos) return;
  int start_s = std::atoi(s.c_str());
  int duration_s = std::atoi(s.c_str() + bar1 + 1);
  std::string path = s.substr(bar2 + 1, bar3 == std::string::npos ? std::string::npos : bar3 - bar2 - 1);
  std::string thread_name = bar3 == std::string::npos ? "GPU Commands" : s.substr(bar3 + 1);
  std::thread([start_s, duration_s, path, thread_name] {
    using namespace cpu_sampler_detail;
    std::this_thread::sleep_for(std::chrono::seconds(start_s));
    HANDLE thread = thread_name == "busiest"
                        ? FindBusiestThread()
                        : FindThreadByName(std::wstring(thread_name.begin(), thread_name.end()));
    if (!thread) {
      REXLOG_ERROR("CPU sampler: thread '{}' not found", thread_name);
      return;
    }
    timeBeginPeriod(1);
    std::unordered_map<DWORD64, uint32_t> self_counts, inclusive_counts;
    // Whole stacks (the innermost frames), to see who calls what.
    constexpr int kStackFrames = 14;
    std::map<std::vector<DWORD64>, uint32_t> stack_counts;
    uint32_t samples = 0;
    DWORD64 frames[kMaxFrames];
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);
    while (std::chrono::steady_clock::now() < end) {
      if (SuspendThread(thread) != DWORD(-1)) {
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        int count = GetThreadContext(thread, &context) ? Unwind(context, frames) : 0;
        ResumeThread(thread);
        if (count) {
          ++samples;
          ++self_counts[frames[0]];
          // Count each return address once per sample (recursion).
          // Return addresses point after the call: step back into it.
          for (int i = 1; i < count; ++i) --frames[i];
          for (int i = 0; i < count; ++i) {
            bool seen = false;
            for (int j = 0; j < i && !seen; ++j) seen = frames[j] == frames[i];
            if (!seen) ++inclusive_counts[frames[i]];
          }
          ++stack_counts[std::vector<DWORD64>(frames, frames + std::min(count, kStackFrames))];
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    timeEndPeriod(1);
    CloseHandle(thread);

    // Name the addresses (by function, so different call sites merge).
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::map<std::string, uint32_t> self_by_name;
    for (auto& [address, n] : self_counts) self_by_name[Name(address)] += n;
    // Inclusive per name: a function can appear at several addresses of one
    // sample, so this slightly overcounts recursion through different sites.
    std::map<std::string, uint32_t> inclusive_by_name;
    for (auto& [address, n] : inclusive_counts) inclusive_by_name[Name(address)] += n;
    // Stacks by function names (call sites of one function merge).
    std::map<std::string, uint32_t> stacks_by_name;
    for (auto& [stack, n] : stack_counts) {
      std::string text;
      for (DWORD64 address : stack) text += Name(address) + " < ";
      stacks_by_name[text] += n;
    }
    SymCleanup(GetCurrentProcess());

    auto write_sorted = [&](std::ofstream& out, const std::map<std::string, uint32_t>& counts) {
      std::vector<std::pair<uint32_t, std::string>> sorted;
      for (auto& [name, n] : counts) sorted.emplace_back(n, name);
      std::sort(sorted.rbegin(), sorted.rend());
      for (size_t i = 0; i < sorted.size(); ++i) {
        out << (100.0 * sorted[i].first / std::max(samples, 1u)) << "%\t" << sorted[i].second
            << "\n";
      }
    };
    std::ofstream out(path);
    out << "thread " << thread_name << ", " << samples << " samples in " << duration_s << " s\n";
    out << "\n== self\n";
    write_sorted(out, self_by_name);
    out << "\n== inclusive\n";
    write_sorted(out, inclusive_by_name);
    out << "\n== stacks\n";
    write_sorted(out, stacks_by_name);
    REXLOG_INFO("CPU sampler: {} samples of '{}' written to {}", samples, thread_name, path);
  }).detach();
#endif
}

}  // namespace nier
