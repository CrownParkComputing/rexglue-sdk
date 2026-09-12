#pragma once

#include <cstdint>

namespace rex::graphics::util {

// Split/Second's CPU vehicle compositor locks these six complete RGBA surfaces.
// Its tiled shadow resolves can have the same pitch/height but only write half
// the surface; accepting those would reintroduce per-frame GPU synchronization.
constexpr bool IsSplitSecondCompositorResolve(uint32_t pitch, uint32_t height,
                                             uint32_t format, uint32_t length) {
  return format == 6 && pitch >= 32 && pitch <= 1024 &&
         (pitch & (pitch - 1)) == 0 && height == pitch * 2 &&
         uint64_t(length) == uint64_t(pitch) * height * 4;
}

}  // namespace rex::graphics::util
