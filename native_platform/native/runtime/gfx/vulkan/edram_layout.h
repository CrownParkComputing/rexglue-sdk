#pragma once

#include <cstdint>

namespace xn::gfx {

// The host can track up to four 10 MiB banks for titles that require an
// extended virtual EDRAM address range. This does not change Xenos hardware
// constants or the 12-bit render-target base registers.
constexpr uint32_t kEdramTileBytes = 80 * 16 * 4;
constexpr uint32_t kDefaultEdramTileCapacity = 2048;
constexpr uint32_t kMaxVirtualEdramTileCapacity = 8192;

constexpr uint32_t SelectVirtualEdramMiB(uint32_t configured_mib,
                                          const char* override_value) {
  if (override_value && override_value[0] == '0' && !override_value[1]) return 10;
  if (override_value && override_value[0] == '1' && !override_value[1]) return 20;
  if (override_value && override_value[0] == '4' && override_value[1] == '0' &&
      !override_value[2]) return 40;
  return configured_mib == 40 ? 40 : configured_mib == 20 ? 20 : 10;
}

constexpr uint32_t VirtualEdramTileCapacity(uint32_t mib) {
  return mib == 40 ? 8192 : mib == 20 ? 4096 : kDefaultEdramTileCapacity;
}

// RB_COLOR_INFO and RB_DEPTH_INFO expose bit 11 separately because physical
// Xenos EDRAM wraps at 2048 tiles. Extended virtual EDRAM retains that bit.
constexpr uint32_t VirtualEdramBase(uint32_t register_value, uint32_t capacity) {
  return register_value & (capacity == kDefaultEdramTileCapacity ? 2047u : 4095u);
}

// The image copy reserves complete 80x16 tiles. Use wide arithmetic so an
// invalid guest pitch or height cannot wrap into an apparently valid range.
constexpr bool EdramTileRangeFits(uint32_t base, uint32_t pitch,
                                  uint32_t height, uint32_t capacity) {
  if (!pitch || !height || base >= capacity) return false;
  const uint64_t pitch_tiles = (uint64_t(pitch) + 79) / 80;
  const uint64_t height_tiles = (uint64_t(height) + 15) / 16;
  return pitch_tiles * height_tiles <= uint64_t(capacity) - base;
}

}  // namespace xn::gfx
