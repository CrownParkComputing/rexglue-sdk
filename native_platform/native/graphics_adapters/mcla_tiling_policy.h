#pragma once
#include <cstdint>

namespace mcla::native {
// These modes are decoded by this title's BeginTiledRendering: 0/1 use
// 80x16, 2 uses 80x8, and 3 uses 40x8 sample tiles. Surfaces are padded
// to 160x32 before the title creates its colour and depth pair.
constexpr uint32_t MainSceneTileDemand(uint32_t width, uint32_t height, uint32_t mode) {
  if (!width || !height || width > 1280 || height > 720 || mode > 4) return 0;
  const uint64_t padded_width = (uint64_t(width) + 159) / 160 * 160;
  const uint64_t padded_height = (uint64_t(height) + 31) / 32 * 32;
  const uint32_t tx = mode >= 3 ? 40 : 80;
  const uint32_t ty = mode >= 2 ? 8 : 16;
  const uint64_t tiles = (padded_width / tx) * (padded_height / ty) * 2;
  return uint32_t(tiles);
}
constexpr bool UseSingleTile(bool enabled, uint32_t capacity, bool scene_target,
    uint32_t width, uint32_t height, uint32_t mode, uint32_t original_count) {
  const uint32_t required = MainSceneTileDemand(width, height, mode);
  return enabled && (capacity == 4096 || capacity == 8192) && scene_target &&
      original_count > 1 && required && required <= capacity;
}
constexpr bool SurfaceFits(uint32_t capacity, uint32_t base, uint32_t size, uint32_t end) {
  return (capacity == 4096 || capacity == 8192) && size && base < 4096 &&
      uint64_t(base) + size == end && end <= capacity;
}
}  // namespace mcla::native

// Call once after the renderer initializes and before starting guest threads.
void MclaNativeConfigureTiling(uint8_t* guest_base, uint32_t edram_tile_capacity, bool default_enabled = false);
void MclaNativeReportTiling();
extern "C" bool MclaNativePrepareTile(uint32_t target, uint32_t width, uint32_t height,
    uint32_t mode, uint32_t tile_width, uint32_t tile_height, uint8_t* guest_base);
extern "C" bool MclaNativeAllowSurface(uint32_t base, uint32_t size, uint32_t end);
