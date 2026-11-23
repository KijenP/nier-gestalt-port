/**
 * @file    gpu/render/shader_library.cpp
 */
#include "renut_engine/gpu/render/shader_library.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <smolv.h>
#include <xxhash.h>
#include <zstd.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/bindless_compat.h"  // NieR (2026-11-16): the 4-set layout
#include "renut_engine/gpu/render/pipeline.h"           // kVertexConstantsSet, kDrawLayoutArraySet
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#if defined(PLUME_DEKO3D)
#include "renut_engine/gpu/banjo_renut_deko3d.h"  // banjo-nx (2026-10-17): the deko3d back end's GLSL package
#endif

REXCVAR_DEFINE_STRING(renut_shader_capture_dir, "shader_capture", "Nuts&Bolts/Graphics",
                      "Where shaders missing from the shader cache are written; the next build "
                      "recompiles them into the cache");

namespace renut::gpu {

namespace {

struct DecompressedCache {
  std::once_flag once;
  std::vector<uint8_t> bytes;
};

DecompressedCache g_spirv;
#if defined(_WIN32)
DecompressedCache g_dxil;
#endif
std::atomic<uint64_t> g_decompressed_bytes{0};  // ShaderCacheHostBytes

const uint8_t *Decompress(DecompressedCache &cache, const uint8_t *compressed,
                          size_t compressed_size, size_t decompressed_size, const char *name) {
  std::call_once(cache.once, [&] {
    cache.bytes.resize(decompressed_size);
    const size_t n = ZSTD_decompress(cache.bytes.data(), decompressed_size, compressed,
                                     compressed_size);
    if (ZSTD_isError(n) || n != decompressed_size) {
      RGPU_ERROR("{} shader cache decompression failed", name);
      cache.bytes.clear();
      cache.bytes.shrink_to_fit();
    }
    g_decompressed_bytes.fetch_add(cache.bytes.capacity(), std::memory_order_relaxed);
  });
  return cache.bytes.empty() ? nullptr : cache.bytes.data();
}

std::mutex g_capture_mutex;
std::unordered_set<uint64_t> g_captured;  // CaptureMissingShader

} // namespace

uint64_t HashShaderContainer(const uint8_t *container) {
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  return XXH3_64bits(container, size);
}

const ShaderCacheEntry *FindShaderEntry(uint64_t hash) {
  if (g_shaderCacheLayout != kShaderCacheLayout) {
    static bool logged = false;
    if (!logged) {
      RGPU_ERROR("Shader cache layout {} does not match the renderer's {}; rebuild it",
                 g_shaderCacheLayout, kShaderCacheLayout);
      logged = true;
    }
    return nullptr;
  }
  const ShaderCacheEntry *begin = g_shaderCacheEntries;
  const ShaderCacheEntry *end = begin + g_shaderCacheEntryCount;
  const ShaderCacheEntry *it = std::lower_bound(
      begin, end, hash, [](const ShaderCacheEntry &e, uint64_t h) { return e.hash < h; });
  return (it != end && it->hash == hash) ? it : nullptr;
}

void ShaderBoolSpecMask(const ShaderCacheEntry &entry, uint32_t mask[8]) {
  std::fill(mask, mask + 8, 0u);
#if defined(PLUME_DEKO3D)
  // banjo-nx: the GLSL package lists them (the SPIR-V cache need not be unpacked for deko3d).
  if (IsDeko3D()) {
    banjo::renut_gpu::deko3d::BoolSpecMask(entry.hash, mask);
    return;
  }
#endif
  if (!IsVulkan() || entry.spirvSize == 0)
    return;
  const uint8_t *cache = Decompress(g_spirv, g_compressedSpirvCache, g_spirvCacheCompressedSize,
                                    g_spirvCacheDecompressedSize, "SPIR-V");
  if (!cache)
    return;
  const uint8_t *smol = cache + entry.spirvOffset;
  std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
  if (spirv.size() < 20 || !smolv::Decode(smol, entry.spirvSize, spirv.data(), spirv.size()))
    return;
  // After the five header words, each instruction's first word is (word count << 16) | opcode. OpDecorate (71):
  // target, decoration, literals; SpecId is decoration 1.
  const auto *words = reinterpret_cast<const uint32_t *>(spirv.data());
  const size_t count = spirv.size() / 4;
  for (size_t i = 5; i < count;) {
    const uint32_t length = words[i] >> 16, opcode = words[i] & 0xFFFF;
    if (length == 0)
      break;
    if (opcode == 71 && length >= 4 && i + 3 < count && words[i + 2] == 1) {
      const uint32_t id = words[i + 3];
      if (id >= 1000 && id < 1256)
        mask[(id - 1000) / 32] |= 1u << ((id - 1000) % 32);
    }
    i += length;
  }
}

// The entry's SPIR-V as the device takes it (decoded; on Android, the Mali patches below). false on failure.
bool LoadVulkanSpirv(const ShaderCacheEntry &entry, std::vector<uint8_t> &spirv) {
  const uint8_t *cache = Decompress(g_spirv, g_compressedSpirvCache, g_spirvCacheCompressedSize,
                                    g_spirvCacheDecompressedSize, "SPIR-V");
  if (!cache || entry.spirvSize == 0)
    return false;
  const uint8_t *smol = cache + entry.spirvOffset;
  spirv.resize(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
  if (spirv.empty() || !smolv::Decode(smol, entry.spirvSize, spirv.data(), spirv.size())) {
    RGPU_ERROR("SPIR-V decode failed for shader {:016X}", entry.hash);
    return false;
  }
#if defined(__ANDROID__)
  // NieR (2026-11-07): Mali binds 7 descriptor sets, the draw layout has 8. The Texture2DArray view of the texture
  // table (set 4) is the same table as set 0, so it moves there (aliasing it), and the constant sets 5-7 move to 4-6
  // (pipeline.h, kVertexConstantsSet). OpDecorate (71): target, decoration (DescriptorSet is 34), set.
  {
    auto *words = reinterpret_cast<uint32_t *>(spirv.data());
    const size_t count = spirv.size() / 4;
    for (size_t i = 5; i < count;) {
      const uint32_t length = words[i] >> 16, opcode = words[i] & 0xFFFF;
      if (length == 0)
        break;
      if (opcode == 71 && length >= 4 && i + 3 < count && words[i + 2] == 34) {
        uint32_t &set = words[i + 3];
        if (set == 4) {
          static std::atomic<int> logged{0};  // TEMP (2026-11-07): which shaders read stacked (2D array) textures
          if (logged.fetch_add(1) < 40)
            RGPU_INFO("[android] shader {:016X} reads set 4 (Texture2DArray), aliased onto set 0", entry.hash);
          set = 0;
        }
        else if (set >= 5 && set <= 7)
          set -= 1;
      }
      i += length;
    }
    // NieR (2026-11-07): the positions invariant. The game draws a depth pre-pass and then its colour with an EQUAL
    // depth test; Mali (IDVS) compiles a vertex shader's position differently per pipeline, the depths differed by
    // a bit and the colour pass drew nothing (the 3D was its depth silhouette over the fog). OpDecorate (71) id
    // BuiltIn (11) Position (0) / OpMemberDecorate (72) struct member BuiltIn Position get an Invariant (18) beside.
    std::vector<uint32_t> out;
    out.reserve(count + 8);
    out.insert(out.end(), words, words + std::min<size_t>(5, count));
    for (size_t i = 5; i < count;) {
      const uint32_t length = words[i] >> 16, opcode = words[i] & 0xFFFF;
      if (length == 0 || i + length > count) {
        out.insert(out.end(), words + i, words + count);
        break;
      }
      out.insert(out.end(), words + i, words + i + length);
      if (opcode == 71 && length == 4 && words[i + 2] == 11 && words[i + 3] == 0)
        out.insert(out.end(), {(3u << 16) | 71u, words[i + 1], 18u});
      else if (opcode == 72 && length == 5 && words[i + 3] == 11 && words[i + 4] == 0)
        out.insert(out.end(), {(4u << 16) | 72u, words[i + 1], words[i + 2], 18u});
      i += length;
    }
    // NieR TEMP experiment (2026-11-07): env NIER_FP16=1 lets the Mali run a pixel shader's float math at half
    // precision (RelaxedPrecision, decoration 0, on the results of the float arithmetic), Mali's 2x ALU rate.
    static const bool fp16 = [] {
      const char *e = std::getenv("NIER_FP16");
      return e && e[0] == '1';
    }();
    if (fp16) {
      const size_t n = out.size();
      bool fragment = false;
      size_t last_decoration = 0;
      std::vector<uint32_t> relaxed;
      for (size_t i = 5; i < n;) {
        const uint32_t length = out[i] >> 16, opcode = out[i] & 0xFFFF;
        if (length == 0 || i + length > n)
          break;
        if (opcode == 15 && out[i + 1] == 4)
          fragment = true;
        if (opcode >= 71 && opcode <= 75)
          last_decoration = i + length;
        bool arith = false;
        switch (opcode) {
        case 127: case 129: case 131: case 133: case 136: case 140: case 141: case 142:
        case 143: case 144: case 145: case 146: case 148:
          arith = true;
          break;
        case 12:  // OpExtInst: type, id, set, instruction; GLSL.std.450 float instructions
          if (length >= 5) {
            switch (out[i + 4]) {
            case 1: case 2: case 3: case 4: case 6: case 8: case 9: case 10: case 13: case 14:
            case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 37: case 40:
            case 43: case 46: case 48: case 49: case 50: case 66: case 67: case 68: case 69:
              arith = true;
            }
          }
          break;
        }
        if (arith && length >= 3)
          relaxed.push_back(out[i + 2]);
        i += length;
      }
      if (fragment && last_decoration && !relaxed.empty()) {
        std::vector<uint32_t> deco;
        deco.reserve(relaxed.size() * 3);
        for (uint32_t id : relaxed)
          deco.insert(deco.end(), {(3u << 16) | 71u, id, 0u});
        out.insert(out.begin() + last_decoration, deco.begin(), deco.end());
      }
    }
    spirv.assign(reinterpret_cast<const uint8_t *>(out.data()),
                 reinterpret_cast<const uint8_t *>(out.data() + out.size()));
  }
#endif
  // NieR (2026-11-16): the 4-set layout (compat::Packed, an Adreno 619 binds 4 sets). The texture tables (sets 0-3,
  // and off Android the 2D-array view at 4) become bindings of set 0: 2D 0, 3D 1, cube 2, samplers 3 (the array
  // view reads the 2D one); the constants (kVertexConstantsSet..+2) become sets 1-3. OpDecorate (71): target,
  // decoration (DescriptorSet 34, Binding 33), value; a variable's set is read first, then both are rewritten.
  // NieR (2026-11-19): a GPU without 64-bit shader integers (Adreno 619, or nier_emulate_gpu=adreno619 on a PC) fails
  // the pipelines of shaders declaring OpCapability (17) Int64 (11): each one is reported, to know what to rewrite.
  if (compat::NoInt64()) {
    const auto *words = reinterpret_cast<const uint32_t *>(spirv.data());
    const size_t count = spirv.size() / 4;
    for (size_t i = 5; i < count;) {
      const uint32_t length = words[i] >> 16, opcode = words[i] & 0xFFFF;
      if (length == 0 || opcode != 17)  // the capabilities come first
        break;
      if (length >= 2 && i + 1 < count && words[i + 1] == 11) {
        static int reported = 0;
        if (reported++ < 200)
          RGPU_WARN("[compat] shader {:016X} usa inteiros de 64 bits (Int64): falha numa GPU sem shaderInt64 ({})",
                    entry.hash, compat::Emulated()[0] ? compat::Emulated() : "este aparelho");
        break;
      }
      i += length;
    }
  }
  if (compat::Packed()) {
    auto *words = reinterpret_cast<uint32_t *>(spirv.data());
    const size_t count = spirv.size() / 4;
    std::unordered_map<uint32_t, uint32_t> set_of;
    for (size_t i = 5; i < count;) {
      const uint32_t length = words[i] >> 16, opcode = words[i] & 0xFFFF;
      if (length == 0)
        break;
      if (opcode == 71 && length >= 4 && i + 3 < count && words[i + 2] == 34)
        set_of[words[i + 1]] = words[i + 3];
      i += length;
    }
    for (size_t i = 5; i < count;) {
      const uint32_t length = words[i] >> 16, opcode = words[i] & 0xFFFF;
      if (length == 0)
        break;
      if (opcode == 71 && length >= 4 && i + 3 < count && (words[i + 2] == 34 || words[i + 2] == 33)) {
        const auto it = set_of.find(words[i + 1]);
        if (it != set_of.end()) {
          const uint32_t set = it->second;
          const bool table = set <= 3 || (kDrawLayoutArraySet && set == 4);
          if (words[i + 2] == 34) {
            if (table)
              words[i + 3] = 0;
            else if (set >= kVertexConstantsSet && set <= kVertexConstantsSet + 2)
              words[i + 3] = set - kVertexConstantsSet + 1;
          } else if (table) {
            words[i + 3] = set == 4 ? 0 : set;  // the table's one binding (0) becomes its place in set 0
          }
        }
      }
      i += length;
    }
  }
  return true;
}

std::unique_ptr<plume::RenderShader> CreateCachedShader(plume::RenderDevice *device,
                                                        const ShaderCacheEntry &entry) {
  if (!device)
    return nullptr;

#if defined(PLUME_DEKO3D)
  // banjo-nx (2026-10-17): the deko3d back end's GLSL for uam, from the package in the NRO.
  if (IsDeko3D())
    return banjo::renut_gpu::deko3d::CreateGameShader(device, entry.hash);
#endif

  if (IsVulkan()) {
    std::vector<uint8_t> spirv;
    if (!LoadVulkanSpirv(entry, spirv))
      return nullptr;
    return device->createShader(spirv.data(), spirv.size(), "main",
                                plume::RenderShaderFormat::SPIRV);
  }
#if defined(_WIN32)
  const uint8_t *dxil_cache = Decompress(g_dxil, g_compressedDxilCache, g_dxilCacheCompressedSize,
                                         g_dxilCacheDecompressedSize, "DXIL");
  if (!dxil_cache || entry.dxilSize == 0)
    return nullptr;
  return device->createShader(dxil_cache + entry.dxilOffset, entry.dxilSize, "main",
                              plume::RenderShaderFormat::DXIL);
#else
  return nullptr;
#endif
}


const ShaderCacheEntry *FindShaderBaseEntry(const uint8_t *container) {
  if (g_shaderCacheLayout != kShaderCacheLayout || g_shaderBaseKeyCount == 0)
    return nullptr;
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const u32 virtual_size = u32(header->virtualSize), physical_size = u32(header->physicalSize);
  if (virtual_size <= sizeof(d3d::ShaderContainer))
    return nullptr;
  uint64_t key = XXH3_64bits(container + sizeof(d3d::ShaderContainer), virtual_size - sizeof(d3d::ShaderContainer));
  key ^= uint64_t(physical_size) * 0x9E3779B97F4A7C15ull;
  const ShaderBaseKey *begin = g_shaderBaseKeys, *end = begin + g_shaderBaseKeyCount;
  const ShaderBaseKey *it =
      std::lower_bound(begin, end, key, [](const ShaderBaseKey &e, uint64_t k) { return e.key < k; });
  if (it == end || it->key != key || it->index >= g_shaderCacheEntryCount)
    return nullptr;
  return &g_shaderCacheEntries[it->index];
}

uint64_t ShaderCacheHostBytes() {
  return g_decompressed_bytes.load(std::memory_order_relaxed);
}

void CaptureMissingShader(const uint8_t *container, bool pixel_shader, uint64_t hash) {
  std::lock_guard lock(g_capture_mutex);
  if (!g_captured.insert(hash).second)
    return;

  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  // banjo-nx: next to the executable (on the Switch, the NRO's folder), not the working directory.
  const std::filesystem::path dir = banjo::renut_gpu::NextToExecutable(REXCVAR_GET(renut_shader_capture_dir));
  const std::filesystem::path path =
      dir / fmt::format("{}_{:016x}.bin", pixel_shader ? "ps" : "vs", hash);

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(path, std::ios::binary);
  if (out)
    out.write(reinterpret_cast<const char *>(container), std::streamsize(size));
  if (!out) {
    RGPU_WARN("Shader {:016X} is not in the shader cache and could not be captured to {}", hash,
              path.string());
    return;
  }
  RGPU_WARN("Shader {:016X} is not in the shader cache: captured to {}, the next build adds it",
            hash, path.string());
}

} // namespace renut::gpu
