// nier - NieR 2D art from the game data (see game_assets.h).
//
// layer1.cpk is a CRI CPK: a "CPK " header @UTF table gives TocOffset, the
// TOC @UTF table lists DirName/FileName/FileOffset/FileSize. The 2D packs are
// stored uncompressed. A .2DV is a "KPKy" container whose first part
// (*.gpr.vir) is a HEAP of "TX2D" records: name, a 0x40-byte texture header
// (Xenos fetch constant at +0x1C) and the texture's range in the .2DP.

#include "game_assets.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <string_view>

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace nier {
using rex::X_STATUS;
namespace {

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

class GuestFile {
 public:
  explicit GuestFile(std::string_view path) {
    auto* fs = REX_KERNEL_FS();
    rex::filesystem::Entry* entry = fs ? fs->ResolvePath(path) : nullptr;
    if (entry && entry->Open(rex::filesystem::FileAccess::kGenericRead, &file_) != X_STATUS_SUCCESS) {
      file_ = nullptr;
    }
  }
  ~GuestFile() {
    if (file_) file_->Destroy();
  }
  bool ok() const { return file_ != nullptr; }
  bool Read(uint64_t offset, size_t size, std::vector<uint8_t>& out) {
    out.resize(size);
    size_t got = 0;
    return file_ && file_->ReadSync(std::span<uint8_t>(out.data(), size), size_t(offset), &got) ==
                        X_STATUS_SUCCESS &&
           got == size;
  }

 private:
  rex::filesystem::File* file_ = nullptr;
};

// One @UTF table: rows of column name -> integer or string.
struct UtfRow {
  std::map<std::string, uint64_t> num;
  std::map<std::string, std::string> str;
};

std::vector<UtfRow> ParseUtf(std::vector<uint8_t> t) {
  std::vector<UtfRow> rows;
  if (t.size() < 32) return rows;
  if (std::memcmp(t.data(), "@UTF", 4) != 0) {  // masked table
    uint32_t m = 0x655F;
    for (uint8_t& b : t) {
      b ^= uint8_t(m);
      m *= 0x4115;
    }
    if (std::memcmp(t.data(), "@UTF", 4) != 0) return rows;
  }
  const uint8_t* d = t.data();
  size_t rows_o = Be32(d + 8) + 8, str_o = Be32(d + 12) + 8;
  uint32_t ncol = Be16(d + 24), row_len = Be16(d + 26), nrow = Be32(d + 28);
  auto str = [&](uint32_t off) {
    size_t s = str_o + off, e = s;
    while (e < t.size() && t[e]) ++e;
    return std::string(reinterpret_cast<const char*>(d + s), e - s);
  };
  // Reads a value of the column type at o; returns its size.
  auto read = [&](uint8_t type, size_t o, uint64_t& num, std::string& s) -> size_t {
    switch (type & 0x0F) {
      case 0: case 1: num = d[o]; return 1;
      case 2: case 3: num = Be16(d + o); return 2;
      case 4: case 5: num = Be32(d + o); return 4;
      case 6: case 7: num = Be64(d + o); return 8;
      case 8: num = Be32(d + o); return 4;
      case 0xA: s = str(Be32(d + o)); return 4;
      case 0xB: return 8;
    }
    return 0;
  };
  struct Col { uint8_t flags; std::string name; uint64_t num = 0; std::string s; };
  std::vector<Col> cols;
  size_t p = 32;
  for (uint32_t c = 0; c < ncol && p + 5 <= t.size(); ++c) {
    Col col{d[p], str(Be32(d + p + 1))};
    p += 5;
    if ((col.flags & 0xF0) == 0x30) p += read(col.flags, p, col.num, col.s);
    cols.push_back(std::move(col));
  }
  for (uint32_t r = 0; r < nrow; ++r) {
    size_t q = rows_o + size_t(r) * row_len;
    UtfRow row;
    for (const Col& col : cols) {
      uint64_t num = col.num;
      std::string s = col.s;
      if ((col.flags & 0xF0) == 0x50) {
        if (q + 8 > t.size()) break;
        q += read(col.flags, q, num, s);
      } else if ((col.flags & 0xF0) != 0x30) {
        continue;
      }
      if ((col.flags & 0x0F) == 0xA) row.str[col.name] = s;
      else row.num[col.name] = num;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

// An @UTF table preceded by a 16-byte chunk header ("CPK ", "TOC ").
std::vector<UtfRow> ReadChunkTable(GuestFile& f, uint64_t offset) {
  std::vector<uint8_t> head;
  if (!f.Read(offset, 16, head)) return {};
  uint64_t size = 0;
  std::memcpy(&size, head.data() + 8, 8);  // little-endian
  if (size == 0 || size > (64u << 20)) return {};
  std::vector<uint8_t> table;
  if (!f.Read(offset + 16, size_t(size), table)) return {};
  return ParseUtf(std::move(table));
}

int TiledOffset(int x, int y, int width, int log2_bpb) {
  int aligned_width = (width + 31) & ~31;
  int macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log2_bpb + 7);
  int micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpb;
  int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// One texture of a .2DP: DXT1 (0x12), DXT2/3 (0x13), DXT4/5 (0x14), 8888 (0x06).
bool Decode(const uint8_t* fetch, const std::vector<uint8_t>& data, size_t data_off,
            GameImage& out) {
  uint32_t f0 = Be32(fetch), f1 = Be32(fetch + 4), f2 = Be32(fetch + 8);
  int format = f1 & 0x3F, endian = (f1 >> 6) & 3;
  bool tiled = f0 & 0x80000000u;
  int pitch = int((f0 >> 22) & 0x1FF) * 32;
  int w = int(f2 & 0x1FFF) + 1, h = int((f2 >> 13) & 0x1FFF) + 1;
  bool dxt = format == 0x12 || format == 0x13 || format == 0x14;
  if (!dxt && format != 0x06) return false;
  int bpb = format == 0x12 ? 8 : (dxt ? 16 : 4);
  int log2 = bpb == 8 ? 3 : (bpb == 16 ? 4 : 2);
  int pitch_blocks = dxt ? std::max(pitch, w) / 4 : std::max(pitch, w);
  int bw = dxt ? (w + 3) / 4 : w, bh = dxt ? (h + 3) / 4 : h;
  out.width = w;
  out.height = h;
  out.rgba.assign(size_t(w) * h * 4, 0);
  uint8_t blk[16];
  for (int by = 0; by < bh; ++by) {
    for (int bx = 0; bx < bw; ++bx) {
      size_t src = data_off + size_t(tiled ? TiledOffset(bx, by, pitch_blocks, log2)
                                           : (by * pitch_blocks + bx) * bpb);
      if (src + bpb > data.size()) continue;
      std::memcpy(blk, data.data() + src, bpb);
      if (endian == 1) {
        for (int i = 0; i < bpb; i += 2) std::swap(blk[i], blk[i + 1]);
      } else if (endian == 2) {
        for (int i = 0; i < bpb; i += 4) std::reverse(blk + i, blk + i + 4);
      }
      if (!dxt) {  // stored ARGB
        uint8_t* q = &out.rgba[(size_t(by) * w + bx) * 4];
        q[0] = blk[1]; q[1] = blk[2]; q[2] = blk[3]; q[3] = blk[0];
        continue;
      }
      int co = bpb == 16 ? 8 : 0;
      int c[2] = {blk[co] | blk[co + 1] << 8, blk[co + 2] | blk[co + 3] << 8};
      int pal[4][4];
      for (int k = 0; k < 2; ++k) {
        pal[k][0] = ((c[k] >> 11) & 31) * 255 / 31;
        pal[k][1] = ((c[k] >> 5) & 63) * 255 / 63;
        pal[k][2] = (c[k] & 31) * 255 / 31;
        pal[k][3] = 255;
      }
      if (c[0] > c[1] || bpb == 16) {
        for (int k = 0; k < 3; ++k) {
          pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
          pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        }
        pal[2][3] = pal[3][3] = 255;
      } else {
        for (int k = 0; k < 3; ++k) {
          pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
          pal[3][k] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
      }
      uint32_t idx = blk[co + 4] | blk[co + 5] << 8 | blk[co + 6] << 16 | uint32_t(blk[co + 7]) << 24;
      int alpha[16];
      if (format == 0x13) {
        for (int i = 0; i < 16; ++i) alpha[i] = ((blk[i / 2] >> ((i & 1) * 4)) & 15) * 17;
      } else if (format == 0x14) {
        int ap[8] = {blk[0], blk[1]};
        if (ap[0] > ap[1]) {
          for (int i = 1; i < 7; ++i) ap[i + 1] = ((7 - i) * ap[0] + i * ap[1]) / 7;
        } else {
          for (int i = 1; i < 5; ++i) ap[i + 1] = ((5 - i) * ap[0] + i * ap[1]) / 5;
          ap[6] = 0;
          ap[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) alpha[i] = ap[(bits >> (3 * i)) & 7];
      } else {
        std::fill(alpha, alpha + 16, -1);
      }
      for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
          int x = bx * 4 + px, y = by * 4 + py;
          if (x >= w || y >= h) continue;
          int i = py * 4 + px, ci = (idx >> (2 * i)) & 3;
          uint8_t* q = &out.rgba[(size_t(y) * w + x) * 4];
          q[0] = uint8_t(pal[ci][0]);
          q[1] = uint8_t(pal[ci][1]);
          q[2] = uint8_t(pal[ci][2]);
          q[3] = uint8_t(alpha[i] >= 0 ? alpha[i] : pal[ci][3]);
        }
      }
    }
  }
  return true;
}

}  // namespace

std::vector<CpkFile> ListCpk(const std::string& guest_path) {
  std::vector<CpkFile> files;
  GuestFile cpk(guest_path);
  if (!cpk.ok()) return files;
  std::vector<UtfRow> header = ReadChunkTable(cpk, 0);
  if (header.empty() || !header[0].num.count("TocOffset")) return files;
  uint64_t toc = header[0].num["TocOffset"];
  uint64_t content = header[0].num.count("ContentOffset") ? header[0].num["ContentOffset"] : toc;
  uint64_t base = std::min(toc, content);
  for (UtfRow& row : ReadChunkTable(cpk, toc)) {
    CpkFile f;
    f.dir = row.str["DirName"];
    f.name = row.str["FileName"];
    f.offset = row.num["FileOffset"] + base;
    f.size = row.num["FileSize"];
    f.extract_size = row.num.count("ExtractSize") ? row.num["ExtractSize"] : f.size;
    files.push_back(std::move(f));
  }
  return files;
}

bool ReadCpkFile(const std::string& guest_path, const CpkFile& file, std::vector<uint8_t>& out) {
  if (file.size != file.extract_size) return false;  // compressed
  GuestFile cpk(guest_path);
  return cpk.ok() && cpk.Read(file.offset, size_t(file.size), out);
}

std::map<std::string, GameImage> LoadGameImages(
    const std::map<std::string, std::vector<std::string>>& wanted) {
  std::map<std::string, GameImage> images;
  GuestFile cpk("game:\\media\\layer1.cpk");
  if (!cpk.ok()) {
    REXLOG_WARN("Game art: layer1.cpk not found");
    return images;
  }
  std::vector<UtfRow> header = ReadChunkTable(cpk, 0);
  if (header.empty() || !header[0].num.count("TocOffset")) {
    REXLOG_WARN("Game art: CPK header not readable");
    return images;
  }
  uint64_t toc = header[0].num["TocOffset"];
  uint64_t content = header[0].num.count("ContentOffset") ? header[0].num["ContentOffset"] : toc;
  uint64_t base = std::min(toc, content);
  std::map<std::string, std::pair<uint64_t, uint64_t>> files;  // "2D/X.2DV" -> offset, size
  for (UtfRow& row : ReadChunkTable(cpk, toc)) {
    files[row.str["DirName"] + "/" + row.str["FileName"]] = {row.num["FileOffset"] + base,
                                                              row.num["FileSize"]};
  }
  for (const auto& [pack, names] : wanted) {
    auto v = files.find("2D/" + pack + ".2DV");
    auto p = files.find("2D/" + pack + ".2DP");
    std::vector<uint8_t> vir, pix;
    if (v == files.end() || p == files.end() || !cpk.Read(v->second.first, v->second.second, vir) ||
        !cpk.Read(p->second.first, p->second.second, pix) || vir.size() < 0x20 ||
        std::memcmp(vir.data(), "KPKy", 4) != 0) {
      REXLOG_WARN("Game art: 2D/{} not readable", pack);
      continue;
    }
    size_t gpr = Be32(&vir[0x0C]);
    if (gpr + 0x20 > vir.size()) continue;
    uint32_t count = Be32(&vir[gpr + 0x1C]);
    size_t entries = gpr + Be32(&vir[gpr + 0x08]), name_base = gpr + Be32(&vir[gpr + 0x0C]),
           headers = gpr + Be32(&vir[gpr + 0x10]);
    for (uint32_t i = 0; i < count; ++i) {
      size_t e = entries + size_t(i) * 0x20;
      if (e + 0x20 > vir.size() || std::memcmp(&vir[e], "TX2D", 4) != 0) break;
      const char* name = reinterpret_cast<const char*>(&vir[name_base + Be32(&vir[e + 4])]);
      if (std::find(names.begin(), names.end(), name) == names.end()) continue;
      size_t h = headers + Be32(&vir[e + 0xC]);
      GameImage image;
      if (h + 0x34 <= vir.size() && Decode(&vir[h + 0x1C], pix, Be32(&vir[e + 0x14]), image)) {
        images[name] = std::move(image);
      }
    }
  }
  REXLOG_INFO("Game art: {} images loaded", images.size());
  return images;
}

std::map<std::string, PackTexture> ListPackTextures(const std::string& pack) {
  std::map<std::string, PackTexture> out;
  GuestFile cpk("game:\\media\\layer1.cpk");
  if (!cpk.ok()) return out;
  std::vector<UtfRow> header = ReadChunkTable(cpk, 0);
  if (header.empty() || !header[0].num.count("TocOffset")) return out;
  uint64_t toc = header[0].num["TocOffset"];
  uint64_t content = header[0].num.count("ContentOffset") ? header[0].num["ContentOffset"] : toc;
  uint64_t base = std::min(toc, content);
  std::vector<uint8_t> vir;
  for (UtfRow& row : ReadChunkTable(cpk, toc)) {
    if (row.str["DirName"] != "2D" || row.str["FileName"] != pack + ".2DV") continue;
    if (!cpk.Read(row.num["FileOffset"] + base, size_t(row.num["FileSize"]), vir)) return out;
    break;
  }
  if (vir.size() < 0x20 || std::memcmp(vir.data(), "KPKy", 4) != 0) return out;
  size_t gpr = Be32(&vir[0x0C]);
  if (gpr + 0x20 > vir.size()) return out;
  uint32_t count = Be32(&vir[gpr + 0x1C]);
  size_t entries = gpr + Be32(&vir[gpr + 0x08]), name_base = gpr + Be32(&vir[gpr + 0x0C]),
         headers = gpr + Be32(&vir[gpr + 0x10]);
  for (uint32_t i = 0; i < count; ++i) {
    size_t e = entries + size_t(i) * 0x20;
    if (e + 0x20 > vir.size() || std::memcmp(&vir[e], "TX2D", 4) != 0) break;
    size_t h = headers + Be32(&vir[e + 0xC]);
    if (h + 0x34 > vir.size() || name_base + Be32(&vir[e + 4]) >= vir.size()) continue;
    PackTexture t;
    t.offset = Be32(&vir[e + 0x14]);
    t.size = Be32(&vir[e + 0x18]);
    std::memcpy(t.fetch, &vir[h + 0x1C], sizeof(t.fetch));
    out[reinterpret_cast<const char*>(&vir[name_base + Be32(&vir[e + 4])])] = t;
  }
  return out;
}

bool DecodePackTexture(const PackTexture& t, const std::vector<uint8_t>& pix, GameImage& out) {
  return Decode(t.fetch, pix, t.offset, out);
}

}  // namespace nier
