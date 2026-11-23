// nier - reads the few things the first-run setup checks in the player's copy of the game, without extracting
// it: files out of an Xbox 360 disc image (XDVDFS/GDFX), the title ID from default.xex's XEX2 header, and the
// title tile (a PNG) from the disc's nxeart package. A C++ copy of tools/GameImage.cs (setup.ps1 uses that one).
#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace nier::disc {

constexpr uint32_t kNierTitleId = 0x535107E8;  // NieR Gestalt / NieR (Xbox 360)

inline uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// An opened disc image: the game partition's root directory table.
class Image {
 public:
  bool Open(const std::filesystem::path& path) {
    file_.open(path, std::ios::binary);
    if (!file_) return false;
    file_.seekg(0, std::ios::end);
    size_ = uint64_t(file_.tellg());
    // Byte offsets of the game partition in the known layouts (as ReXGlue's DiscImageDevice).
    for (uint64_t offset : {0x00000000ull, 0x0000FB20ull, 0x00020600ull, 0x02080000ull, 0x0FD90000ull}) {
      std::vector<uint8_t> head;
      if (!Read(offset + 32ull * kSector, 28, head) || std::memcmp(head.data(), "MICROSOFT*XBOX*MEDIA", 20) != 0)
        continue;
      const uint32_t root_sector = Le32(&head[20]), root_size = Le32(&head[24]);
      if (root_size < 13 || root_size > 32u * 1024 * 1024) return false;
      game_offset_ = offset;
      xbox_ = Read(offset + uint64_t(root_sector) * kSector, root_size, root_);
      return xbox_;
    }
    return false;
  }
  bool IsXboxImage() const { return xbox_; }

  // A file or folder by path from the root ("media/layer1.cpk"), case-insensitive.
  struct Entry {
    uint64_t sector = 0, size = 0;
    bool directory = false;
  };
  bool Find(std::string_view path, Entry& out) {
    std::vector<uint8_t> dir = root_;
    while (!path.empty()) {
      const size_t slash = path.find('/');
      const std::string_view name = path.substr(0, slash);
      if (!FindIn(dir, 0, name, out, 0)) return false;
      if (slash == std::string_view::npos) return true;
      path.remove_prefix(slash + 1);
      if (!out.directory || out.size > 32u * 1024 * 1024 ||
          !Read(game_offset_ + out.sector * kSector, size_t(out.size), dir))
        return false;
    }
    return false;
  }
  bool ReadFile(std::string_view path, std::vector<uint8_t>& out, uint64_t max_size = 64ull << 20) {
    Entry e;
    return Find(path, e) && !e.directory && e.size <= max_size &&
           Read(game_offset_ + e.sector * kSector, size_t(e.size), out);
  }

 private:
  static constexpr uint64_t kSector = 2048;

  bool Read(uint64_t offset, size_t count, std::vector<uint8_t>& out) {
    if (offset + count > size_) return false;
    out.resize(count);
    file_.clear();
    file_.seekg(std::streamoff(offset));
    file_.read(reinterpret_cast<char*>(out.data()), std::streamsize(count));
    return size_t(file_.gcount()) == count;
  }

  // Directory entries form a binary tree: left/right child offsets in 4-byte units.
  static bool FindIn(const std::vector<uint8_t>& dir, uint32_t ordinal, std::string_view name, Entry& out,
                     int depth) {
    const size_t p = size_t(ordinal) * 4;
    if (depth > 64 || p + 14 > dir.size()) return false;
    const uint16_t left = uint16_t(dir[p] | dir[p + 1] << 8), right = uint16_t(dir[p + 2] | dir[p + 3] << 8);
    const size_t len = dir[p + 13];
    if (p + 14 + len > dir.size()) return false;
    bool same = len == name.size();
    for (size_t i = 0; same && i < len; ++i)
      same = std::tolower(dir[p + 14 + i]) == std::tolower(static_cast<unsigned char>(name[i]));
    if (same) {
      out.sector = Le32(&dir[p + 4]);
      out.size = Le32(&dir[p + 8]);
      out.directory = (dir[p + 12] & 0x10) != 0;
      return true;
    }
    return (left != 0 && left != 0xFFFF && FindIn(dir, left, name, out, depth + 1)) ||
           (right != 0 && right != 0xFFFF && FindIn(dir, right, name, out, depth + 1));
  }

  std::ifstream file_;
  uint64_t size_ = 0, game_offset_ = 0;
  std::vector<uint8_t> root_;
  bool xbox_ = false;
};

// XEX2 (big-endian): optional header 0x00040006 is the execution info (media_id, version, base_version,
// title_id, ...). 0 when it is not a XEX2 or has none.
inline uint32_t XexTitleId(const std::vector<uint8_t>& xex) {
  if (xex.size() < 0x18 || std::memcmp(xex.data(), "XEX2", 4) != 0) return 0;
  const uint32_t count = Be32(&xex[0x14]);
  for (uint32_t i = 0; i < count && 0x18 + size_t(i) * 8 + 8 <= xex.size(); ++i) {
    const uint32_t key = Be32(&xex[0x18 + size_t(i) * 8]), value = Be32(&xex[0x18 + size_t(i) * 8 + 4]);
    if (key == 0x00040006 && size_t(value) + 16 <= xex.size()) return Be32(&xex[value + 12]);
  }
  return 0;
}

// nxeart is an STFS package (CON/LIVE/PIRS); its header carries the title tile as a PNG: size at 0x1712, data at
// 0x171A. Empty when absent.
inline std::vector<uint8_t> TileFromNxeArt(const std::vector<uint8_t>& stfs) {
  if (stfs.size() < 0x171A + 8) return {};
  if (std::memcmp(stfs.data(), "PIRS", 4) && std::memcmp(stfs.data(), "LIVE", 4) && std::memcmp(stfs.data(), "CON ", 4))
    return {};
  const uint32_t size = Be32(&stfs[0x1712]);
  if (size <= 8 || 0x171A + size_t(size) > stfs.size() || stfs[0x171A] != 0x89) return {};
  return std::vector<uint8_t>(stfs.begin() + 0x171A, stfs.begin() + 0x171A + size);
}

// A Windows .ico holding the PNG as is (one image; PNG icons work since Windows Vista).
inline std::vector<uint8_t> IcoFromPng(const std::vector<uint8_t>& png) {
  if (png.size() < 24) return {};
  const uint32_t w = Be32(&png[16]), h = Be32(&png[20]);
  std::vector<uint8_t> ico = {0, 0, 1, 0, 1, 0,
                              uint8_t(w >= 256 ? 0 : w), uint8_t(h >= 256 ? 0 : h), 0, 0, 1, 0, 32, 0};
  const uint32_t size = uint32_t(png.size()), offset = 22;
  for (uint32_t v : {size, offset})
    for (int i = 0; i < 4; ++i) ico.push_back(uint8_t(v >> (8 * i)));
  ico.insert(ico.end(), png.begin(), png.end());
  return ico;
}

}  // namespace nier::disc
