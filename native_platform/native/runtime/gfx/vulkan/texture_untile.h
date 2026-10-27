/**
 ******************************************************************************
 * xenon-native - Xenos 2D tiled texture address helpers
 ******************************************************************************
 * Copied from ReXGlue src/graphics/pipeline/texture/conversion.cpp (Xenia,
 * BSD licence), which credits
 * https://github.com/BinomialLLC/crunch/blob/ea9b8d8c/inc/crn_decomp.h#L4108
 */
#pragma once
#include <cstdint>

namespace xn::gfx::untile {

// Xenos packs every mip level whose smallest dimension is <= 16 into a single
// 32x32 tile, side by side. For a texture that is itself that small (e.g. the
// 16x64 glow strip Geometry Wars draws its particles with) the BASE level is
// packed too, at an offset inside the tile - reading it at (0,0) yields the
// neighbouring smaller levels instead, which is mostly empty space.
// Copied from ReXGlue src/graphics/pipeline/texture/util.cpp
// (GetPackedMipOffset; Xenia, BSD licence) with its derivation comment.
//
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// 0         +.4x4.+ +.....8x8.....+ +............16x16............+
// 1         +.4x4.+ +.....8x8.....+ +............16x16............+
// 4 x               +.....8x8.....+ +............16x16............+
// 8 2x2                             +............16x16............+
//
// Returns false (and zero offsets) when the level is not packed.
inline uint32_t Log2Ceil(uint32_t v) {
  uint32_t r = 0;
  while ((uint32_t(1) << r) < v) ++r;
  return r;
}

inline bool GetPackedMipOffset(uint32_t width, uint32_t height, uint32_t mip,
                               uint32_t block_width, uint32_t block_height, uint32_t& x_blocks,
                               uint32_t& y_blocks) {
  const uint32_t log2_width = Log2Ceil(width);
  const uint32_t log2_height = Log2Ceil(height);
  const uint32_t log2_size = log2_width < log2_height ? log2_width : log2_height;
  if (log2_size > 4 + mip) {
    x_blocks = 0;
    y_blocks = 0;
    return false;  // the shortest dimension is bigger than 16: not packed
  }
  const uint32_t packed_mip_base = (log2_size > 4) ? (log2_size - 4) : 0;
  const uint32_t packed_mip = mip - packed_mip_base;
  if (packed_mip < 3) {
    if (log2_width > log2_height) {
      x_blocks = 0;                 // wider than tall: laid out vertically
      y_blocks = 16u >> packed_mip;
    } else {
      x_blocks = 16u >> packed_mip;  // taller than wide: laid out horizontally
      y_blocks = 0;
    }
  } else {
    if (log2_width > log2_height) {
      x_blocks = (1u << (log2_width - packed_mip_base)) >> (packed_mip - 2);
      y_blocks = 0;
    } else {
      x_blocks = 0;
      y_blocks = (1u << (log2_height - packed_mip_base)) >> (packed_mip - 2);
    }
  }
  x_blocks /= block_width;
  y_blocks /= block_height;
  return true;
}

inline uint32_t TiledOffset2DRow(uint32_t y, uint32_t width, uint32_t log2_bpp) {
  uint32_t macro = ((y / 32) * (width / 32)) << (log2_bpp + 7);
  uint32_t micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) +
         ((y & 1) << 4);
}

inline uint32_t TiledOffset2DColumn(uint32_t x, uint32_t y, uint32_t log2_bpp,
                                    uint32_t base_offset) {
  uint32_t macro = (x / 32) << (log2_bpp + 7);
  uint32_t micro = (x & 7) << log2_bpp;
  uint32_t offset = base_offset + (macro + ((micro & ~0xF) << 1) + (micro & 0xF));
  return ((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

// Xenia/ReXGlue GetTiledAddressUpperBound2D (BSD licence). Small R8/R8G8
// tiles spread across memory banks beyond their nominal texel byte count.
inline uint64_t TiledExtent2D(uint32_t right, uint32_t bottom, uint32_t pitch,
                              uint32_t log2_bpp) {
  if (!right || !bottom) return 0;
  const uint32_t x = (right - 1) & ~31u, y = (bottom - 1) & ~31u;
  const uint64_t origin = TiledOffset2DColumn(x, y, log2_bpp,
      TiledOffset2DRow(y, pitch, log2_bpp));
  return origin + (log2_bpp == 0 ? 0xA00u : log2_bpp == 1 ? 0xC00u : uint64_t(0x400) << log2_bpp);
}

// XGRAPHICS::TileVolume layout, as reconstructed in Xenia / ReXGlue's
// graphics/pipeline/texture/util.cpp (BSD licence). 3D tiles are 32x32x4;
// consecutive Z slices are interleaved rather than independent 2D textures.
inline uint32_t TiledOffset3D(uint32_t x, uint32_t y, uint32_t z,
                              uint32_t pitch, uint32_t height, uint32_t log2_bpp) {
  pitch = (pitch + 31) & ~31u;
  height = (height + 31) & ~31u;
  const uint32_t macro_outer = ((y >> 4) + (z >> 2) * (height >> 4)) * (pitch >> 5);
  const uint32_t macro = ((((x >> 5) + macro_outer) << (log2_bpp + 6)) & 0xFFFFFFF) << 1;
  const uint32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bpp + 6)) >> 6;
  const uint32_t outer = ((y >> 3) + (z >> 2)) & 1;
  const uint32_t offset1 = outer + ((((x >> 3) + (outer << 1)) & 3) << 1);
  const uint32_t offset2 = ((macro + (micro & ~15u)) << 1) + (micro & 15) +
                           ((z & 3) << (log2_bpp + 6)) + ((y & 1) << 4);
  uint32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address = (address << 3) + (offset1 & ~1u);
  address = (address << 2) + (offset2 & ~511u);
  return (address << 3) + (offset2 & 63);
}

// Untiles `width` x `height` blocks of `bytes_per_block` bytes from a tiled
// surface whose row pitch is `input_pitch_blocks` blocks (multiple of 32) into a
// linear buffer with `output_pitch_bytes` per row.
inline void Untile2D(uint8_t* out, const uint8_t* in, uint32_t width, uint32_t height,
                     uint32_t bytes_per_block, uint32_t input_pitch_blocks,
                     uint32_t output_pitch_bytes, uint32_t offset_x = 0, uint32_t offset_y = 0) {
  // log2 of bytes per block, as conversion.cpp computes it (valid for 1..16).
  const uint32_t log2_bpp =
      (bytes_per_block / 4) + ((bytes_per_block / 2) >> (bytes_per_block / 4));
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t row = TiledOffset2DRow(offset_y + y, input_pitch_blocks, log2_bpp);
    uint8_t* dst = out + size_t(y) * output_pitch_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t off = TiledOffset2DColumn(offset_x + x, offset_y + y, log2_bpp, row) >> log2_bpp;
      __builtin_memcpy(dst + size_t(x) * bytes_per_block, in + size_t(off) * bytes_per_block,
                       bytes_per_block);
    }
  }
}

}  // namespace xn::gfx::untile
