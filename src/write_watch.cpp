// Dev aid: NIER_WATCH_WRITE=<hex guest address>[,<hex>...] logs every guest
// write to these dwords, with the recompiled function that did it (found from
// the host instruction address through PPCFuncMappings) and the old and new
// values. The watched pages are made read-only; a write faults, the page is
// unprotected for one single-stepped instruction, then protected again.
// Slow on pages that are written often - for finding writers only.
// NIER_WATCH_READ=1: watch reads (and writes) instead - the pages get no
// access, and once a second the functions that touched the watched dwords are
// logged with their counts.

#include "write_watch.h"

#if defined(_WIN32)

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/func.h>

namespace nier {
namespace {

uint8_t* g_base = nullptr;
// Fixed slots (2026-11-16: armed while the game runs, from a file read again and again): the handler reads them
// without a lock.
constexpr int kWatchMax = 64;
std::atomic<uint32_t> g_addresses[kWatchMax];
std::atomic<int> g_address_count{0};
std::atomic<uint8_t*> g_pages[kWatchMax];
std::atomic<int> g_page_count{0};
bool WatchedPage(uint8_t* page) {
  for (int i = 0, n = g_page_count.load(); i < n; ++i)
    if (g_pages[i].load() == page) return true;
  return false;
}
// Host function start and guest address, sorted by the host start.
std::vector<std::pair<uintptr_t, uint32_t>> g_functions;
SIZE_T g_page_size = 4096;
bool g_read_mode = false;
DWORD g_protection = PAGE_READONLY;
std::mutex g_touch_mutex;
std::map<uint32_t, uint32_t> g_touches;  // guest function -> count, this second
auto g_touch_start = std::chrono::steady_clock::now();
thread_local uint8_t* t_reprotect_page = nullptr;
thread_local uint32_t t_pending_address = 0;
thread_local uint32_t t_pending_old = 0;
thread_local uint32_t t_pending_function = 0;

uint32_t ReadGuestU32(uint32_t address) {
  uint32_t value;
  std::memcpy(&value, g_base + address, sizeof(value));
  return _byteswap_ulong(value);
}

uint32_t GuestFunctionFor(uintptr_t host_address) {
  auto it = std::upper_bound(g_functions.begin(), g_functions.end(),
                             std::make_pair(host_address, UINT32_MAX));
  if (it == g_functions.begin()) {
    return 0;
  }
  return std::prev(it)->second;
}

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* pointers) {
  EXCEPTION_RECORD* record = pointers->ExceptionRecord;
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 &&
      (record->ExceptionInformation[0] == 1 || g_read_mode)) {
    auto* address = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
    auto* page = reinterpret_cast<uint8_t*>(uintptr_t(address) & ~uintptr_t(g_page_size - 1));
    if (!WatchedPage(page)) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    uint32_t guest = uint32_t(address - g_base);
    t_pending_address = 0;
    if (g_read_mode) {
      for (int k = 0, n = g_address_count.load(); k < n; ++k) {
        const uint32_t watched = g_addresses[k].load();
        if (guest + 16 > watched && guest < watched + 4) {
          std::lock_guard<std::mutex> lock(g_touch_mutex);
          ++g_touches[GuestFunctionFor(uintptr_t(record->ExceptionAddress))];
          auto now = std::chrono::steady_clock::now();
          if (now - g_touch_start >= std::chrono::seconds(1)) {
            std::string line;
            for (auto& [function, count] : g_touches) {
              char item[32];
              std::snprintf(item, sizeof(item), " sub_%08X:%u", function, count);
              line += item;
            }
            REXLOG_INFO("WATCHREAD{}", line);
            g_touches.clear();
            g_touch_start = now;
          }
          break;
        }
      }
    }
    for (int k = 0, n = g_read_mode ? 0 : g_address_count.load(); k < n; ++k) {
      const uint32_t watched = g_addresses[k].load();
      if (guest + 8 > watched && guest < watched + 4) {
        t_pending_address = watched;
        t_pending_old = ReadGuestU32(watched);
        t_pending_function = GuestFunctionFor(uintptr_t(record->ExceptionAddress));
        break;
      }
    }
    DWORD old_protect;
    VirtualProtect(page, g_page_size, PAGE_READWRITE, &old_protect);
    t_reprotect_page = page;
    pointers->ContextRecord->EFlags |= 0x100;  // Trap flag: single step.
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_reprotect_page) {
    if (t_pending_address) {
      uint32_t new_value = ReadGuestU32(t_pending_address);
      if (new_value != t_pending_old) {
        REXLOG_INFO("WATCH {:08X}: {:08X} -> {:08X} ({} -> {}) by sub_{:08X}", t_pending_address,
                    t_pending_old, new_value, int32_t(t_pending_old), int32_t(new_value),
                    t_pending_function);
      }
      t_pending_address = 0;
    }
    DWORD old_protect;
    VirtualProtect(t_reprotect_page, g_page_size, g_protection, &old_protect);
    t_reprotect_page = nullptr;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void StartWriteWatch(uint8_t* guest_base) {
  const char* spec = std::getenv("NIER_WATCH_WRITE");
  // 2026-11-20: no variable (a launcher's did not reach the game): a watch.txt next to the game (the working folder)
  // arms it the same, read again while the game runs ("file:" mode).
  static const char kFileSpec[] = "file:watch.txt";
  if ((!spec || !*spec) && GetFileAttributesA("watch.txt") != INVALID_FILE_ATTRIBUTES) spec = kFileSpec;
  if (!spec || !*spec || !guest_base) {
    return;
  }
  g_base = guest_base;
  const char* read_mode = std::getenv("NIER_WATCH_READ");
  g_read_mode = read_mode && *read_mode && *read_mode != '0';
  g_protection = g_read_mode ? PAGE_NOACCESS : PAGE_READONLY;
  SYSTEM_INFO system_info;
  GetSystemInfo(&system_info);
  g_page_size = system_info.dwPageSize;
  // "file:<path>" (2026-11-16): the addresses are read from that file once it appears (armed while running).
  const std::string spec_s(spec);
  const bool from_file = spec_s.rfind("file:", 0) == 0;
  // Adds addresses (hex, comma separated) and protects their pages; known ones are skipped.
  auto arm = [](const std::string& s) {
    size_t pos = 0;
    while (pos < s.size()) {
      size_t comma = s.find(',', pos);
      const uint32_t a = uint32_t(std::strtoul(s.substr(pos, comma - pos).c_str(), nullptr, 16)) & ~uint32_t(3);
      pos = comma == std::string::npos ? s.size() : comma + 1;
      bool known = !a;
      for (int k = 0, n = g_address_count.load(); k < n && !known; ++k) known = g_addresses[k].load() == a;
      if (known || g_address_count.load() >= kWatchMax) continue;
      g_addresses[g_address_count.load()] = a;
      g_address_count.fetch_add(1);
      auto* page = reinterpret_cast<uint8_t*>(uintptr_t(g_base + a) & ~uintptr_t(g_page_size - 1));
      REXLOG_INFO("WATCH armed for {:08X} (current {:08X})", a, ReadGuestU32(a));
      if (WatchedPage(page) || g_page_count.load() >= kWatchMax) continue;
      g_pages[g_page_count.load()] = page;
      g_page_count.fetch_add(1);
      DWORD old_protect;
      if (!VirtualProtect(page, g_page_size, g_protection, &old_protect))
        REXLOG_INFO("WATCH could not protect page {}", static_cast<void*>(page));
    }
  };
  for (const PPCFuncMapping* mapping = PPCFuncMappings; mapping->guest; ++mapping) {
    g_functions.emplace_back(reinterpret_cast<uintptr_t>(mapping->host), uint32_t(mapping->guest));
  }
  std::sort(g_functions.begin(), g_functions.end());
  AddVectoredExceptionHandler(1, WatchHandler);
  // NIER_WATCH_AFTER=<seconds>: arm later - memory the game commits after the
  // arming comes back read-write and is not watched.
  const char* after = std::getenv("NIER_WATCH_AFTER");
  int delay = after ? std::atoi(after) : 0;
  std::thread([delay, from_file, spec_s, arm] {
    std::this_thread::sleep_for(std::chrono::seconds(delay));
    if (!from_file) {
      arm(spec_s);
      return;
    }
    // The file is read again every 300 ms: new addresses in it are armed too.
    const std::string path = spec_s.substr(5);
    std::string last;
    for (;;) {
      if (std::FILE* f = std::fopen(path.c_str(), "r")) {
        char line[1024] = {};
        std::fgets(line, sizeof(line), f);
        std::fclose(f);
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (s != last) {
          last = s;
          arm(s);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
  }).detach();
}

}  // namespace nier

#else  // Windows only (vectored exception handler, single-stepping).

namespace nier {
void StartWriteWatch(uint8_t*) {}
}  // namespace nier

#endif
