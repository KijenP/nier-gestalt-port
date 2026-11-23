// nier - CRI audio formats the game's music is made of (read from the player's copy, nothing shipped):
//
//  * @UTF tables: CPK tables of contents and the cue sheets (.CSB). Big-endian; a 32-byte header, columns (a type
//    byte: storage << 4 | type, a name), then fixed-size rows. "data" cells point into the table's data area, often
//    holding another @UTF table.
//  * AAX: an @UTF table "AAX" of segments (column data: an ADX stream; lpflg: 1 = the part that repeats). NieR's
//    music is an intro segment (lpflg 0) and a loop segment, or one segment.
//  * ADX: CRI's 4-bit ADPCM. Header 0x8000, data after the "(c)CRI" mark (offset at +2, plus 4); frames of 18 bytes
//    per channel (a 16-bit scale, then 32 nibbles), the channels one after the other. Two-tap prediction whose
//    coefficients come from the high-pass frequency in the header. NieR's are type 3, unencrypted.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace nier::jukebox::cri {

inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

struct Cell {
  bool has = false;
  uint8_t type = 0;
  uint64_t num = 0;
  std::string str;
  size_t data_off = 0, data_size = 0;  // "data" cells: absolute in the parsed buffer
};

struct Table {
  std::string name;
  std::vector<std::string> cols;
  std::vector<std::vector<Cell>> rows;

  int Col(std::string_view n) const {
    for (size_t i = 0; i < cols.size(); ++i)
      if (cols[i] == n) return int(i);
    return -1;
  }
  const Cell* At(size_t r, std::string_view c) const {
    const int i = Col(c);
    return i < 0 || r >= rows.size() || !rows[r][size_t(i)].has ? nullptr : &rows[r][size_t(i)];
  }
  uint64_t Num(size_t r, std::string_view c, uint64_t def = 0) const {
    const Cell* x = At(r, c);
    return x ? x->num : def;
  }
  std::string Str(size_t r, std::string_view c) const {
    const Cell* x = At(r, c);
    return x ? x->str : std::string();
  }
};

// The @UTF table at buf[base]; false when it is not one.
inline bool ParseUtf(const uint8_t* buf, size_t size, size_t base, Table& out) {
  out = {};
  if (base + 32 > size || std::memcmp(buf + base, "@UTF", 4) != 0) return false;
  const size_t t = base + 8, end = std::min(size, t + Be32(buf + base + 4));
  const size_t rows_o = t + Be16(buf + t + 2), str_o = t + Be32(buf + t + 4), data_o = t + Be32(buf + t + 8);
  const uint32_t name_o = Be32(buf + t + 12);
  const uint32_t ncol = Be16(buf + t + 16), row_len = Be16(buf + t + 18), nrow = Be32(buf + t + 20);
  auto str = [&](uint32_t off) {
    size_t s = str_o + off, e = s;
    while (e < end && buf[e]) ++e;
    return s < end ? std::string(reinterpret_cast<const char*>(buf + s), e - s) : std::string();
  };
  auto read = [&](uint8_t type, size_t o, Cell& c) -> size_t {
    c.has = true;
    c.type = type;
    switch (type) {
      case 0: case 1: c.num = buf[o]; return 1;
      case 2: case 3: c.num = Be16(buf + o); return 2;
      case 4: case 5: case 8: c.num = Be32(buf + o); return 4;
      case 6: case 7: case 9: c.num = Be64(buf + o); return 8;
      case 0xA: c.str = str(Be32(buf + o)); return 4;
      case 0xB: c.data_off = data_o + Be32(buf + o); c.data_size = Be32(buf + o + 4); return 8;
    }
    c.has = false;
    return 0;
  };
  out.name = str(name_o);
  struct Col { uint8_t storage, type; Cell konst; };
  std::vector<Col> cols;
  size_t p = t + 24;
  for (uint32_t i = 0; i < ncol; ++i) {
    if (p + 5 > end) return false;
    Col c{uint8_t(buf[p] & 0xF0), uint8_t(buf[p] & 0x0F), {}};
    out.cols.push_back(str(Be32(buf + p + 1)));
    p += 5;
    if (c.storage == 0x30 || c.storage == 0x70) p += read(c.type, p, c.konst);
    cols.push_back(c);
  }
  out.rows.resize(nrow);
  for (uint32_t r = 0; r < nrow; ++r) {
    size_t q = rows_o + size_t(r) * row_len;
    auto& row = out.rows[r];
    row.resize(cols.size());
    for (size_t i = 0; i < cols.size(); ++i) {
      if (cols[i].storage == 0x50) {
        if (q + 8 > end + 8) return false;
        q += read(cols[i].type, q, row[i]);
      } else if (cols[i].storage == 0x30 || cols[i].storage == 0x70) {
        row[i] = cols[i].konst;
      }
    }
  }
  return true;
}

// A CPK chunk ("CPK ", "TOC "): a 16-byte header (size at +8, little-endian) then the @UTF table.
inline uint64_t ChunkSize(const uint8_t* head16) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t(head16[8 + i]) << (8 * i);
  return v;
}

struct AdxInfo {
  int channels = 0;
  int rate = 0;
  uint32_t samples = 0;
  uint32_t data_start = 0;  // first frame, from the ADX's start
  int32_t c1 = 0, c2 = 0;
  bool ok() const { return channels > 0 && channels <= 8 && rate > 0; }
  uint32_t frame_bytes() const { return 18u * uint32_t(channels); }
};

inline bool ParseAdx(const uint8_t* h, size_t n, AdxInfo& info) {
  info = {};
  if (n < 0x14 || Be16(h) != 0x8000 || h[4] != 3 || h[5] != 18 || h[6] != 4) return false;
  if (h[0x13] & 0x08) return false;  // encrypted: none in NieR
  info.data_start = uint32_t(Be16(h + 2)) + 4;
  info.channels = h[7];
  info.rate = int(Be32(h + 8));
  info.samples = Be32(h + 12);
  const double cutoff = Be16(h + 16);
  const double a = std::sqrt(2.0) - std::cos(2.0 * 3.14159265358979323846 * cutoff / info.rate);
  const double b = std::sqrt(2.0) - 1.0;
  const double c = (a - std::sqrt((a + b) * (a - b))) / b;
  info.c1 = int32_t(c * 8192.0);
  info.c2 = int32_t(-(c * c) * 4096.0);
  return info.ok();
}

struct AdxHistory {
  int32_t h1[8] = {}, h2[8] = {};
};

// One frame set (18 bytes per channel) -> 32 interleaved samples per channel, in [-1, 1).
inline void DecodeAdxFrame(const uint8_t* frame, const AdxInfo& info, AdxHistory& hist, float* out) {
  for (int ch = 0; ch < info.channels; ++ch) {
    const uint8_t* f = frame + 18 * ch;
    const int32_t scale = int32_t(Be16(f)) + 1;
    int32_t h1 = hist.h1[ch], h2 = hist.h2[ch];
    for (int i = 0; i < 32; ++i) {
      const uint8_t byte = f[2 + i / 2];
      int32_t nib = (i & 1) ? (byte & 15) : (byte >> 4);
      if (nib >= 8) nib -= 16;
      int32_t v = nib * scale + ((info.c1 * h1 + info.c2 * h2) >> 12);
      v = std::clamp(v, -32768, 32767);
      h2 = h1;
      h1 = v;
      out[i * info.channels + ch] = float(v) * (1.0f / 32768.0f);
    }
    hist.h1[ch] = h1;
    hist.h2[ch] = h2;
  }
}

}  // namespace nier::jukebox::cri
