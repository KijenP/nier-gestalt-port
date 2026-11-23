/**
 * @file    gpu/render/render_scale.h
 * @brief   Render resolution: host pixels per guest pixel.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace renut::gpu {

// Host pixels per guest pixel of a render target, and of what resolves from
// it, as one of a fixed set of ratios: the render resolutions
// (renut_resolution: 1x, 480p, 1080p, 1440p, 4K) and the targets that render
// finer than the rest (graphics_options.h: shadow maps at 4x, ambient
// occlusion at twice the resolution: 4/3, 6x). Code 0 is 1x, the scale of all
// content that is not rendered (guest textures). Shaders get the code in the
// descriptor index (kResolutionScaleShift) and have the same factors
// (renutTextureScale in shader_common.h).
// MGR package ABI: code 6 is half resolution (360p), replacing Banjo's unused 6x.
// The shader's renutTextureScale table uses the same code.
// NieR (2026-11-05): code 7 (4/3, finer shadow / AO maps, which NieR never renders) is the custom
// scale: any ratio, set by SetCustomRatio, for render resolutions off the table (180p, 640p, the
// PC menu's free resolution scale). Shaders read its factor from SharedConstants (1352,
// g_RenutCustomScale in shader_common.h).
struct RenderScale {
  static constexpr uint32_t kCount = 8;
  static constexpr uint32_t kCustomCode = 7;
  static constexpr uint32_t kNumerators[kCount] = {1, 2, 3, 2, 3, 4, 1, 4};
  static constexpr uint32_t kDenominators[kCount] = {1, 3, 2, 1, 1, 1, 2, 3};
  static inline uint32_t custom_numerator = 4, custom_denominator = 3;

  uint32_t code = 0;
  // The custom code's ratio when this scale was made (numerator << 16 | denominator; 0 for the
  // table's codes): a target or texture made at another custom ratio is another scale, so a live
  // change from one custom ratio to another rebuilds them instead of reusing them at the old size.
  uint32_t ratio = 0;

  static uint32_t Numerator(uint32_t c) { return c == kCustomCode ? custom_numerator : kNumerators[c]; }
  static uint32_t Denominator(uint32_t c) { return c == kCustomCode ? custom_denominator : kDenominators[c]; }
  static uint32_t CurrentRatio() { return custom_numerator << 16 | custom_denominator; }
  static RenderScale Custom() { return RenderScale{kCustomCode, CurrentRatio()}; }

  uint32_t Num() const { return code == kCustomCode && ratio ? ratio >> 16 : Numerator(code); }
  uint32_t Den() const { return code == kCustomCode && ratio ? ratio & 0xFFFF : Denominator(code); }

  // Reduced; the custom code then means numerator / denominator.
  static void SetCustomRatio(uint32_t numerator, uint32_t denominator) {
    uint32_t a = numerator, b = denominator;
    while (b) {
      const uint32_t t = a % b;
      a = b;
      b = t;
    }
    custom_numerator = numerator / a;
    custom_denominator = denominator / a;
  }

  float Factor() const { return float(Num()) / float(Den()); }
  bool operator==(const RenderScale &) const = default;

  // A guest pixel edge in host pixels, to the nearest: rects that share an
  // edge in guest pixels share it in host pixels.
  int32_t Coord(int32_t guest) const {
    return int32_t(std::lround(double(guest) * Num() / Den()));
  }

  // A guest size in host pixels, 1 at least.
  uint32_t Size(uint32_t guest) const {
    return std::max<uint32_t>(uint32_t(Coord(int32_t(guest))), 1u);
  }

  // The scale of ratio numerator / denominator, if it is one of them.
  static bool FromRatio(uint32_t numerator, uint32_t denominator, RenderScale &out) {
    for (uint32_t c = 0; c < kCount; ++c) {
      if (uint64_t(numerator) * Denominator(c) == uint64_t(Numerator(c)) * denominator) {
        out.code = c;
        out.ratio = c == kCustomCode ? CurrentRatio() : 0;
        return true;
      }
    }
    return false;
  }

  // This scale times 'multiplier', or this one when that is not among them.
  RenderScale Times(uint32_t multiplier) const {
    RenderScale out = *this;
    FromRatio(Num() * multiplier, Den(), out);
    return out;
  }
};

} // namespace renut::gpu
