/**
 * @file    gpu/backend/bindless_compat.h
 * @brief   NieR (2026-11-15): the renderer on GPUs without descriptor indexing (the Mali Bifrost: G52, G72, G76...).
 *
 * The renderer's textures and samplers live in two bindless tables (8,192 and 256 slots) that every shader indexes.
 * A GPU without descriptor indexing (runtime arrays, partially bound and update-after-bind sets) cannot have them: a
 * Mali-G52 created them without a word and hung in the first draw. In this mode the tables are kept on the CPU only
 * (MirrorTable: the same setTexture / setSampler calls), the shaders' unsized arrays become arrays of kSize
 * (plume::SetVulkanFixedDescriptorArrays), and each draw binds small sets that hold the slots it reads at entries
 * 0..kSize-1, the shader's indices rewritten to those entries. The sets are made once per frame slot and content
 * (cached), and reused once the GPU is done with that frame.
 *
 * nier_bindless_compat: "auto" (the mode where the GPU needs it), "on" (forced: a test on any GPU), "off".
 */
#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

namespace renut::gpu {
struct VideoState;
}

namespace renut::gpu::compat {

// Entries of each small set: SharedConstants holds 32 fetch constants per texture kind and 32 sampler words.
constexpr uint32_t kSize = 32;

// The draw layout's sets: the 2D, 3D, cube tables, the samplers (and off Android the 2D-array table), then the
// three constant buffers.
#if defined(__ANDROID__)
constexpr uint32_t kDrawLayoutSets = 7;
#else
constexpr uint32_t kDrawLayoutSets = 8;
#endif

// Decided once, after the device is made and before any shader or layout (CreateHostDevice). Logs the GPU's answer.
void Decide(plume::RenderDevice *device);
bool On();
// NieR (2026-11-16): the 4-set draw layout (GPUs that bind fewer sets than kDrawLayoutSets; implies On()): set 0
// holds the 2D, 3D, cube textures and the samplers at bindings 0-3 (CombinedSetLocked), sets 1-3 the constants;
// the guest shaders' sets are renumbered as they load (shader_library.cpp).
bool Packed();
// NieR (2026-11-19): the GPU (or the one nier_emulate_gpu stands for) has no 64-bit integers in shaders
// (shaderInt64 0: Adreno 619). Shaders that need them are reported as they load (shader_library.cpp).
bool NoInt64();
// A Vulkan 1.1 driver (Adreno 619's 1.1.128; or nier_emulate_gpu=adreno619). Newer Adrenos report shaderInt64 0 as
// well but run the 64-bit host shaders (the Adreno 740 always did).
bool OldDriver();
// A Qualcomm Adreno of the 7 series (Vulkan vendor 0x5143, "Adreno (TM) 7..."): its bilinear scan-out shows holes.
bool Adreno();
// The GPU nier_emulate_gpu imitates ("" when off): its limits applied here, to find a phone's faults on a PC.
const char *Emulated();
void BuildCombinedTable(plume::RenderDescriptorSetBuilder &builder);
// A combined set: the three texture kinds' slots and the samplers', `count` each (the rest padded), for this frame.
plume::RenderDescriptorSet *CombinedSetLocked(VideoState &s, const uint32_t *const textures[3], const uint32_t *samplers,
                                              uint32_t count);

// The table builders every layout and the bindless sets use: kSize entries in this mode, the bindless table else.
void BuildTextureTable(plume::RenderDescriptorSetBuilder &builder, uint32_t bindless_count);
void BuildSamplerTable(plume::RenderDescriptorSetBuilder &builder, uint32_t bindless_count);

// This mode's s.texture_set / s.sampler_set: CPU tables (no GPU set behind them; never bound).
std::unique_ptr<plume::RenderDescriptorSet> MakeMirror(bool samplers, uint32_t count);

// The frame slot's sets are free again (the GPU finished it): DrainSlot.
void RecycleSlotLocked(VideoState &s, uint32_t frame);

// A set holding these slots at entries 0..n-1 (the rest the null texture / the linear sampler), for this frame.
plume::RenderDescriptorSet *TextureSetLocked(VideoState &s, const uint32_t *slots, uint32_t count);
plume::RenderDescriptorSet *SamplerSetLocked(VideoState &s, const uint32_t *slots, uint32_t count);

// The utility layout's sets 0 and 1 (and 2, the depth resolve's stencil view) for one draw: the texture slots at
// entries 0.., the sampler at 0, set 2's slot at 0. The caller's push indices become those entries.
void BindUtilityLocked(VideoState &s, plume::RenderCommandList *cmd, std::initializer_list<uint32_t> textures,
                       uint32_t sampler, uint32_t set2_slot = UINT32_MAX);

}  // namespace renut::gpu::compat
