/**
 * @file    gpu/backend/bindless_compat.cpp
 * @brief   NieR (2026-11-15): see bindless_compat.h.
 */
#include "renut_engine/gpu/backend/bindless_compat.h"

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_vulkan.h>
#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/log.h"

REXCVAR_DEFINE_STRING(nier_bindless_compat, "auto", "NieR/Graphics",
                      "GPUs without descriptor indexing (Mali-G52 and other Mali Bifrost): small texture sets per draw "
                      "instead of the bindless tables. auto: where the GPU needs it; on: always (a test on any GPU); "
                      "off: never. Takes effect after a restart.")
    .allowed({"auto", "on", "off"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(nier_packed_sets, "auto", "NieR/Graphics",
                      "GPUs that bind fewer than the draw layout's descriptor sets (Adreno 6xx with older drivers bind "
                      "4): the textures and samplers in one set, the shaders' sets renumbered when they load. auto: "
                      "where the GPU needs it; on: always (a test, Vulkan); off: never. Takes effect after a restart.")
    .allowed({"auto", "on", "off"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(nier_emulate_gpu, "off", "NieR/Graphics",
                      "Imitate a phone's GPU on this one (Vulkan), to find its faults on a PC: adreno619 (Snapdragon "
                      "695: 4 descriptor sets, no 64-bit integers in shaders, which are reported), mali-g52 (no "
                      "descriptor indexing). off: this GPU as it is. Takes effect after a restart.")
    .allowed({"off", "adreno619", "mali-g52"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace renut::gpu::compat {

namespace {

bool g_on = false;
bool g_packed = false;
bool g_no_int64 = false;
bool g_old_driver = false;
bool g_adreno = false;
std::string g_emulated;

// The CPU table behind s.texture_set / s.sampler_set in this mode.
class MirrorTable final : public plume::RenderDescriptorSet {
 public:
  struct Entry {
    const plume::RenderTexture *texture = nullptr;
    const plume::RenderTextureView *view = nullptr;
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::SHADER_READ;
    const plume::RenderSampler *sampler = nullptr;
    uint32_t generation = 0;
  };

  explicit MirrorTable(uint32_t count) : entries_(count) {}

  void setBuffer(uint32_t, const plume::RenderBuffer *, uint64_t, const plume::RenderBufferStructuredView *,
                 const plume::RenderBufferFormattedView *) override {}
  void setTexture(uint32_t index, const plume::RenderTexture *texture, plume::RenderTextureLayout layout,
                  const plume::RenderTextureView *view) override {
    if (index >= entries_.size())
      return;
    Entry &e = entries_[index];
    e.texture = texture;
    e.view = view;
    e.layout = layout;
    ++e.generation;
  }
  void setSampler(uint32_t index, const plume::RenderSampler *sampler) override {
    if (index >= entries_.size())
      return;
    entries_[index].sampler = sampler;
    ++entries_[index].generation;
  }
  void setAccelerationStructure(uint32_t, const plume::RenderAccelerationStructure *) override {}

  const Entry *Get(uint32_t index) const { return index < entries_.size() ? &entries_[index] : nullptr; }

 private:
  std::vector<Entry> entries_;
};

// Up to four parts of kSize entries (the combined set of the 4-set layout: 2D, 3D, cube textures and samplers).
constexpr uint32_t kMaxParts = 4;
using Key = std::array<uint64_t, kSize * kMaxParts>;

struct KeyHash {
  size_t operator()(const Key &k) const {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t v : k) {
      h ^= v;
      h *= 1099511628211ull;
    }
    return size_t(h ^ (h >> 32));
  }
};

// One kind of small set (textures, samplers, or the combined one): the sets of each frame slot, reused once the GPU
// is done with it.
// 2026-11-20 (Poco C75, Mali-G52: render thread 71 ms a frame against 8 ms on a Mali-G57 with the bindless tables):
// the sets and their keys now outlive the frame. Before, each frame slot's cache was emptied when the slot came back
// and the sets were handed out again in order, so every draw looked its set up in an empty cache and most were
// rewritten (one vkUpdateDescriptorSets per entry, slow on Mali's driver). Now a set keeps its key; the same content
// next time this slot comes round is found as it is, with nothing written. A set is only rewritten for new content,
// and then one not used in this slot's last two rounds (else a new one, up to kPoolCap).
constexpr uint32_t kPoolCap = 2048;
struct Pool {
  struct Set {
    std::unique_ptr<plume::RenderDescriptorSet> set;
    Key key{};
    bool keyed = false;
    uint32_t stamp = 0;  // the slot's round it was last used in
    std::array<const void *, kSize * kMaxParts> written_a{};  // texture / sampler last written per entry
    std::array<const void *, kSize * kMaxParts> written_b{};  // view
    std::array<int, kSize * kMaxParts> written_layout{};
    // The mirror entry's generation last written: a texture freed and a new one made at the same address (common
    // while a stage loads its render targets) looks the same by pointer, and the set kept the dead image's view:
    // Mali-G52 lost the device ~1 s into the first stage (log of a Galaxy A14, 2026-11-16).
    std::array<uint32_t, kSize * kMaxParts> written_gen{};
    std::array<uint32_t, kSize * kMaxParts> written_slot{};
  };
  std::vector<Set> sets[kFramesInFlight];
  uint32_t round[kFramesInFlight] = {};  // this slot's round: starts at 2, above the sets' stamp 0 and its next
  uint32_t cursor[kFramesInFlight] = {};
  std::unordered_map<Key, uint32_t, KeyHash> cache[kFramesInFlight];  // key -> index in sets
  uint64_t made = 0;
  const char *what;
  explicit Pool(const char *w) : what(w) {
    for (uint32_t &r : round) r = 2;
  }
};

Pool g_textures("texture");
Pool g_samplers("sampler");
Pool g_combined("combined");

MirrorTable *Mirror(const std::unique_ptr<plume::RenderDescriptorSet> &set) {
  return static_cast<MirrorTable *>(set.get());
}

struct Part {
  bool samplers;
  const uint32_t *slots;
  uint32_t count;
  uint32_t pad;
};

// A set of the frame slot holding each part's `count` slots (padded with `pad`) at entries part * kSize.., made or
// found. `build` makes the set's layout.
// 2026-11-20: what this mode costs the render thread, logged every 10 s (a Mali-G52 phone's log answers where its
// 71 ms a frame go): the time in GetSet, its calls, and the sets it had to write.
struct CostLog {
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  double seconds = 0;
  uint64_t calls = 0, writes = 0, entries = 0;
};
CostLog g_cost;

struct CostScope {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~CostScope() {
    const auto now = std::chrono::steady_clock::now();
    g_cost.seconds += std::chrono::duration<double>(now - t0).count();
    ++g_cost.calls;
    if (now - g_cost.start >= std::chrono::seconds(10)) {
      RGPU_INFO("[compat] 10 s: {:.1f} ms in the small sets ({} lookups, {} sets written, {} entries written)",
                g_cost.seconds * 1000.0, g_cost.calls, g_cost.writes, g_cost.entries);
      g_cost = CostLog{};
      g_cost.start = now;
    }
  }
};

plume::RenderDescriptorSet *GetSet(VideoState &s, Pool &pool, const Part *parts, uint32_t nparts,
                                   void (*build)(plume::RenderDescriptorSetBuilder &)) {
  const CostScope cost_scope;
  uint32_t used_slots[kSize * kMaxParts];
  const MirrorTable::Entry *entries[kSize * kMaxParts];
  Key key{};
  for (uint32_t p = 0; p < nparts; ++p) {
    MirrorTable *mirror = Mirror(parts[p].samplers ? s.sampler_set : s.texture_set);
    if (!mirror)
      return nullptr;
    for (uint32_t i = 0; i < kSize; ++i) {
      const uint32_t at = p * kSize + i;
      uint32_t slot = i < parts[p].count ? parts[p].slots[i] : parts[p].pad;
      const MirrorTable::Entry *e = mirror->Get(slot);
      if (!e || (parts[p].samplers ? e->sampler == nullptr : e->texture == nullptr)) {
        slot = parts[p].pad;
        e = mirror->Get(slot);
      }
      used_slots[at] = slot;
      entries[at] = e;
      key[at] = uint64_t(slot) | (uint64_t(e ? e->generation : 0) << 32);
    }
  }
  const uint32_t frame = s.frame % kFramesInFlight;
  auto &cache = pool.cache[frame];
  auto &sets = pool.sets[frame];
  const uint32_t round = pool.round[frame];
  if (auto it = cache.find(key); it != cache.end()) {
    Pool::Set &found = sets[it->second];
    found.stamp = round;
    return found.set.get();
  }
  // A set to (re)write: one not used in this round or the last (its content is the least likely to come back),
  // else a new one; at the cap, any not used in this round (the GPU finished this slot's earlier rounds).
  uint32_t index = UINT32_MAX;
  const uint32_t n = uint32_t(sets.size());
  for (int pass = 0; pass < 2 && index == UINT32_MAX; ++pass) {
    if (pass == 1 && n < kPoolCap)
      break;
    for (uint32_t k = 0; k < n; ++k) {
      const uint32_t i = (pool.cursor[frame] + k) % n;
      const uint32_t stamp = sets[i].stamp;
      if (pass == 0 ? stamp + 1 < round : stamp != round) {
        index = i;
        pool.cursor[frame] = (i + 1) % n;
        break;
      }
    }
  }
  if (index == UINT32_MAX) {
    plume::RenderDescriptorSetBuilder builder;
    build(builder);
    Pool::Set fresh;
    fresh.set = builder.create(s.device.get());
    if (!fresh.set) {
      RGPU_ERROR("[compat] descriptor set creation failed ({} made)", pool.made);
      return nullptr;
    }
    fresh.written_layout.fill(-1);
    fresh.written_slot.fill(UINT32_MAX);
    sets.push_back(std::move(fresh));
    index = uint32_t(sets.size() - 1);
    if ((++pool.made & (pool.made - 1)) == 0)  // 1, 2, 4, 8...
      RGPU_INFO("[compat] {} {} sets made", pool.made, pool.what);
  }
  Pool::Set &target = sets[index];
  if (target.keyed)
    cache.erase(target.key);
  ++g_cost.writes;
  for (uint32_t i = 0; i < nparts * kSize; ++i) {
    const MirrorTable::Entry *e = entries[i];
    if (!e)
      continue;
    const bool samplers = parts[i / kSize].samplers;
    const bool same_source = target.written_slot[i] == used_slots[i] && target.written_gen[i] == e->generation;
    if (samplers) {
      if (!same_source || target.written_a[i] != e->sampler) {
        ++g_cost.entries;
        target.set->setSampler(i, e->sampler);
        target.written_a[i] = e->sampler;
      }
    } else if (!same_source || target.written_a[i] != e->texture || target.written_b[i] != e->view ||
               target.written_layout[i] != int(e->layout)) {
      ++g_cost.entries;
      target.set->setTexture(i, e->texture, e->layout, e->view);
      target.written_a[i] = e->texture;
      target.written_b[i] = e->view;
      target.written_layout[i] = int(e->layout);
    }
    target.written_slot[i] = used_slots[i];
    target.written_gen[i] = e->generation;
  }
  target.key = key;
  target.keyed = true;
  target.stamp = round;
  cache.emplace(key, index);
  return target.set.get();
}

}  // namespace

bool On() { return g_on; }
bool Packed() { return g_packed; }
bool NoInt64() { return g_no_int64; }
bool OldDriver() { return g_old_driver; }
bool Adreno() { return g_adreno; }
const char *Emulated() { return g_emulated.c_str(); }

void Decide(plume::RenderDevice *device) {
  const std::string mode = REXCVAR_GET(nier_bindless_compat);
  const std::string emulate = REXCVAR_GET(nier_emulate_gpu);
  g_emulated = IsVulkan() && emulate != "off" ? emulate : "";
  bool needed = false;
  bool int64 = true;
  if (device && IsVulkan()) {
    auto *vd = static_cast<plume::VulkanDevice *>(device);
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{};
    indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceBufferDeviceAddressFeatures address{};
    address.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    address.pNext = &indexing;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &address;
    vkGetPhysicalDeviceFeatures2(vd->physicalDevice, &features);
    VkPhysicalDeviceDescriptorIndexingProperties props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &props;
    vkGetPhysicalDeviceProperties2(vd->physicalDevice, &properties);
    const VkPhysicalDeviceLimits &l = properties.properties.limits;
    // The draw layout's three (four off Android) tables of kTextureDescriptorCount in every stage.
    const uint32_t tables = 4;
    needed = !indexing.runtimeDescriptorArray || !indexing.descriptorBindingPartiallyBound ||
             !indexing.descriptorBindingVariableDescriptorCount ||
             !indexing.descriptorBindingSampledImageUpdateAfterBind ||
             props.maxPerStageDescriptorUpdateAfterBindSampledImages < tables * kTextureDescriptorCount ||
             props.maxPerStageDescriptorUpdateAfterBindSamplers < kSamplerDescriptorCount;
    RGPU_INFO("[compat] GPU: driver 0x{:08X}, runtimeArray {} partiallyBound {} variableCount {} sampledImageUAB {}, "
              "UAB per stage {} images {} samplers; dynamic indexing {}; int64 {}; device address {}; per stage {} "
              "images {} samplers, {} sets",
              properties.properties.driverVersion, indexing.runtimeDescriptorArray,
              indexing.descriptorBindingPartiallyBound, indexing.descriptorBindingVariableDescriptorCount,
              indexing.descriptorBindingSampledImageUpdateAfterBind,
              props.maxPerStageDescriptorUpdateAfterBindSampledImages,
              props.maxPerStageDescriptorUpdateAfterBindSamplers, features.features.shaderSampledImageArrayDynamicIndexing,
              features.features.shaderInt64, address.bufferDeviceAddress, l.maxPerStageDescriptorSampledImages,
              l.maxPerStageDescriptorSamplers, l.maxBoundDescriptorSets);
    int64 = features.features.shaderInt64 != 0;
    g_old_driver = properties.properties.apiVersion < VK_API_VERSION_1_2;
    // An Adreno 7xx (2026-11-21: the holes on the Poco F6's 740; the Snapdragon 695's 619 has none).
    g_adreno = properties.properties.vendorID == 0x5143 &&
               std::string(properties.properties.deviceName).find("Adreno (TM) 7") != std::string::npos;
  }
  // The imitated phone's limits (nier_emulate_gpu): a Snapdragon 695's 4 sets and no int64; a Mali-G52's lack of
  // descriptor indexing.
  if (g_emulated == "adreno619") {
    int64 = false;
    g_old_driver = true;
    needed = true;
  } else if (g_emulated == "mali-g52") {
    needed = true;
  }
  g_no_int64 = IsVulkan() && !int64;
  if (!g_emulated.empty()) RGPU_INFO("[compat] nier_emulate_gpu {}: its limits on this GPU", g_emulated);
  // NieR (2026-11-16): a Snapdragon 695 (Adreno 619, driver 0x80212000) binds 4 sets; the draw layout has 7 on
  // Android (8 elsewhere): its pipeline layout failed and the game closed after "Renderer: Vulkan". Packed: one set
  // of the 2D, 3D, cube tables and the samplers, then the 3 constant sets. Per draw, so on top of the small sets.
  const std::string packed_mode = REXCVAR_GET(nier_packed_sets);
  uint32_t max_sets = 32;
  if (device && IsVulkan())
    max_sets = static_cast<plume::VulkanDevice *>(device)->physicalDeviceProperties.limits.maxBoundDescriptorSets;
  const uint32_t layout_sets = kDrawLayoutSets;
  if (g_emulated == "adreno619") max_sets = 4;
  g_packed = IsVulkan() && (packed_mode == "on" || (packed_mode == "auto" && max_sets < layout_sets));
  g_on = IsVulkan() && (g_packed || mode == "on" || (mode == "auto" && needed));
  plume::SetVulkanFixedDescriptorArrays(g_on ? kSize : 0);
  RGPU_INFO("[compat] nier_bindless_compat {}: {}", mode,
            g_on ? "small texture sets per draw (no bindless tables)" : "bindless tables");
  RGPU_INFO("[compat] nier_packed_sets {}: the GPU binds {} sets, the draw layout {}: {}", packed_mode, max_sets,
            g_packed ? 4u : layout_sets, g_packed ? "textures and samplers in one set" : "a set per table");
}

void BuildTextureTable(plume::RenderDescriptorSetBuilder &builder, uint32_t bindless_count) {
  builder.begin();
  if (g_on) {
    builder.addTexture(0, kSize);
    builder.end();
  } else {
    builder.addTexture(0, bindless_count);
    builder.end(true, bindless_count);
  }
}

void BuildSamplerTable(plume::RenderDescriptorSetBuilder &builder, uint32_t bindless_count) {
  builder.begin();
  if (g_on) {
    builder.addSampler(0, kSize);
    builder.end();
  } else {
    builder.addSampler(0, bindless_count);
    builder.end(true, bindless_count);
  }
}

void BuildCombinedTable(plume::RenderDescriptorSetBuilder &builder) {
  builder.begin();
  builder.addTexture(0, kSize);  // Texture2D[] (and Texture2DArray[] read through it)
  builder.addTexture(1, kSize);  // Texture3D[]
  builder.addTexture(2, kSize);  // TextureCube[]
  builder.addSampler(3, kSize);  // SamplerState[]
  builder.end();
}

std::unique_ptr<plume::RenderDescriptorSet> MakeMirror(bool, uint32_t count) {
  return std::make_unique<MirrorTable>(count);
}

void RecycleSlotLocked(VideoState &, uint32_t frame) {
  if (!g_on)
    return;
  frame %= kFramesInFlight;
  // The slot's sets are kept with their keys (a new round: the GPU is done with the earlier ones).
  for (Pool *pool : {&g_textures, &g_samplers, &g_combined})
    ++pool->round[frame];
}

namespace {
void BuildTextureKind(plume::RenderDescriptorSetBuilder &b) { BuildTextureTable(b, 0); }
void BuildSamplerKind(plume::RenderDescriptorSetBuilder &b) { BuildSamplerTable(b, 0); }
}  // namespace

plume::RenderDescriptorSet *TextureSetLocked(VideoState &s, const uint32_t *slots, uint32_t count) {
  const Part part{false, slots, count, kNullTextureSlot};
  return GetSet(s, g_textures, &part, 1, BuildTextureKind);
}

plume::RenderDescriptorSet *SamplerSetLocked(VideoState &s, const uint32_t *slots, uint32_t count) {
  const Part part{true, slots, count, kLinearClampSamplerSlot};
  return GetSet(s, g_samplers, &part, 1, BuildSamplerKind);
}

plume::RenderDescriptorSet *CombinedSetLocked(VideoState &s, const uint32_t *const textures[3], const uint32_t *samplers,
                                              uint32_t count) {
  const Part parts[4] = {{false, textures[0], count, kNullTextureSlot},
                         {false, textures[1], count, kNullTextureSlot},
                         {false, textures[2], count, kNullTextureSlot},
                         {true, samplers, count, kLinearClampSamplerSlot}};
  return GetSet(s, g_combined, parts, 4, BuildCombinedTable);
}

void BindUtilityLocked(VideoState &s, plume::RenderCommandList *cmd, std::initializer_list<uint32_t> textures,
                       uint32_t sampler, uint32_t set2_slot) {
  uint32_t slots[kSize];
  uint32_t n = 0;
  for (uint32_t t : textures)
    if (n < kSize)
      slots[n++] = t;
  if (plume::RenderDescriptorSet *set = TextureSetLocked(s, slots, n))
    cmd->setGraphicsDescriptorSet(set, 0);
  if (plume::RenderDescriptorSet *set = SamplerSetLocked(s, &sampler, 1))
    cmd->setGraphicsDescriptorSet(set, 1);
  if (set2_slot != UINT32_MAX) {
    if (plume::RenderDescriptorSet *set = TextureSetLocked(s, &set2_slot, 1))
      cmd->setGraphicsDescriptorSet(set, 2);
  }
}

}  // namespace renut::gpu::compat
