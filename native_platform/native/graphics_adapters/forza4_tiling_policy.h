#pragma once

#include <cstdint>

namespace forza4::native {

constexpr uint32_t kSceneTableAddress = 0x82D883C0;
constexpr uint32_t kSceneTableStride = 1232;
constexpr uint32_t kSceneTableEntries = 5;
constexpr uint32_t kPairedSceneTileDemand = 2880;

// The audited constructor uses 80x8 tiles at 2x MSAA. Its full-height
// colour and depth pair needs (1280 / 80) * (720 / 8) * 2 tiles.
constexpr bool EligibleScene(bool enabled, uint32_t capacity, uint32_t index,
                             uint32_t width, uint32_t height, uint32_t mode,
                             uint32_t count, uint32_t x0, uint32_t y0,
                             uint32_t x1, uint32_t y1,
                             uint32_t next_x0, uint32_t next_y0,
                             uint32_t next_x1, uint32_t next_y1) {
  return enabled && (capacity == 4096 || capacity == 8192) &&
         kPairedSceneTileDemand <= capacity && index < kSceneTableEntries &&
         width == 1280 && height == 720 && mode == 1 && count == 2 &&
         x0 == 0 && y0 == 0 && x1 == 1280 && y1 == 512 &&
         next_x0 == 0 && next_y0 == 512 &&
         next_x1 == 1280 && next_y1 == 720;
}

}  // namespace forza4::native

// Configure once after the renderer is initialized and before guest threads.
void Forza4NativeConfigureTiling(uint8_t* guest_base, uint32_t edram_tile_capacity,
                                 bool default_enabled = false);
void Forza4NativeReportTiling();
extern "C" void Forza4NativePrepareTiling(uint32_t index, uint8_t* guest_base);
