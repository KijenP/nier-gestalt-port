/**
 * @file    gpu/render/graphics_options.cpp
 */
#include "renut_engine/gpu/render/graphics_options.h"

#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include <rex/logging.h>

#include <rex/cvar.h>
#include <rex/types.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/guest/widescreen.h"
#include "renut_engine/gpu/render/format.h"

// NieR (2026-11-12): "1x" by default: the game's shadow filter steps in console texels, so a larger map shows
// checkered, unsmoothed shadow edges (seen at 4K in the junkyard; 720p looks right).
REXCVAR_DEFINE_STRING(renut_shadow_resolution, "1x", "Nuts&Bolts/Graphics",
                      "Shadow map resolution, as a multiple of the console's: Resolution follows "
                      "renut_resolution; 1x is the console's; 2x, 3x and 4x are sharper.")
    .allowed({"Resolution", "1x", "2x", "3x", "4x"});

REXCVAR_DEFINE_STRING(renut_ambient_occlusion, "Half", "Nuts&Bolts/Graphics",
                      "Ambient occlusion (SSAO) resolution: Half the screen's, as on the console, "
                      "or Full.")
    .allowed({"Half", "Full"});

REXCVAR_DEFINE_STRING(renut_anisotropic_filtering, "Game", "Nuts&Bolts/Graphics",
                      "Anisotropic texture filtering: the game's own, or at least 2x, 4x, 8x or "
                      "16x.")
    .allowed({"Game", "2x", "4x", "8x", "16x"});

// sa2-nx (2026-10-20): "Game" by default: SA2's picture stays the console's (16-bit buffers change it).
REXCVAR_DEFINE_STRING(renut_post_process_precision, "Game", "Nuts&Bolts/Graphics",
                      "Precision of the bloom, depth of field and SSAO buffers: Game keeps the "
                      "console's 8 bits (faint glow shows as blobs sliding over dark areas), High "
                      "uses 16 bits.")
    .allowed({"Game", "High"});

namespace renut::gpu {

namespace {

// D3DFORMAT low bits of D3DFMT_D24S8 (the shadow maps; the scene's depth is
// D24FS8).
constexpr u32 kFormatD24S8 = 22;
// The SSAO targets' EDRAM base (sub_8235FA90).
constexpr u32 kAmbientOcclusionEdramBase = 1864;
// The game's screen height (widescreen.h).
constexpr u32 kGuestScreenHeight = 720;

bool IsShadowMap(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  if (width != height)
    return false;
  if ((u32(desc.Format) & 0x3F) == kFormatD24S8)
    return width == 512 || width == 768 || width == 1024;
  // The local lights' depth, converted for their atlas.
  return !d3d::IsDepthFormat(desc.Format) && width == 512 &&
         GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF) == GpuColorFormat::Fmt16_16_Float;
}

bool IsAmbientOcclusion(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  return !d3d::IsDepthFormat(desc.Format) && width == 640 && height == 360 &&
         (u32(desc.ColorInfo) & 0xFFF) == kAmbientOcclusionEdramBase;
}

// A quarter of the screen or less: the post-processing buffers (depth of
// field and bloom, 320x180 down to 20x11 at 1280x720). Their blurs average a
// few taps 2 of their own texels apart, made for buffers box-downsampled from
// the screen; rendered at a higher scale, the buffers keep detail those taps
// step over, so edges come out as ghost copies rather than blurred.
bool IsPostProcessBuffer(const d3d::SurfaceDesc &desc, u32 width, u32 height) {
  return !d3d::IsDepthFormat(desc.Format) && width * 4 <= GuestScreenWidth() &&
         height * 4 <= kGuestScreenHeight;
}

// Half the screen or less, single-sampled, 8_8_8_8: the bloom's bright pass
// (640x360) and the post-processing buffers below it, and the SSAO targets.
bool IsLowPrecisionPostTarget(const d3d::SurfaceDesc &desc) {
  const u32 width = d3d::SurfaceWidth(desc.Size);
  const u32 height = d3d::SurfaceHeight(desc.Size);
  return !d3d::IsDepthFormat(desc.Format) && ((u32(desc.SurfaceInfo) >> 16) & 3) == 0 &&
         width * 2 <= GuestScreenWidth() && height * 2 <= kGuestScreenHeight &&
         GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF) == GpuColorFormat::Fmt8_8_8_8;
}

// The options as LatchGraphicsOptionsLocked last read them: the draw path
// asks for every target and sampler of every draw, too often for the
// settings' strings.
struct Latched {
  bool shadow_follows_resolution = true;
  RenderScale shadow_scale;
  bool full_ambient_occlusion = false;
  uint32_t min_anisotropy = 1;
  bool high_precision_post = true;
};
Latched g_latched;

} // namespace

void LatchGraphicsOptionsLocked() {
  const std::string &shadows = REXCVAR_GET(renut_shadow_resolution);
  g_latched.shadow_follows_resolution = shadows == "Resolution" || shadows.empty();
  g_latched.shadow_scale = {};
  if (!g_latched.shadow_follows_resolution)
    RenderScale::FromRatio(uint32_t(std::atoi(shadows.c_str())), 1, g_latched.shadow_scale);
  g_latched.full_ambient_occlusion = REXCVAR_GET(renut_ambient_occlusion) == "Full";
  // banjo-nx (2026-10-18): deko3d draws every target at the console's scale (its shaders are made for 1x, pass scale1
  // of tools/deko3d_glsl_opt.py): shadow maps follow the resolution (1x) and the ambient occlusion is the game's.
  // sa2-nx (1.0.3.1): kept: on deko3d the shadow maps follow the render resolution (1.5x at 1080p docked, with the
  // package made without scale1) and the ambient occlusion is the game's, SA2's defaults on every back end.
  if (IsDeko3D()) {
    g_latched.shadow_follows_resolution = true;
    g_latched.shadow_scale = {};
    g_latched.full_ambient_occlusion = false;
  }
  const int anisotropy = std::atoi(REXCVAR_GET(renut_anisotropic_filtering).c_str());
  g_latched.min_anisotropy = anisotropy >= 2 ? uint32_t(anisotropy) : 1;
  g_latched.high_precision_post = REXCVAR_GET(renut_post_process_precision) != "Game";
}

RenderScale SurfaceRenderScale(const d3d::SurfaceDesc &desc, RenderScale resolution) {
  // MGR: no evidence identifies N&B's SSAO EDRAM base 1864, its shadow-map
  // dimensions or its post-process targets in this game. Preserve the guest
  // targets' relative sizes instead of applying another game's heuristics.
  // NieR TEMP (2026-11-12): checkered shadows/effects at 4K. NIER_SURFDBG logs each new target size;
  // NIER_SHADOW1X keeps square targets (shadow maps) at the console's size; NIER_EFFECTS1X keeps every target
  // narrower than the 1280 screen at the console's size.
  const u32 w = d3d::SurfaceWidth(desc.Size), h = d3d::SurfaceHeight(desc.Size);
  static const bool surf_dbg = std::getenv("NIER_SURFDBG") != nullptr;
  if (surf_dbg) {
    static std::mutex m;
    static std::set<uint64_t> seen;
    const uint64_t key = (uint64_t(w) << 48) | (uint64_t(h) << 32) | u32(desc.Format);
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert(key).second)
      REXLOG_INFO("[surf] {}x{} d3dfmt 0x{:X} info 0x{:08X} depth {}", w, h, u32(desc.Format), u32(desc.ColorInfo),
                  d3d::IsDepthFormat(desc.Format));
  }
#if !defined(__ANDROID__)
  // NieR (2026-11-12): the game's filters step in console texels, so above 720p the shadow map and the effect
  // targets (640x360 pass, bloom chain, all narrower than the 1280 screen) showed checkered, unsmoothed edges at 4K
  // (junkyard). Above 1x they stay at the console's size; below 1x they follow the resolution as before.
  // The shadow map (a square depth target, 1024x1024) follows renut_shadow_resolution (1x by default).
  if (resolution.Factor() > 1.0f) {
    if (w == h && w >= 256 && d3d::IsDepthFormat(desc.Format)) {
      if (!g_latched.shadow_follows_resolution && g_latched.shadow_scale.Num() != 0)
        return g_latched.shadow_scale;
    } else if (w <= 640 && std::getenv("NIER_EFFECTS_SCALED") == nullptr) {
      // 640 and narrower only (2026-11-14): the 3D scene itself is narrower than 1280 at the lower 3D sharpness levels
      // (768x432 at Minimum, 60%), and kept at 1x it showed as less than 720p at 4K.
      RenderScale one{};
      RenderScale::FromRatio(1, 1, one);
      return one;
    }
  }
#endif
  // NieR (2026-11-14): below half the console's resolution (360p under 100%, 180p) the effect targets (640x360 pass
  // with its depth, the bloom chain 512..2) went too small: the scene washed out white (360p at 80%, 3/8) or too
  // bright (50%). They stay at half (as at 360p 100%, which is right) at least. The shadow map follows as before.
  const bool shadow_map = w == h && w >= 1024 && d3d::IsDepthFormat(desc.Format);
  if (resolution.Factor() < 0.5f && w < 1280 && !shadow_map) {
    RenderScale half{};
    RenderScale::FromRatio(1, 2, half);
    return half;
  }
  return resolution;
}

plume::RenderFormat SurfaceColorFormat(const d3d::SurfaceDesc &desc) {
  // MGR's format is the one in its resource header. N&B's size-based format
  // override is not justified for MGR's post effects.
  const u32 fmt = (u32(desc.ColorInfo) >> 16) & 0xF;
  // NieR TEMP (2026-11-12): env NIER_RTFMTDBG=1 logs each new (format, EDRAM base) of a color target once.
  static const bool dbg = std::getenv("NIER_RTFMTDBG") != nullptr;
  if (dbg) {
    static std::mutex m;
    static std::set<u32> seen;
    const u32 key = (fmt << 16) | (u32(desc.ColorInfo) & 0xFFF);
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert(key).second)
      REXLOG_INFO("[rtfmt] fmt {} edram 0x{:X} color_info 0x{:08X}", fmt, u32(desc.ColorInfo) & 0xFFF,
                  u32(desc.ColorInfo));
  }
  return ColorTargetFormat(GpuColorFormat(fmt));
}

uint32_t MinAnisotropy() {
  return g_latched.min_anisotropy;
}

} // namespace renut::gpu
