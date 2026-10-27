#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include "index_expand.h"

namespace xn::gfx {
struct VertexIndexRange {
  uint32_t first = UINT32_MAX, last = 0;
};

inline bool ScaleVertexIndexRange(VertexIndexRange& range, float scale) {
  if (range.first > range.last || range.last >= (1u << 23) || !std::isfinite(scale) || scale < 0)
    return false;
  const double first = double(range.first) * scale, last = double(range.last) * scale;
  if (!std::isfinite(last) || last >= (1u << 23)) return false;
  // Enclose float32 multiplication and fetch rounding in either direction.
  // Below 2^23, one extra vertex at each end covers the rounding error.
  range.first = uint32_t(std::max(0.0, std::floor(first) - 1.0));
  range.last = uint32_t(std::ceil(last) + 1.0);
  return true;
}

// Index conversion already visits every submitted index. Collect extrema in
// that pass, before applying the common base/clamp, instead of re-reading the
// guest buffer and repeating endian conversion for a separate bounds scan.
template <typename Index, bool CollectRange, rex::graphics::xenos::Endian Endian>
VertexIndexRange CopyVertexIndicesTyped(Index* output, const uint8_t* input, uint32_t count) {
  using namespace rex::graphics::native;
  VertexIndexRange range;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t value;
    if constexpr (sizeof(Index) == 4) value = ConvertIndex32(input + size_t(i) * 4, Endian);
    else value = ConvertIndex16(input + size_t(i) * 2, Endian);
    output[i] = Index(value);
    if constexpr (CollectRange) {
      range.first = std::min(range.first, value);
      range.last = std::max(range.last, value);
    }
  }
  return range;
}

template <typename Index, bool CollectRange>
VertexIndexRange CopyVertexIndices(Index* output, const uint8_t* input, uint32_t count,
                                  rex::graphics::xenos::Endian endian) {
  using E = rex::graphics::xenos::Endian;
  // As with restarting strips, dispatch once so the copy and min/max reduction
  // can use SIMD. In particular the common 16-bit path otherwise performs a
  // conditional byte swap for every index.
  if constexpr (sizeof(Index) == 2) {
    if (rex::graphics::native::IndexEndianSwaps(endian))
      return CopyVertexIndicesTyped<Index,CollectRange,E::k8in16>(output,input,count);
    return CopyVertexIndicesTyped<Index,CollectRange,E::kNone>(output,input,count);
  } else {
    switch (endian) {
      case E::k8in16:
        return CopyVertexIndicesTyped<Index,CollectRange,E::k8in16>(output,input,count);
      case E::k8in32:
        return CopyVertexIndicesTyped<Index,CollectRange,E::k8in32>(output,input,count);
      case E::k16in32:
        return CopyVertexIndicesTyped<Index,CollectRange,E::k16in32>(output,input,count);
      default:
        return CopyVertexIndicesTyped<Index,CollectRange,E::kNone>(output,input,count);
    }
  }
}

inline VertexIndexRange RemapVertexIndices(VertexIndexRange range, uint32_t base,
                                          uint32_t minimum, uint32_t maximum) {
  constexpr uint32_t mask = rex::graphics::xenos::kVertexIndexMask;
  const uint64_t lo = uint64_t(range.first) + (base & mask);
  const uint64_t hi = uint64_t(range.last) + (base & mask);
  // A range crossing any 24-bit wrap may contain both extremes. Enclose the
  // whole interval rather than incorrectly mapping only the two endpoints.
  const bool wraps = (lo >> 24) != (hi >> 24);
  return {std::clamp(wraps ? 0u : uint32_t(lo) & mask, minimum, maximum),
          std::clamp(wraps ? mask : uint32_t(hi) & mask, minimum, maximum)};
}

// Use 32-bit host indices for restarting strips even for a 16-bit guest
// buffer: a custom guest reset value must not make a real 0xFFFF vertex into
// Vulkan's fixed reset marker. Xenos checks the low 24 bits before base/clamp.
template <bool CollectRange, unsigned InputBits, rex::graphics::xenos::Endian Endian>
VertexIndexRange CopyRestartVertexIndicesTyped(uint32_t* output, const uint8_t* input,
                                              uint32_t count, uint32_t reset_index) {
  using namespace rex::graphics::native;
  VertexIndexRange range;
  constexpr uint32_t mask = rex::graphics::xenos::kVertexIndexMask;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t value;
    if constexpr (InputBits == 32) value = ConvertIndex32(input + size_t(i)*4, Endian) & mask;
    else if constexpr (InputBits == 16) value = ConvertIndex16(input + size_t(i)*2, Endian);
    else value = i & mask;
    const bool restart = value == reset_index;
    const uint32_t converted = restart ? UINT32_MAX : value;
    output[i] = converted;
    if constexpr (CollectRange) {
      range.first = std::min(range.first, converted);
      range.last = std::max(range.last, restart ? 0 : value);
    }
  }
  return range;
}

template <bool CollectRange>
VertexIndexRange CopyRestartVertexIndices(uint32_t* output,
                                         const rex::graphics::native::GuestIndexSource& source,
                                         uint32_t count, uint32_t reset_index) {
  using E = rex::graphics::xenos::Endian;
  // Choose source width and endian once per draw. Constant conversion modes
  // and branchless reset/range updates let the compiler vectorize this loop.
  if (!source.data)
    return CopyRestartVertexIndicesTyped<CollectRange,0,E::kNone>(output,nullptr,count,reset_index);
  if (!source.is_32bit) {
    if (rex::graphics::native::IndexEndianSwaps(source.endianness))
      return CopyRestartVertexIndicesTyped<CollectRange,16,E::k8in16>(output,source.data,count,reset_index);
    return CopyRestartVertexIndicesTyped<CollectRange,16,E::kNone>(output,source.data,count,reset_index);
  }
  switch (source.endianness) {
    case E::k8in16:
      return CopyRestartVertexIndicesTyped<CollectRange,32,E::k8in16>(output,source.data,count,reset_index);
    case E::k8in32:
      return CopyRestartVertexIndicesTyped<CollectRange,32,E::k8in32>(output,source.data,count,reset_index);
    case E::k16in32:
      return CopyRestartVertexIndicesTyped<CollectRange,32,E::k16in32>(output,source.data,count,reset_index);
    default:
      return CopyRestartVertexIndicesTyped<CollectRange,32,E::kNone>(output,source.data,count,reset_index);
  }
}
} // namespace xn::gfx
