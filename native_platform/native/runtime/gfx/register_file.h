#pragma once
#include <cstdint>

namespace xn {
struct GpuRegisterFile {
  static constexpr uint32_t kCount = 0x5003;
  uint32_t values[kCount] = {};
  uint32_t operator[](uint32_t i) const { return i < kCount ? values[i] : 0; }
};
}  // namespace xn
