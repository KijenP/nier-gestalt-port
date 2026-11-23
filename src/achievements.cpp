// nier - native achievements.
//
// The game unlocks an achievement through XUserWriteAchievements (sub_8255C6F8: r3 = count, r4 =
// entries of {u32 user index, u32 id}), which sends message 0xB0008 to the XGI app; the SDK has no
// achievement system. The hook keeps each new id with its date in achievements.txt next to
// nier.toml ("<id> <unix seconds>" lines). The names, descriptions and points are the game's own,
// read from its XDBF at run time, so no text of the game is part of the port.

#include "achievements.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>

#include "game_text.h"
#include "pc_options.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../thirdparty/stb/stb_image.h"

extern "C" void __imp__sub_8255C6F8(PPCContext& ctx, uint8_t* base);  // XUserWriteAchievements

namespace nier::achievements {
namespace {

std::mutex g_mutex;
std::atomic<bool> g_sound{false};  // an unlock's sound, for the game thread (TakeSound)
std::map<uint32_t, int64_t> g_unlocked;  // id -> unix seconds
bool g_loaded = false;
Info g_notice;
bool g_has_notice = false;

std::filesystem::path StateFile() {
  const auto& config = nier::pc_options_state().config_path;
  if (!config.empty()) return config.parent_path() / "achievements.txt";
  return std::filesystem::path("achievements.txt");
}

void LoadLocked() {
  if (g_loaded) return;
  g_loaded = true;
  std::ifstream in(StateFile());
  uint32_t id = 0;
  int64_t when = 0;
  while (in >> id >> when) g_unlocked[id] = when;
}

void SaveLocked() {
  std::ofstream out(StateFile(), std::ios::trunc);
  for (const auto& [id, when] : g_unlocked) out << id << ' ' << when << '\n';
}

std::string Date(int64_t when) {
  std::time_t t = std::time_t(when);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
  return buf;
}

rex::system::XLanguage Language(int code) {
  switch (code ? code : nier::text::GameLanguageOverride()) {
    case 3: return rex::system::XLanguage::kFrench;
    case 4: return rex::system::XLanguage::kGerman;
    default: return rex::system::XLanguage::kEnglish;
  }
}

}  // namespace

std::vector<Info> List(int game_language) {
  std::vector<Info> list;
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return list;
  const rex::system::util::XdbfGameData db = kernel->title_xdbf();
  if (!db.is_valid()) return list;
  const rex::system::XLanguage language = db.GetExistingLanguage(Language(game_language));
  std::lock_guard lock(g_mutex);
  LoadLocked();
  for (const auto& e : db.GetAchievements()) {
    Info info;
    info.id = uint16_t(e.id);
    info.name = db.GetStringTableEntry(language, uint16_t(e.label_id));
    info.description = db.GetStringTableEntry(language, uint16_t(e.description_id));
    info.locked = db.GetStringTableEntry(language, uint16_t(e.unachieved_id));
    info.points = uint16_t(e.gamerscore);
    info.image_id = uint32_t(e.image_id);
    if (auto it = g_unlocked.find(info.id); it != g_unlocked.end()) {
      info.unlocked = true;
      info.when = Date(it->second);
    }
    list.push_back(std::move(info));
  }
  // TEMP (2026-11-20, the player: are the time-based achievements counted?): the whole list once, to a text file
  // next to achievements.txt - id, unlocked, points, name, description, the locked text.
  static bool dumped = false;
  if (!dumped && !list.empty()) {
    dumped = true;
    std::ofstream out(StateFile().parent_path() / "achievements_lista.txt", std::ios::trunc);
    for (const Info& a : list)
      out << a.id << '\t' << (a.unlocked ? "SIM" : "nao") << '\t' << a.points << '\t' << a.name << '\t'
          << a.description << '\t' << a.locked << '\n';
  }
  return list;
}

void Unlock(uint32_t id) {
  bool fresh = false;
  {
    std::lock_guard lock(g_mutex);
    LoadLocked();
    if (!g_unlocked.count(id)) {
      g_unlocked[id] = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      SaveLocked();
      fresh = true;
    }
  }
  if (!fresh) return;
  g_sound = true;
  std::string name;
  for (const Info& a : List()) {
    if (a.id == id) {
      name = a.name;
      std::lock_guard lock(g_mutex);
      g_notice = a;
      g_has_notice = true;
    }
  }
  REXLOG_INFO("Conquista desbloqueada: {} ({})", name.empty() ? "?" : name, id);
}

bool Icon(uint32_t image_id, int& width, int& height, std::vector<uint8_t>& rgba) {
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return false;
  const rex::system::util::XdbfGameData db = kernel->title_xdbf();
  if (!db.is_valid()) return false;
  const rex::system::util::XdbfBlock block =
      image_id ? db.GetEntry(rex::system::util::XdbfSection::kImage, image_id) : db.icon();
  if (!block) return false;
  int channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(block.buffer), int(block.size),
                                          &width, &height, &channels, 4);
  if (!pixels) return false;
  rgba.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
  stbi_image_free(pixels);
  return true;
}

bool TakeSound() { return g_sound.exchange(false); }

bool TakeNotice(Info& out) {
  std::lock_guard lock(g_mutex);
  if (!g_has_notice) return false;
  g_has_notice = false;
  out = g_notice;
  return true;
}

}  // namespace nier::achievements

// TEMP (2026-11-20, the boss time achievements): every request of the game's (sub_8213B660(manager, id): the ids it
// asks, before its queue and XUserWriteAchievements), with its caller.
extern "C" void __imp__sub_8213B660(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_8213B660) {
  REXLOG_INFO("[conquista] o jogo pediu {} (por {:08X})", ctx.r4.s32, uint32_t(ctx.lr));
  __imp__sub_8213B660(ctx, base);
}

REX_HOOK_RAW(sub_8255C6F8) {
  const uint32_t count = ctx.r3.u32, entries = ctx.r4.u32;
  if (entries && count > 0 && count < 1000) {
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* p = base + entries + i * 8 + 4;
      nier::achievements::Unlock(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]);
    }
  }
  __imp__sub_8255C6F8(ctx, base);
}
