#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace rex::graphics::draw_util {

// Address calculation for the two-export, 64-byte copy shader. Each vertex
// writes eight 8-byte elements, starting at 8 * (vertex_index + c68.x).
// The caller must verify the shader and its other constants first.
inline std::optional<std::pair<uint32_t, uint32_t>> GetMemExportCopyOffset(
    uint32_t vertex_first, uint32_t vertex_count, float destination_offset,
    uint32_t stream_element_count) {
  if (!vertex_count || !std::isfinite(destination_offset) ||
      std::trunc(destination_offset) != destination_offset) {
    return std::nullopt;
  }
  // Restrict to exactly representable integer arithmetic in the guest float
  // shader, including the 23-bit element index in eA.y. Never guess at wrapping.
  double first = double(vertex_first) + double(destination_offset);
  double end = first + double(vertex_count);
  if (vertex_first > (1u << 24) ||
      uint64_t(vertex_first) + vertex_count > (1u << 24) ||
      std::abs(double(destination_offset)) > (1u << 24) || first < 0 ||
      end * 8 > stream_element_count || end * 8 > (1u << 23)) {
    return std::nullopt;
  }
  return std::make_pair(uint32_t(first) * 64, vertex_count * 64);
}

}  // namespace rex::graphics::draw_util
