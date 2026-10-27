#pragma once

#include <array>
#include <cstdint>

namespace rex::ppc {

// vupkd3d128 NORMPACKED64. The input is the guest's low doubleword
// (host u64[0]); the returned words use the reversed host vector lane order.
// These are biased float *bits*, not conversions of the fields to float.
// PGR4 removes the bias and scales the mantissa in its position decoder.
constexpr std::array<uint32_t, 4> UnpackNormPacked64(uint64_t packed) {
  std::array<uint32_t, 4> result{};
  for (unsigned lane = 0; lane < 3; ++lane) {
    const uint32_t field = uint32_t(packed >> (lane * 20)) & 0xFFFFF;
    const int32_t signed_field = int32_t(field ^ 0x80000) - 0x80000;
    result[3 - lane] = uint32_t(0x40400000 + signed_field);
  }
  result[0] = 0x3F800000 | uint32_t(packed >> 60);
  return result;
}

}  // namespace rex::ppc
