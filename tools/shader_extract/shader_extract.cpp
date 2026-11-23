// NieR Gestalt PC - shader extractor (2026-11-14).
//
// Pulls every Xbox 360 shader container out of the player's own copy of the game, for Renut's shader cache
// (cmake/nier_renut_gpu.cmake). Nothing of the game is in the repository: this runs on the player's machine.
//
//   nier_shader_extract [--archives] <out dir> <game.iso | game folder | file>...
//
// Found by a scan, without reading the disc's or the archives' file systems (both sit whole in the disc image, so an
// .iso is scanned as one file; a folder's *.cpk and default.xex instead):
//   - default.xex: AES-encrypted and "basic"-compressed (decrypted with the retail key and expanded here). Its
//     containers are whole and exactly what the game binds (US disc: 117, 112 of the shaders captured in play).
//   - --archives (research only): the CRI archives (media/*.cpk), raw and inside CRILAYLA-compressed files
//     (decompressed here). NOT usable as they are (2026-11-14): there a container's header and its microcode sit in
//     different files (a model's CPU and GPU parts; the bytes after the header are not its microcode), and the
//     Direct3D runtime adds the vertex fetches of the bound declaration to a vertex shader's microcode when it is set
//     (24 bytes more measured), so no file holds the shader the game binds. A cache made from them drew black.
//     Those shaders come from Renut's capture while playing (CaptureMissingShader; the next build adds them).
//
// A container starts with 0x102A1100 (pixel) or 0x102A1101 (vertex), then its virtual and physical sizes (d3d.h
// ShaderContainer). Each is written as Renut captures one (shader_library.cpp CaptureMissingShader):
// ps_/vs_<XXH3_64 of the whole container>.bin.
#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kHeaderSize = 0x24;
constexpr uint32_t kMaxPart = 1u << 20;  // a part over 1 MB is not a shader

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }

struct Stats {
  uint64_t found = 0, written = 0, archives = 0, images = 0;
  std::set<uint64_t> unique;
  fs::path out;
  bool scan_archives = false;  // --archives: the CRI archives too (research; see the top)
};

// The container's whole size when the header at h is a sane one, else 0.
uint32_t ContainerSize(const uint8_t* h) {
  const uint32_t virt = Be32(h + 4), phys = Be32(h + 8);
  const uint32_t constants = Be32(h + 16), shader = Be32(h + 24);
  if (virt < kHeaderSize || virt > kMaxPart || phys == 0 || phys > kMaxPart || (phys & 3)) return 0;
  if (shader < kHeaderSize || shader >= virt || constants > virt) return 0;
  return virt + phys;
}

bool IsShaderMagic(const uint8_t* p) { return p[0] == 0x10 && p[1] == 0x2A && p[2] == 0x11 && p[3] <= 1; }

void Write(const uint8_t* c, uint32_t size, Stats& st) {
  ++st.found;
  const uint64_t hash = XXH3_64bits(c, size);
  if (!st.unique.insert(hash).second) return;
  char name[64];
  std::snprintf(name, sizeof(name), "%s_%016llx.bin", c[3] == 0 ? "ps" : "vs", static_cast<unsigned long long>(hash));
  const fs::path file = st.out / name;
  std::error_code ec;
  if (fs::exists(file, ec)) return;
  std::ofstream o(file, std::ios::binary);
  o.write(reinterpret_cast<const char*>(c), std::streamsize(size));
  if (o) ++st.written;
}

// ---------------------------------------------------------------------------------------------------------------
// CRILAYLA (CRI's archive compression): "CRILAYLA", the size out (LE), the compressed size (LE), the compressed
// bits read backwards from its end, then the first 0x100 bytes of the file stored as they are.
// ---------------------------------------------------------------------------------------------------------------
bool CrilaylaExpand(const uint8_t* in, size_t avail, std::vector<uint8_t>& out) {
  if (avail < 0x10 || std::memcmp(in, "CRILAYLA", 8) != 0) return false;
  const uint32_t size = Le32(in + 8), packed = Le32(in + 12);
  if (size == 0 || size > (256u << 20) || size_t(packed) + 0x110 > avail) return false;
  out.assign(size_t(size) + 0x100, 0);
  std::memcpy(out.data(), in + 0x10 + packed, 0x100);
  int64_t pos = int64_t(0x10) + packed - 1;  // the bits are read from here back
  uint32_t pool = 0, left = 0;
  bool bad = false;
  auto bits = [&](uint32_t count) -> uint32_t {
    uint32_t v = 0, got = 0;
    while (got < count) {
      if (left == 0) {
        if (pos < 0x10) {
          bad = true;
          return 0;
        }
        pool = in[pos--];
        left = 8;
      }
      const uint32_t take = std::min(left, count - got);
      v = (v << take) | ((pool >> (left - take)) & ((1u << take) - 1));
      left -= take;
      got += take;
    }
    return v;
  };
  static constexpr uint32_t kLens[4] = {2, 3, 5, 8};
  const int64_t end = int64_t(0x100) + size - 1;
  int64_t done = 0;
  while (done < int64_t(size) && !bad) {
    if (bits(1)) {
      int64_t from = end - done + bits(13) + 3;
      int64_t length = 3;
      int level = 0;
      for (; level < 4; ++level) {
        const uint32_t v = bits(kLens[level]);
        length += v;
        if (v != (1u << kLens[level]) - 1) break;
      }
      if (level == 4) {
        uint32_t v;
        do {
          v = bits(8);
          length += v;
        } while (v == 255 && !bad);
      }
      for (int64_t i = 0; i < length && done < int64_t(size); ++i) {
        if (from > end || from < 0) return false;
        out[size_t(end - done)] = out[size_t(from--)];
        ++done;
      }
    } else {
      out[size_t(end - done)] = uint8_t(bits(8));
      ++done;
    }
  }
  return !bad;
}

// ---------------------------------------------------------------------------------------------------------------
// AES-128 (decryption only), for default.xex.
// ---------------------------------------------------------------------------------------------------------------
struct Aes128 {
  uint8_t rk[176];
  static uint8_t Mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
      if (b & 1) r ^= a;
      a = uint8_t((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
      b >>= 1;
    }
    return r;
  }
  static const uint8_t* Sbox() {
    static uint8_t s[256];
    static bool made = false;
    if (!made) {
      // The S-box from the multiplicative inverse and the affine map.
      for (int x = 0; x < 256; ++x) {
        uint8_t inv = 0;
        for (int y = 1; y < 256 && x; ++y)
          if (Mul(uint8_t(x), uint8_t(y)) == 1) inv = uint8_t(y);
        uint8_t b = inv, r = inv;
        for (int i = 0; i < 4; ++i) {
          b = uint8_t((b << 1) | (b >> 7));
          r ^= b;
        }
        s[x] = uint8_t(r ^ 0x63);
      }
      made = true;
    }
    return s;
  }
  static const uint8_t* InvSbox() {
    static uint8_t inv[256];
    static bool made = false;
    if (!made) {
      const uint8_t* s = Sbox();
      for (int i = 0; i < 256; ++i) inv[s[i]] = uint8_t(i);
      made = true;
    }
    return inv;
  }
  explicit Aes128(const uint8_t key[16]) {
    const uint8_t* s = Sbox();
    std::memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
      uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
      if (i % 16 == 0) {
        const uint8_t u = t[0];
        t[0] = uint8_t(s[t[1]] ^ rcon);
        t[1] = s[t[2]];
        t[2] = s[t[3]];
        t[3] = s[u];
        rcon = Mul(rcon, 2);
      }
      for (int j = 0; j < 4; ++j) rk[i + j] = uint8_t(rk[i - 16 + j] ^ t[j]);
    }
  }
  void DecryptBlock(uint8_t b[16]) const {
    const uint8_t* is = InvSbox();
    for (int i = 0; i < 16; ++i) b[i] ^= rk[160 + i];
    for (int round = 9; round >= 0; --round) {
      uint8_t t[16];  // inverse shift rows, then inverse sub bytes
      for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) t[((c + r) % 4) * 4 + r] = is[b[c * 4 + r]];
      for (int i = 0; i < 16; ++i) b[i] = uint8_t(t[i] ^ rk[round * 16 + i]);
      if (round == 0) break;
      for (int c = 0; c < 4; ++c) {  // inverse mix columns
        const uint8_t a0 = b[c * 4], a1 = b[c * 4 + 1], a2 = b[c * 4 + 2], a3 = b[c * 4 + 3];
        b[c * 4] = uint8_t(Mul(a0, 14) ^ Mul(a1, 11) ^ Mul(a2, 13) ^ Mul(a3, 9));
        b[c * 4 + 1] = uint8_t(Mul(a0, 9) ^ Mul(a1, 14) ^ Mul(a2, 11) ^ Mul(a3, 13));
        b[c * 4 + 2] = uint8_t(Mul(a0, 13) ^ Mul(a1, 9) ^ Mul(a2, 14) ^ Mul(a3, 11));
        b[c * 4 + 3] = uint8_t(Mul(a0, 11) ^ Mul(a1, 13) ^ Mul(a2, 9) ^ Mul(a3, 14));
      }
    }
  }
};

// ---------------------------------------------------------------------------------------------------------------
// default.xex (XEX2): its file format info (optional header 0x3FF) says the encryption (1 = AES with the retail key
// over a per-title key at security info + 0x150) and the compression (1 = basic: blocks of data, each followed by
// zeroes). The image follows the headers (at +8).
// ---------------------------------------------------------------------------------------------------------------
// The size of the XEX2 at x (headers plus its data), 0 when it is not one this reads.
size_t XexSize(const uint8_t* x, size_t avail) {
  if (avail < 24 || std::memcmp(x, "XEX2", 4) != 0) return 0;
  const uint32_t pe = Be32(x + 8), count = Be32(x + 20);
  if (pe > avail || count > 64 || 24 + size_t(count) * 8 > avail) return 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (Be32(x + 24 + i * 8) != 0x3FF) continue;
    const uint32_t ffi = Be32(x + 28 + i * 8);
    if (ffi + 8 > pe) return 0;
    const uint32_t ffi_size = Be32(x + ffi);
    if (Be32(x + ffi + 4) >> 16 > 1 || (Be32(x + ffi + 4) & 0xFFFF) != 1 || ffi + ffi_size > pe) return 0;
    size_t data = 0;
    for (uint32_t o = 8; o + 8 <= ffi_size; o += 8) data += Be32(x + ffi + o);
    return pe + data;
  }
  return 0;
}

bool XexImage(const uint8_t* x, size_t size, std::vector<uint8_t>& image) {
  if (XexSize(x, size) == 0 || XexSize(x, size) > size) return false;
  const uint32_t pe = Be32(x + 8), count = Be32(x + 20), security = Be32(x + 16);
  uint32_t ffi = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (Be32(x + 24 + i * 8) == 0x3FF) ffi = Be32(x + 28 + i * 8);
  const bool encrypted = (Be32(x + ffi + 4) >> 16) == 1;
  std::vector<uint8_t> data(x + pe, x + XexSize(x, size));
  if (encrypted) {
    if (security + 0x160 > pe) return false;
    static constexpr uint8_t kRetail[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                            0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
    uint8_t key[16];
    std::memcpy(key, x + security + 0x150, 16);
    Aes128(kRetail).DecryptBlock(key);
    const Aes128 aes(key);
    uint8_t iv[16] = {}, prev[16];
    for (size_t o = 0; o + 16 <= data.size(); o += 16) {  // CBC
      std::memcpy(prev, data.data() + o, 16);
      aes.DecryptBlock(data.data() + o);
      for (int i = 0; i < 16; ++i) data[o + i] ^= iv[i];
      std::memcpy(iv, prev, 16);
    }
  }
  image.clear();
  const uint32_t ffi_size = Be32(x + ffi);
  size_t at = 0;
  for (uint32_t o = 8; o + 8 <= ffi_size; o += 8) {
    const uint32_t d = Be32(x + ffi + o), z = Be32(x + ffi + o + 4);
    if (at + d > data.size()) return false;
    image.insert(image.end(), data.begin() + at, data.begin() + at + d);
    image.insert(image.end(), z, 0);
    at += d;
  }
  return image.size() >= 2 && image[0] == 'M' && image[1] == 'Z';
}

// ---------------------------------------------------------------------------------------------------------------

void ScanBuffer(const uint8_t* b, size_t n, Stats& st) {
  for (size_t i = 0; i + kHeaderSize <= n; i += 4) {
    if (!IsShaderMagic(b + i)) continue;
    const uint32_t size = ContainerSize(b + i);
    if (size && i + size <= n) Write(b + i, size, st);
  }
}

bool ReadAt(std::ifstream& in, uint64_t at, size_t size, std::vector<uint8_t>& out) {
  out.resize(size);
  in.clear();
  in.seekg(std::streamoff(at));
  in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
  out.resize(size_t(in.gcount()));
  in.clear();
  return out.size() == size;
}

// A file (an .iso, a .cpk, default.xex) as a stream: the containers in the clear, then each CRILAYLA file and XEX2
// found in it.
void ScanFile(const fs::path& path, Stats& st) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
    return;
  }
  std::vector<uint64_t> shaders, packed, xexes;
  {
    std::vector<uint8_t> buf(16u << 20);
    uint64_t pos = 0;      // the file offset of buf[0] (4-byte aligned)
    uint64_t checked = 0;  // the offsets below this were looked at
    size_t keep = 0;
    while (true) {
      in.read(reinterpret_cast<char*>(buf.data() + keep), std::streamsize(buf.size() - keep));
      const size_t len = keep + size_t(in.gcount());
      if (len < 8) break;
      // Containers sit 4-byte aligned in their files, the files sector-aligned in the archives and the image.
      for (size_t i = 0; i + 8 <= len; i += 4) {
        if (pos + i < checked) continue;
        checked = pos + i + 4;
        const uint8_t* p = buf.data() + i;
        if (IsShaderMagic(p))
          shaders.push_back(pos + i);
        else if (p[0] == 'C' && std::memcmp(p, "CRILAYLA", 8) == 0)
          packed.push_back(pos + i);
        else if (p[0] == 'X' && std::memcmp(p, "XEX2", 4) == 0 && ((pos + i) & 0x7FF) == 0)
          xexes.push_back(pos + i);
      }
      if (!in) break;
      keep = (len & 3) + 8;  // the tail, which the next read looks at again (the start stays 4-byte aligned)
      std::memmove(buf.data(), buf.data() + len - keep, keep);
      pos += len - keep;
    }
  }
  std::vector<uint8_t> c, raw;
  if (!st.scan_archives) {  // only the executables (see the top)
    shaders.clear();
    packed.clear();
  }
  for (const uint64_t at : shaders) {
    if (!ReadAt(in, at, kHeaderSize, c)) continue;
    const uint32_t size = ContainerSize(c.data());
    if (size && ReadAt(in, at, size, c)) Write(c.data(), size, st);
  }
  std::vector<uint8_t> expanded;
  for (const uint64_t at : packed) {
    if (!ReadAt(in, at, 0x10, raw)) continue;
    const size_t whole = size_t(Le32(raw.data() + 12)) + 0x110;
    if (whole > (256u << 20) || !ReadAt(in, at, whole, raw)) continue;
    if (CrilaylaExpand(raw.data(), raw.size(), expanded)) {
      ++st.archives;
      ScanBuffer(expanded.data(), expanded.size(), st);
    }
  }
  for (const uint64_t at : xexes) {
    if (!ReadAt(in, at, 0x10000, raw)) continue;
    const size_t whole = XexSize(raw.data(), raw.size());
    if (!whole || whole > (512u << 20) || !ReadAt(in, at, whole, raw)) continue;
    if (XexImage(raw.data(), raw.size(), expanded)) {
      ++st.images;
      ScanBuffer(expanded.data(), expanded.size(), st);
    }
  }
}

std::string Lower(std::string s) {
  for (auto& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  Stats st;
  int first = 1;
  if (argc > 1 && std::strcmp(argv[1], "--archives") == 0) {
    st.scan_archives = true;
    first = 2;
  }
  if (argc < first + 2) {
    std::fprintf(stderr, "usage: nier_shader_extract [--archives] <out dir> <game.iso | game folder | file>...\n");
    return 2;
  }
  st.out = fs::u8path(argv[first]);
  std::error_code ec;
  fs::create_directories(st.out, ec);
  size_t inputs = 0;
  for (int i = first + 1; i < argc; ++i) {
    const fs::path p = fs::u8path(argv[i]);
    if (fs::is_directory(p, ec)) {
      // An extracted copy: its archives and default.xex.
      for (const auto& e : fs::recursive_directory_iterator(p, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string name = Lower(e.path().filename().string());
        if (Lower(e.path().extension().string()) == ".cpk" || name == "default.xex") {
          ScanFile(e.path(), st);
          ++inputs;
        }
      }
    } else if (fs::is_regular_file(p, ec)) {
      ScanFile(p, st);
      ++inputs;
    } else {
      std::fprintf(stderr, "not found: %s\n", argv[i]);
    }
  }
  std::printf("shaders: %zu input(s), %llu compressed file(s) and %llu executable(s) opened, %llu container(s) found, "
              "%zu unique, %llu new\n",
              inputs, static_cast<unsigned long long>(st.archives), static_cast<unsigned long long>(st.images),
              static_cast<unsigned long long>(st.found), st.unique.size(),
              static_cast<unsigned long long>(st.written));
  return inputs && !st.unique.empty() ? 0 : 1;
}
