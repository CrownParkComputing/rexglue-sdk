#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace xn::gfx {

// Offsets refer to immutable uploads in the CURRENT command-buffer batch.
// Reset before its ring segment is recycled, including mid-frame submissions.
// Compare CPU storage, never write-combined mapped Vulkan memory. Exact bytes
// also cover snapshot/Shape A callers that bypass register dirty tracking.
template <size_t Capacity>
class ConstantUploadCache {
 public:
  void Reset() { valid_ = false; }
  bool Find(const void* data, size_t size, uint32_t& offset) const {
    if (!valid_ || size != size_ || std::memcmp(data_.data(), data, size)) return false;
    offset = offset_;
    return true;
  }
  void Remember(const void* data, size_t size, uint32_t offset) {
    valid_ = size <= Capacity;
    if (!valid_) return;
    std::memcpy(data_.data(), data, size);
    size_ = size;
    offset_ = offset;
  }

 private:
  std::array<uint8_t, Capacity> data_{};
  size_t size_ = 0;
  uint32_t offset_ = 0;
  bool valid_ = false;
};

}  // namespace xn::gfx
