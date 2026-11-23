// nier - downloadable content: see dlc.h.

#include "dlc.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <string>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

namespace nier::dlc {
namespace {

constexpr uint32_t kTitleId = 0x535107E8;

uint32_t Be32(const unsigned char* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// A content package of this game: the STFS magic, and the title id at 0x360 of its header.
bool IsPackage(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  unsigned char head[0x364] = {};
  if (!in.read(reinterpret_cast<char*>(head), sizeof(head))) return false;
  const bool magic = !std::memcmp(head, "LIVE", 4) || !std::memcmp(head, "PIRS", 4) || !std::memcmp(head, "CON ", 4);
  return magic && Be32(head + 0x360) == kTitleId;
}

}  // namespace

void InstallFrom(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return;
  auto* kernel = REX_KERNEL_STATE();
  auto* content = kernel ? kernel->content_manager() : nullptr;
  if (!content) return;

  std::set<std::string> installed;
  for (const auto& data : content->ListContent(uint32_t(rex::system::xam::DummyDeviceId::HDD), 0,
                                               rex::system::XContentType::kMarketplaceContent, kTitleId)) {
    installed.insert(data.file_name());
  }

  for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec) || !IsPackage(it->path())) continue;
    const std::string name = it->path().filename().string();
    if (installed.count(name)) {
      REXLOG_INFO("[dlc] {} ja instalada", name);
      continue;
    }
    REXLOG_INFO("[dlc] instalando {} ...", it->path().string());
    const auto result = content->InstallContent(it->path());
    if (result == 0) {
      installed.insert(name);
      REXLOG_INFO("[dlc] {} instalada", name);
    } else {
      REXLOG_WARN("[dlc] {} falhou ({:#x})", name, uint32_t(result));
    }
  }
}

}  // namespace nier::dlc
