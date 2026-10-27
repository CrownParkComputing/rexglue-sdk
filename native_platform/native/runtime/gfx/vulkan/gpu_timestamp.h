#pragma once

#include <cstdint>

namespace xn::gfx {

// Queue families may expose fewer than 64 significant timestamp bits. The
// measured batch must be shorter than that counter's wrap interval.
inline constexpr uint64_t GpuTimestampTicks(uint64_t begin, uint64_t end,
                                          uint32_t valid_bits) {
  if (!valid_bits || valid_bits > 64) return 0;
  const uint64_t delta = end - begin;
  return valid_bits == 64 ? delta : delta & ((uint64_t(1) << valid_bits) - 1);
}

}  // namespace xn::gfx
