/**
 ******************************************************************************
 * xenon-native - VulkanRenderer: render targets, shaders, pipelines, textures,
 * Draw / Clear / Resolve / Present
 ******************************************************************************
 */
#include "vulkan_renderer_impl.h"

#include <algorithm>
#include <chrono>
#include <cmath>

// Build the hot guest-memory hashes for the same x86-64-v3 baseline as the
// renderer. The system library may use SSE2 even on our AVX2 target, making
// unchanged vertex buffers expensive to validate on every draw. The inlined
// implementation produces the identical XXH3 hash; no checks are skipped.
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <cstdio>
#include <string>

#include <fmt/format.h>
#include <cstdlib>

#include <rex/graphics/util/draw.h>
#include <rex/graphics/flags.h>

#include "gfx/frame_rate.h"
#include "presentation_mode.h"
#include "index_expand.h"
#include "shader_constants.h"
#include "texture_untile.h"
#include "debug_shaders.h"
#include "vertex_fetch_range.h"
#include "vertex_index_range.h"

namespace xn {
__attribute__((weak)) double MeasuredFrameRate() { return 0.0; }
__attribute__((weak)) bool FpsOverlayEnabled() { return false; }
}  // namespace xn

namespace {
static const uint8_t* GetFpsGlyph(char c) {
  static const uint8_t kSpace[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  static const uint8_t kDot[8]   = {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00};
  static const uint8_t k0[8]     = {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00};
  static const uint8_t k1[8]     = {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00};
  static const uint8_t k2[8]     = {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00};
  static const uint8_t k3[8]     = {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00};
  static const uint8_t k4[8]     = {0x0C, 0x1C, 0x34, 0x64, 0x7E, 0x04, 0x0E, 0x00};
  static const uint8_t k5[8]     = {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00};
  static const uint8_t k6[8]     = {0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00};
  static const uint8_t k7[8]     = {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00};
  static const uint8_t k8[8]     = {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00};
  static const uint8_t k9[8]     = {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00};
  static const uint8_t kF[8]     = {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00};
  static const uint8_t kP[8]     = {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00};
  static const uint8_t kS[8]     = {0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00};
  switch (c) {
    case '0': return k0;
    case '1': return k1;
    case '2': return k2;
    case '3': return k3;
    case '4': return k4;
    case '5': return k5;
    case '6': return k6;
    case '7': return k7;
    case '8': return k8;
    case '9': return k9;
    case '.': return kDot;
    case 'F': case 'f': return kF;
    case 'P': case 'p': return kP;
    case 'S': case 's': return kS;
    default: return kSpace;
  }
}
}  // namespace

namespace xn::gfx {

namespace draw_util = rex::graphics::draw_util;
using namespace rex::graphics;  // XE_GPU_REG_* enumerators
using rex::graphics::native::ConvertIndex16;
using rex::graphics::native::ConvertIndex32;
using rex::graphics::native::ExpandQuadList;
using rex::graphics::native::ExpandRectangleList;
using rex::graphics::native::GuestIndexSource;
using rex::graphics::native::QuadListExpandedCount;
using rex::graphics::native::QuadListNeeds32Bit;
using rex::graphics::native::RectangleListExpandedCount;

VulkanRenderer::Impl::LaunchDiagnostics::LaunchDiagnostics() {
  if (const char* precision = std::getenv("XN_VK_HIGH_PRECISION"))
    high_precision = precision[0] == 'f' ? 2 : 1;
  if (const char* mask = std::getenv("XN_DEBUG_DEPTH_MASK"))
    depth_mask = uint32_t(std::strtoul(mask,nullptr,0));
  depth_clear = std::getenv("XN_DEBUG_DEPTH_CLEAR") != nullptr;
  resolve_rect = std::getenv("XN_DEBUG_RESOLVE_RECT") != nullptr;
  level_curve = std::getenv("XN_DEBUG_LEVEL_CURVE") != nullptr;
}

namespace {

// Rate-limited skip logging: the same reason is reported a few times, not
// once per frame forever.
void LogSkip(const char* reason, uint64_t draw) {
  static std::unordered_map<std::string, int> counts;
  int& n = counts[reason];
  ++n;
  static const bool unlimited = std::getenv("XN_DEBUG_SKIPS") != nullptr;
  if (unlimited || n < 8) {
    REXLOG_WARN("xn-vk: draw #{} skipped: {}", draw, reason);
  }
}

struct TexFormatInfo {
  VkFormat vk = VK_FORMAT_UNDEFINED;
  uint32_t bytes_per_block = 0;
  uint32_t block_w = 1, block_h = 1;
  uint32_t host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  // Guest 16-bit packed formats we expand to RGBA8 on the CPU.
  bool expand_to_rgba8 = false;
};

void RecordResolvedRegion(TextureEntry& entry, VkRect2D region) {
  if (entry.fully_resolved || !region.extent.width || !region.extent.height) return;
  std::vector<VkRect2D> pending{region};
  // Retain only newly covered rectangles, so repeated partial updates don't
  // grow the list and overlapping writes aren't copied twice when sampling.
  for (const auto& covered : entry.resolved_regions) {
    std::vector<VkRect2D> remaining;
    for (const auto& part : pending) {
      const int32_t x0 = part.offset.x, y0 = part.offset.y;
      const int32_t x1 = x0 + part.extent.width, y1 = y0 + part.extent.height;
      const int32_t ix0 = std::max(x0, covered.offset.x);
      const int32_t iy0 = std::max(y0, covered.offset.y);
      const int32_t ix1 = std::min(x1, int32_t(covered.offset.x + covered.extent.width));
      const int32_t iy1 = std::min(y1, int32_t(covered.offset.y + covered.extent.height));
      if (ix0 >= ix1 || iy0 >= iy1) { remaining.push_back(part); continue; }
      auto add = [&](int32_t x, int32_t y, int32_t right, int32_t bottom) {
        if (right > x && bottom > y)
          remaining.push_back({{x, y}, {uint32_t(right - x), uint32_t(bottom - y)}});
      };
      add(x0, y0, x1, iy0);
      add(x0, iy1, x1, y1);
      add(x0, iy0, ix0, iy1);
      add(ix1, iy0, x1, iy1);
    }
    pending = std::move(remaining);
    if (pending.empty()) return;
  }
  entry.resolved_regions.insert(entry.resolved_regions.end(), pending.begin(), pending.end());
  uint64_t area = 0;
  for (const auto& part : entry.resolved_regions)
    area += uint64_t(part.extent.width) * part.extent.height;
  if (area == uint64_t(entry.image.width) * entry.image.height) {
    entry.fully_resolved = true;
    entry.resolved_regions = {{{0, 0}, {entry.image.width, entry.image.height}}};
  }
}

bool GetTexFormatInfo(xenos::TextureFormat f, TexFormatInfo& o) {
  using TF = xenos::TextureFormat;
  switch (f) {
    case TF::k_8:
      o = {VK_FORMAT_R8_UNORM, 1, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case TF::k_8_8:
      o = {VK_FORMAT_R8G8_UNORM, 2, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case TF::k_8_8_8_8:
    case TF::k_8_8_8_8_AS_16_16_16_16:
      o = {VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case TF::k_2_10_10_10:
    case TF::k_2_10_10_10_AS_16_16_16_16:
      o = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case TF::k_6_5_5:
      // Guest R5G5B6 becomes host R5G6B5 by exchanging green and blue.
      o = {VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, 1, XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G)};
      return true;
    case TF::k_5_6_5:
      // Put guest red in the host's high bits. R5G6B5 supports render-target
      // resolves on devices where B5G6R5 is available only for sampling.
      o = {VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB};
      return true;
    case TF::k_1_5_5_5:
    case TF::k_4_4_4_4:
      o = {VK_FORMAT_R8G8B8A8_UNORM, 2, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, true};
      return true;
    case TF::k_DXT1:
    case TF::k_DXT1_AS_16_16_16_16:
      o = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case TF::k_DXT2_3:
    case TF::k_DXT2_3_AS_16_16_16_16:
      o = {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case TF::k_DXT4_5:
    case TF::k_DXT4_5_AS_16_16_16_16:
      o = {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case TF::k_DXT5A:
      // DXT5's alpha block alone is BC4. Xenos replicates a single
      // component before applying the fetch swizzle, including into alpha.
      o = {VK_FORMAT_BC4_UNORM_BLOCK, 8, 4, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case TF::k_DXN:
      o = {VK_FORMAT_BC5_UNORM_BLOCK, 16, 4, 4, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case TF::k_16_16:
      o = {VK_FORMAT_R16G16_UNORM, 4, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case TF::k_16_16_16_16:
      o = {VK_FORMAT_R16G16B16A16_UNORM, 8, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case TF::k_16_16_16_16_FLOAT:
    case TF::k_16_16_16_16_EXPAND:
      o = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case TF::k_16_16_FLOAT:
    case TF::k_16_16_EXPAND:
      o = {VK_FORMAT_R16G16_SFLOAT, 4, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case TF::k_16_FLOAT:
    case TF::k_16_EXPAND:
      o = {VK_FORMAT_R16_SFLOAT, 2, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case TF::k_32_FLOAT:
      o = {VK_FORMAT_R32_SFLOAT, 4, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    case TF::k_32_32_FLOAT:
      o = {VK_FORMAT_R32G32_SFLOAT, 8, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
      return true;
    case TF::k_32_32_32_32_FLOAT:
      o = {VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
      return true;
    case TF::k_24_8:
    case TF::k_24_8_FLOAT:
      o = {VK_FORMAT_R32_SFLOAT, 4, 1, 1, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};
      return true;
    default:
      return false;
  }
}

// The unit the guest endian swap applies to for a format (Xenos swaps within
// 16- or 32-bit words as the fetch constant says; block formats are 16-bit
// word streams).
uint32_t GuestToHostSwizzle(uint32_t guest_swizzle, uint32_t host_format_swizzle) {
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t g = (guest_swizzle >> (3 * i)) & 0b111;
    uint32_t h = g >= xenos::XE_GPU_TEXTURE_SWIZZLE_0 ? (g & 0b101)
                                                      : ((host_format_swizzle >> (3 * g)) & 0b111);
    host_swizzle |= h << (3 * i);
  }
  return host_swizzle;
}

VkComponentSwizzle ComponentSwizzle(uint32_t host_swizzle, uint32_t component) {
  switch ((host_swizzle >> (3 * component)) & 0b111) {
    case xenos::XE_GPU_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_0: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
  }
}

// Post-swizzle TextureSign per component, 2 bits each (translator ABI:
// SystemConstants::texture_swizzled_signs). Constant components: unsigned.
uint8_t SwizzleSigns(const xenos::xe_gpu_texture_fetch_t& fetch) {
  uint8_t signs = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t sw = (fetch.swizzle >> (3 * i)) & 0b111;
    if (sw & 0b100) continue;
    signs |= uint8_t((fetch.dword_0 >> (2 + sw * 2)) & 0b11) << (i * 2);
  }
  return signs;
}

VkPrimitiveTopology TopologyFor(xenos::PrimitiveType prim, bool& supported) {
  supported = true;
  switch (prim) {
    case xenos::PrimitiveType::kPointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case xenos::PrimitiveType::kLineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case xenos::PrimitiveType::kLineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case xenos::PrimitiveType::kTriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case xenos::PrimitiveType::kTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case xenos::PrimitiveType::kTriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    // Expanded on the CPU to triangle lists.
    case xenos::PrimitiveType::kRectangleList:
    case xenos::PrimitiveType::kQuadList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    default:
      supported = false;
      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }
}

VkCompareOp CompareOp(xenos::CompareFunction f) { return VkCompareOp(uint32_t(f)); }
VkStencilOp StencilOp(xenos::StencilOp o) { return VkStencilOp(uint32_t(o)); }

VkBlendFactor BlendFactor(xenos::BlendFactor f) {
  static const VkBlendFactor kMap[32] = {
      VK_BLEND_FACTOR_ZERO,  VK_BLEND_FACTOR_ONE,  VK_BLEND_FACTOR_ZERO,  VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  return kMap[uint32_t(f) & 31];
}

VkBlendOp BlendOp(xenos::BlendOp o) {
  switch (o) {
    case xenos::BlendOp::kSubtract: return VK_BLEND_OP_SUBTRACT;
    case xenos::BlendOp::kMin: return VK_BLEND_OP_MIN;
    case xenos::BlendOp::kMax: return VK_BLEND_OP_MAX;
    case xenos::BlendOp::kRevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

VkSamplerAddressMode AddressMode(xenos::ClampMode m) {
  switch (m) {
    case xenos::ClampMode::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case xenos::ClampMode::kMirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case xenos::ClampMode::kClampToEdge:
    case xenos::ClampMode::kClampToHalfway: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
    case xenos::ClampMode::kMirrorClampToBorder:
      return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case xenos::ClampMode::kClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}

// Expand a 16-bit packed guest texel format to RGBA8 (after the endian swap).
void ExpandToRgba8(xenos::TextureFormat f, const uint16_t* in, uint8_t* out, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint16_t v = in[i];
    uint8_t r, g, b, a;
    if (f == xenos::TextureFormat::k_4_4_4_4) {
      r = uint8_t((v & 0xF) * 17);
      g = uint8_t(((v >> 4) & 0xF) * 17);
      b = uint8_t(((v >> 8) & 0xF) * 17);
      a = uint8_t(((v >> 12) & 0xF) * 17);
    } else {  // k_1_5_5_5
      r = uint8_t(((v & 0x1F) * 255 + 15) / 31);
      g = uint8_t((((v >> 5) & 0x1F) * 255 + 15) / 31);
      b = uint8_t((((v >> 10) & 0x1F) * 255 + 15) / 31);
      a = (v & 0x8000) ? 255 : 0;
    }
    out[i * 4 + 0] = r;
    out[i * 4 + 1] = g;
    out[i * 4 + 2] = b;
    out[i * 4 + 3] = a;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Render targets
// ---------------------------------------------------------------------------
// Bytes per texel of the HOST image. The guest's EDRAM texel is 4 bytes for
// every colour format this renderer models, so a host format that is not also
// 4 bytes cannot be byte-aliased against it - see EdramRange.
uint32_t VulkanRenderer::Impl::HostBytesPerTexel(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R32_SFLOAT:
      return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R32G32_SFLOAT:
      return 8;
    default:
      return 0;  // unknown: EdramRange will refuse to alias it
  }
}

VkFormat VulkanRenderer::Impl::ColorFormatToVk(xenos::ColorRenderTargetFormat f) {
  using CF = xenos::ColorRenderTargetFormat;
  switch (f) {
    case CF::k_8_8_8_8:
    case CF::k_8_8_8_8_GAMMA:
      // XN_VK_HIGH_PRECISION=1: keep the guest's 32-bit-per-texel EDRAM model
      // (the store/load path requires bytes_per_texel == 4) but give the
      // intermediates 10 bits per channel instead of 8. If the frame's
      // compressive transfer comes from rounding accumulated over many additive
      // passes in 8 bits, 4x the precision should visibly reduce it; if the
      // shape is unchanged, accumulation precision is excluded. The resolve is a
      // vkCmdBlitImage, which converts, so the presented image stays 8-bit.
      // XN_VK_HIGH_PRECISION=float: R16G16B16A16_SFLOAT, which has NO ceiling
      // at 1.0. The 10-bit test below raised precision but left the range
      // untouched - an A2B10G10R10 target is still UNORM and still clamps
      // above 1.0 - so its null result excluded precision and said nothing
      // about range. Rendering into a UNORM target clamps values the console
      // keeps, before a tone map reads them, and step 4 IS a tone map.
      // This costs EDRAM aliasing (8 B/texel cannot alias the guest's 4 B
      // layout), which would once have confounded the test - but aliasing has
      // been measured to make no difference to the finished frame at all
      // (identical to two decimals), so the confound is empirically gone.
      if (launch_diagnostics.high_precision == 2) {
        static bool announced_f = false;
        if (!announced_f) {
          announced_f = true;
          REXLOG_INFO("xn-vk: HIGHPRECISION=float active - 8888 colour targets are "
                      "R16G16B16A16_SFLOAT (no clamp at 1.0)");
        }
        return VK_FORMAT_R16G16B16A16_SFLOAT;
      }
      if (launch_diagnostics.high_precision == 1) {
        // Positive trace: a diagnostic that leaves none cannot be shown to have
        // run, and a silent no-op already invalidated a whole sweep once.
        static bool announced = false;
        if (!announced) {
          announced = true;
          REXLOG_INFO("xn-vk: HIGHPRECISION active - 8888 colour targets are A2B10G10R10 "
                      "(10 bits/channel, still 4 bytes/texel so the EDRAM model is unchanged)");
        }
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      }
      return VK_FORMAT_R8G8B8A8_UNORM;
    case CF::k_2_10_10_10:
    case CF::k_2_10_10_10_AS_10_10_10_10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case CF::k_2_10_10_10_FLOAT:
    case CF::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case CF::k_16_16_16_16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case CF::k_16_16: return VK_FORMAT_R16G16_SNORM;
    case CF::k_16_16_16_16: return VK_FORMAT_R16G16B16A16_SNORM;
    case CF::k_16_16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case CF::k_32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case CF::k_32_32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

// The height the guest is actually rendering: its scissor bounds the pass, and
// it is NOT the screen aspect applied to the pitch. Geometry Wars' smallest
// bloom level is 320x184 - 180 rounded up to a multiple of 8 for tile
// alignment - so deriving 180 from the aspect left four rows of every level
// unwritten and misplaced the haze everywhere the level was sampled.
VkSampleCountFlagBits VulkanRenderer::Impl::GuestSampleCount(xenos::MsaaSamples msaa) const {
  // XN_VK_NO_MSAA=1 forces single-sample, for attributing a measurement to
  // MSAA rather than to whatever else changed in the same build.
  static const bool force_1x = std::getenv("XN_VK_NO_MSAA") != nullptr;
  if (force_1x) return VK_SAMPLE_COUNT_1_BIT;
  // XN_VK_FORCE_MSAA_ALL=1: DIAGNOSTIC ONLY, never a fix. The console renders
  // the quarter-scale fill pass single-sampled (register read, both runtimes),
  // so forcing 2x there is not correct - it isolates our ONE-sample code path
  // from our TWO-sample one, which is the last structural difference between
  // the pass that matches the console and the pass that does not. If the fill
  // pass's output moves when it goes through the multisample path, the defect
  // is in the single-sample path; if it does not, that path is excluded.
  static const bool force_msaa = std::getenv("XN_VK_FORCE_MSAA_ALL") != nullptr;
  if (force_msaa) {
    static bool announced = false;
    if (!announced) {
      announced = true;
      REXLOG_INFO("xn-vk: FORCEMSAAALL active - every colour target takes the 2x path "
                  "(diagnostic: isolates the 1-sample code path, NOT a fix)");
    }
    return (supported_sample_counts & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT
                                                             : VK_SAMPLE_COUNT_1_BIT;
  }
  VkSampleCountFlagBits wanted = VK_SAMPLE_COUNT_1_BIT;
  if (msaa == xenos::MsaaSamples::k2X) wanted = VK_SAMPLE_COUNT_2_BIT;
  else if (msaa == xenos::MsaaSamples::k4X) wanted = VK_SAMPLE_COUNT_4_BIT;
  return (supported_sample_counts & wanted) ? wanted : VK_SAMPLE_COUNT_1_BIT;
}

// How many rows of the EDRAM view at this pitch a pass uses.
//
// The guest scissor is the source of truth - that is what fixed Geometry Wars'
// bloom levels from 320x180 to the correct 320x184 - but only when it IS a
// scissor. A scissor covering the whole 8192x8192 Xenos window is the guest
// saying "do not clip", and carries no information about the surface. Geometry
// Wars 2 issues exactly that on its viewport-transform-disabled passes, in the
// same frame and at the same EDRAM base as its real geometry; because target
// identity is (base, pitch, format) and a taller pass grows the image, one such
// draw stretched every target at that pitch to 8192 rows, so 1920x1080 became
// 1920x8192 and the picture collapsed into a strip. Treat it as absent and fall
// back to the aspect derivation used before the scissor was consulted.
// Render-target rows per EDRAM tile. Geometry Wars' resolve asks for 184 rows
// of a surface whose visible height is 180, i.e. the next multiple of 8.
constexpr uint32_t kEdramTileRows = 8;

uint32_t VulkanRenderer::Impl::GuestTargetHeight(uint32_t width) const {
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  const uint32_t from_scissor = scissor.offset[1] + scissor.extent[1];
  const bool full_window = scissor.extent[1] >= xenos::kTexture2DCubeMaxWidthHeight ||
                           from_scissor >= xenos::kTexture2DCubeMaxWidthHeight;
  if (from_scissor && !full_window) {
    // The scissor gives the VISIBLE extent; the surface is tile-aligned, and
    // those differ. Geometry Wars' bloom level is scissored to 320x180 while
    // its resolve reads 320x184 - so whichever arrived first decided the
    // image, the other grew it, and the target's shape depended on ordering.
    // Measured over one game: 2,072 requests for 180 from the scissor and
    // 1,499 for 184 from the resolve, on the same surface.
    //
    // Rounding the visible extent up to the tile height makes both sources
    // agree on 184 - the verified-correct value, worth a large amount of
    // measured fidelity when it was first found - so the image is created
    // once, at the right shape, in every run. Note this rounds UP only: it
    // never routes to the fallback formula, which gives 180 for this surface
    // and would look plausible while being wrong.
    return (from_scissor + kEdramTileRows - 1) / kEdramTileRows * kEdramTileRows;
  }
  return std::max<uint32_t>(1, (width * config.fallback_render_target_height + 640) / 1280);
}

// A render target that cannot fit in this renderer's virtual EDRAM cannot be
// what the guest asked for, whatever produced the number - so clamp it and say
// so, rather than allocating a surface the hardware could never have held.
// This is deliberately independent of the scissor rule above: it catches the
// next title's variant of this without anyone diagnosing it from frames.
uint32_t VulkanRenderer::Impl::ClampHeightToEdram(uint32_t width, uint32_t height,
                                                 uint32_t bytes_per_texel, uint32_t samples,
                                                 const char* what) const {
  if (!width || !height) return height;
  const uint32_t bpp = bytes_per_texel ? bytes_per_texel : 4;
  const uint64_t per_row = uint64_t(width) * bpp * std::max<uint32_t>(1, samples);
  if (!per_row) return height;
  const uint64_t max_rows = uint64_t(EdramSize()) / per_row;
  if (uint64_t(height) <= max_rows) return height;
  const uint32_t clamped = uint32_t(std::max<uint64_t>(1, max_rows));
  static uint32_t warnings = 0;
  if (warnings++ < 8) REXLOG_WARN(
      "xn-vk: {} target {}x{} ({} bpp, {}x MSAA) needs {} bytes of EDRAM but the virtual range has {}: "
      "clamping height to {}. Check the tile scissor and source resolve extent.",
      what, width, height, bpp, std::max<uint32_t>(1u, samples), uint64_t(height) * per_row,
      uint64_t(EdramSize()), clamped);
  return clamped;
}

RenderTargetImage* VulkanRenderer::Impl::GetColorTarget(uint32_t edram_base,
                                                        xenos::ColorRenderTargetFormat format,
                                                        uint32_t surface_pitch,
                                                        uint32_t height_needed) {
  // Blending precision modifiers do not change the underlying EDRAM view.
  // In particular, a MIN mask through format 12 must update the scene in format 3.
  format = xenos::GetStorageColorFormat(format);
  const uint32_t width = surface_pitch ? surface_pitch : 1280;
  // Which source supplied the height. A render target whose shape varies run
  // to run makes every comparison unreliable, so when chasing that, this says
  // WHICH input decided it rather than only what the answer was.
  const char* height_from = "caller";
  uint32_t height = height_needed;
  if (!height) {
    draw_util::Scissor sc;
    draw_util::GetScissor(regs, sc);
    const uint32_t from_scissor = sc.offset[1] + sc.extent[1];
    const bool full = sc.extent[1] >= xenos::kTexture2DCubeMaxWidthHeight ||
                      from_scissor >= xenos::kTexture2DCubeMaxWidthHeight;
    height_from = (from_scissor && !full) ? "scissor" : "fallback";
    height = GuestTargetHeight(width);
  }
  auto reg_it = registered_color.find(edram_base);
  if (reg_it != registered_color.end() && reg_it->second.width == width &&
      reg_it->second.height > height) {
    height = reg_it->second.height;
    height_from = "registered";
  }
  static const bool trace_rt_height = std::getenv("XN_VK_TRACE_RT_HEIGHT") != nullptr;
  if (trace_rt_height) {
    REXLOG_INFO("xn-vk: RTHEIGHT base={:#x} pitch={} -> {} from {} (caller asked {})", edram_base,
                width, height, height_from, height_needed);
  }
  const VkFormat vk_format = ColorFormatToVk(format);
  // After every source of height has had its say, and before the identity rule
  // can let a bad one grow the shared image.
  // EDRAM is modelled as 32-bit samples throughout this renderer (EdramRange
  // requires bytes_per_texel == 4), so the guard uses the same unit.
  height = ClampHeightToEdram(width, height, 4,
                              1u << uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples),
                              "colour");
  // Identity is the EDRAM view: base, pitch and format. Height is just how
  // much of that view a pass uses, so a taller pass grows the image rather
  // than creating a second one that would not alias the first.
  const VkSampleCountFlagBits samples =
      GuestSampleCount(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples);
  const uint32_t key =
      edram_base ^ (width << 12) ^ (uint32_t(format) << 28) ^ (uint32_t(samples) << 24);
  auto it = color_targets.find(key);
  if (it != color_targets.end()) {
    if (it->second.image.height >= height) return &it->second;
    // Too short for this pass: rebuild taller. Whatever it held is in EDRAM,
    // so the next EnsureRendering reloads it. WaitIdleAndReset() closes the
    // frame, which leaves `cb` stale and `rendering_active` false - reopen a
    // frame before returning, and drop every pointer into the old image.
    const bool was_debug = debug_frame_active;
    EndRendering();
    if (active_color == &it->second) active_color = nullptr;
    for (RenderTargetImage*& owner : edram_color_owners)
      if (owner == &it->second) owner = nullptr;
    if (last_presented == &it->second.image) last_presented = nullptr;
    frames[frame_index].garbage_images.push_back(it->second.image);
    if (it->second.resolved.image) frames[frame_index].garbage_images.push_back(it->second.resolved);
    color_targets.erase(it);
    BeginFrameIfNeeded();
    debug_frame_active = was_debug;
  }
  RenderTargetImage rt;
  rt.id = next_render_target_id++;
  rt.edram_base = edram_base;
  rt.xenos_format = uint32_t(format);
  rt.pitch = width;  // the surface pitch is what makes same-base targets alias
  rt.bytes_per_texel = HostBytesPerTexel(vk_format);
  rt.samples = samples;
  rt.guest_msaa = regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!CreateImage(width, height, vk_format, usage, VK_IMAGE_ASPECT_COLOR_BIT, rt.image,
                   samples)) {
    return nullptr;
  }
  if (samples != VK_SAMPLE_COUNT_1_BIT &&
      !CreateImage(width, height, vk_format, usage, VK_IMAGE_ASPECT_COLOR_BIT, rt.resolved)) {
    return nullptr;
  }
  REXLOG_INFO("xn-vk: colour target #{} edram={:#x} {}x{} fmt={} samples={} (key {:#x}, pitch {})",
              rt.id, edram_base, width, height, uint32_t(format), int(samples), key, surface_pitch);
  return &color_targets.emplace(key, rt).first->second;
}

RenderTargetImage* VulkanRenderer::Impl::GetDepthTarget(uint32_t edram_base, uint32_t width,
                                                        uint32_t height,
                                                        VkSampleCountFlagBits samples) {
  const uint32_t format = uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_format);
  const uint64_t key = uint64_t(edram_base) | (uint64_t(width) << 16) |
      (uint64_t(height) << 32) | (uint64_t(samples) << 48) | (uint64_t(format) << 56);
  auto it = depth_targets.find(key);
  if (it != depth_targets.end()) return &it->second;
  RenderTargetImage rt;
  rt.is_depth = true;
  rt.xenos_format = format;
  rt.edram_base = edram_base;
  rt.pitch = width;
  rt.guest_msaa = regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
  rt.samples = samples;
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  // D24FS8 is floating point. An integer D24 image changes depth comparisons
  // between coplanar UI layers and cannot represent its low-depth precision.
  const VkFormat host_format = format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8)
      ? float_depth_stencil_format : depth_stencil_format;
  if (!CreateImage(width, height, host_format, usage,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, rt.image, samples)) {
    return nullptr;
  }
  if (samples != VK_SAMPLE_COUNT_1_BIT &&
      !CreateImage(width, height, host_format, usage,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, rt.resolved)) {
    return nullptr;
  }
  // SDK clears may use a different pitch and sample count from the later
  // shadow map. Retain the packed value so new views inherit the same tiles.
  auto remaining = known_depth_clear_tiles;
  for (uint32_t tile = 0; tile < edram_tile_capacity; ++tile) {
    if (!remaining[tile]) continue;
    const uint32_t value = depth_clear_values[tile];
    std::bitset<kMaxVirtualEdramTileCapacity> equal;
    for (uint32_t i = tile; i < edram_tile_capacity; ++i) {
      if (remaining[i] && depth_clear_values[i] == value) {
        equal.set(i);
        remaining.reset(i);
      }
    }
    ClearDepthTiles(rt, equal, value);
  }
  return &depth_targets.emplace(key, rt).first->second;
}

// Tile walks produce ordered rectangles. Join adjacent tiles and then rows so
// a uniform SDK clear becomes one attachment clear rather than hundreds.
static void CoalesceClearRects(std::vector<VkClearRect>& rects) {
  for (bool vertical : {false, true}) {
    size_t count = 0;
    for (const VkClearRect next : rects) {
      if (count) {
        auto& previous = rects[count - 1].rect;
        const auto& current = next.rect;
        if (vertical ? (previous.offset.x == current.offset.x &&
                        previous.extent.width == current.extent.width &&
                        previous.offset.y + int32_t(previous.extent.height) == current.offset.y)
                     : (previous.offset.y == current.offset.y &&
                        previous.extent.height == current.extent.height &&
                        previous.offset.x + int32_t(previous.extent.width) == current.offset.x)) {
          if (vertical) previous.extent.height += current.extent.height;
          else previous.extent.width += current.extent.width;
          continue;
        }
      }
      rects[count++] = next;
    }
    rects.resize(count);
  }
}

void VulkanRenderer::Impl::ClearDepthTiles(RenderTargetImage& rt,
    const std::bitset<kMaxVirtualEdramTileCapacity>& tiles, uint32_t packed_clear) {
  if (tiles.none() || !rt.pitch) return;
  RenderTargetImage::DepthClearPlan* plan = nullptr;
  for (auto& candidate : rt.depth_clear_plans) {
    if (candidate.valid && candidate.tiles == tiles) { plan = &candidate; break; }
  }
  const bool reuse = plan != nullptr;
  if (!plan) plan = &rt.depth_clear_plans[rt.next_depth_clear_plan++ % rt.depth_clear_plans.size()];
  auto& rects = plan->rects;
  if (!reuse) {
    const uint32_t tx = rt.guest_msaa == xenos::MsaaSamples::k4X ? 40 : 80;
    const uint32_t ty = rt.guest_msaa == xenos::MsaaSamples::k1X ? 16 : 8;
    const uint32_t row_tiles = (rt.pitch + tx - 1) / tx;
    rects.clear();
    for (uint32_t y = 0; y < rt.image.height; y += ty) {
      for (uint32_t x = 0; x < rt.image.width; x += tx) {
        const uint64_t tile = uint64_t(rt.edram_base) + uint64_t(y / ty) * row_tiles + x / tx;
        if (tile >= edram_tile_capacity || !tiles[tile]) continue;
        rects.push_back({{{int32_t(x), int32_t(y)},
            {std::min(tx, rt.image.width - x), std::min(ty, rt.image.height - y)}}, 0, 1});
      }
    }
    CoalesceClearRects(rects);
    plan->tiles = tiles;
    plan->valid = true;
    ++stats.depth_clear_plans_built;
  } else {
    ++stats.depth_clear_plans_reused;
  }
  if (rects.empty()) return;
  if (debug_frame_active && launch_diagnostics.depth_clear && rt.pitch == 1280) {
    REXLOG_INFO("xn-vk: DEPTHCLEARED base={:#x} rt={}x{} samples={} guest_msaa={} format={} value={:#x} "
                "rects={} first=({},{} {}x{}) last=({},{} {}x{})",
                rt.edram_base, rt.image.width, rt.image.height, uint32_t(rt.samples),
                uint32_t(rt.guest_msaa), rt.xenos_format, packed_clear, rects.size(),
                rects.front().rect.offset.x, rects.front().rect.offset.y,
                rects.front().rect.extent.width, rects.front().rect.extent.height,
                rects.back().rect.offset.x, rects.back().rect.offset.y,
                rects.back().rect.extent.width, rects.back().rect.extent.height);
  }
  EndRendering();
  GpuWorkScope profile(WorkProfile(), cb, "depth_tile_clear", guest_swap_number, rt.id, rt.image.width, rt.image.height, uint32_t(rt.samples));
  Transition(rt.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = rt.image.view;
  att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  info.renderArea.extent = {rt.image.width, rt.image.height};
  info.layerCount = 1;
  info.pDepthAttachment = info.pStencilAttachment = &att;
  vkCmdBeginRendering(cb, &info);
  VkClearAttachment clear = {};
  clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  clear.clearValue.depthStencil = {
      std::clamp(rt.xenos_format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8)
          ? xenos::Float20e4To32(packed_clear >> 8)
          : xenos::UNorm24To32(packed_clear >> 8), 0.0f, 1.0f),
      packed_clear & 255u};
  vkCmdClearAttachments(cb, 1, &clear, uint32_t(rects.size()), rects.data());
  vkCmdEndRendering(cb);
}

void VulkanRenderer::Impl::ForgetDepthClearTiles(RenderTargetImage& rt) {
  // A regular draw discards all hints covered by this view. Further draws
  // cannot remove any more until an SDK clear introduces new hints. City
  // frames issue thousands of these draws; do the tile walk once per clear.
  if (rt.depth_clear_discard_generation == depth_clear_generation) return;
  rt.depth_clear_discard_generation = depth_clear_generation;
  const uint32_t words = !rt.is_depth && xenos::IsColorRenderTargetFormat64bpp(
      xenos::ColorRenderTargetFormat(rt.xenos_format)) ? 2 : 1;
  const uint32_t tx = 80 / (words * (rt.guest_msaa == xenos::MsaaSamples::k4X ? 2 : 1));
  const uint32_t ty = rt.guest_msaa == xenos::MsaaSamples::k1X ? 16 : 8;
  const uint32_t end = uint32_t(std::min<uint64_t>(edram_tile_capacity,
      uint64_t(rt.edram_base) +
      ((uint64_t(rt.pitch) + tx - 1) / tx) * ((uint64_t(rt.image.height) + ty - 1) / ty)));
  for (uint32_t tile = rt.edram_base; tile < end; ++tile) known_depth_clear_tiles.reset(tile);
}

bool VulkanRenderer::Impl::EdramRange(const RenderTargetImage& rt, VkDeviceSize& offset_out,
                                      VkDeviceSize& size_out) const {
  if (!rt.pitch || !rt.image.height) return false;
  // A resolved pixel cannot stand in for the individual samples in EDRAM.
  // Zero clears are handled across all sample counts by ApplyDepthClearAliases.
  if (rt.guest_msaa != xenos::MsaaSamples::k1X) return false;
  // The copy moves whole TEXELS, so it writes rt.bytes_per_texel per texel
  // whatever this function claims. Sizing the range with the guest's 4 bytes
  // while the host image is 8 (every 7e3 target: k_2_10_10_10_FLOAT lands on
  // R16G16B16A16_SFLOAT) made a 1920x1080 store reserve 8.3 MB and then write
  // 16.6 MB into a 10 MB buffer - a 2x overrun past the default EDRAM. The
  // first title cannot reach this: all of its targets are k_8_8_8_8, which is
  // 4 bytes on both sides.
  if (rt.bytes_per_texel != 4) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("xn-vk: EDRAM aliasing disabled for {} B/texel host targets (the guest's EDRAM "
                  "texel is 4 B, so the byte layouts cannot be aliased); surfaces of this format "
                  "will not see what another view wrote",
                  rt.bytes_per_texel);
    }
    return false;
  }
  if (!EdramTileRangeFits(rt.edram_base, rt.pitch, rt.image.height,
                          edram_tile_capacity)) return false;
  offset_out = VkDeviceSize(rt.edram_base) * kEdramTileBytes;
  size_out = VkDeviceSize((uint64_t(rt.pitch) + 79) / 80) *
      ((uint64_t(rt.image.height) + 15) / 16) * kEdramTileBytes;
  return true;
}

namespace {
// A shared tile has the same pixels regardless of the surface pitch. Copy
// one 80x16 tile per region, including partial tiles at the right/bottom edges.
std::vector<VkBufferImageCopy> EdramTileCopies(const RenderTargetImage& rt, VkDeviceSize base) {
  std::vector<VkBufferImageCopy> regions;
  const uint32_t pitch_tiles = (rt.pitch + 79) / 80;
  const uint32_t height = rt.image.height;
  for (uint32_t y = 0; y < height; y += 16) {
    for (uint32_t x = 0; x < rt.image.width; x += 80) {
      VkBufferImageCopy r = {};
      r.bufferOffset = base + (VkDeviceSize(y / 16) * pitch_tiles + x / 80) * 5120;
      r.bufferRowLength = 80;
      r.bufferImageHeight = 16;
      r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      r.imageOffset = {int32_t(x), int32_t(y), 0};
      r.imageExtent = {std::min(80u, rt.image.width - x), std::min(16u, height - y), 1};
      regions.push_back(r);
    }
  }
  return regions;
}
}

void VulkanRenderer::Impl::StoreTargetToEdram(RenderTargetImage& rt) {
  VkDeviceSize offset = 0, size = 0;
  GpuImage& src = ReadableImage(rt);
  if (!edram_aliasing || !edram.buffer || !src.image || !EdramRange(rt, offset, size)) {
    if (debug_frame_active) {
      REXLOG_INFO("xn-vk: EDRAM store rt#{} SKIPPED (aliasing={} buffer={} pitch={} h={} bpt={})",
                  rt.id, edram_aliasing, edram.buffer != VK_NULL_HANDLE, rt.pitch, rt.image.height,
                  rt.bytes_per_texel);
    }
    return;
  }
  if (debug_frame_active) {
    REXLOG_INFO("xn-vk: EDRAM store rt#{} {}x{} pitch={} -> [{:#x},{:#x})", rt.id, rt.image.width,
                rt.image.height, rt.pitch, offset, offset + size);
  }
  EndRendering();
  GpuWorkScope profile(WorkProfile(), cb, "edram_store", guest_swap_number, rt.id, rt.image.width, rt.image.height, uint32_t(rt.samples), size);
  Transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  auto regions = EdramTileCopies(rt, offset);
  vkCmdCopyImageToBuffer(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, edram.buffer,
                         uint32_t(regions.size()), regions.data());
  VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.memoryBarrierCount = 1;
  di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
}

void VulkanRenderer::Impl::LoadTargetFromEdram(RenderTargetImage& rt) {
  VkDeviceSize offset = 0, size = 0;
  // A buffer cannot be copied into a multisample image; a multisample target
  // therefore does not pick EDRAM contents back up. Geometry Wars only aliases
  // its single-sample bloom levels, so this costs nothing here - it would need
  // a full-screen draw to support properly.
  if (rt.samples != VK_SAMPLE_COUNT_1_BIT) return;
  if (!edram_aliasing || !edram.buffer || !EdramRange(rt, offset, size)) return;
  if (debug_frame_active) {
    REXLOG_INFO("xn-vk: EDRAM load rt#{} {}x{} pitch={} <- [{:#x},{:#x})", rt.id, rt.image.width,
                rt.image.height, rt.pitch, offset, offset + size);
  }
  EndRendering();
  GpuWorkScope profile(WorkProfile(), cb, "edram_load", guest_swap_number, rt.id, rt.image.width, rt.image.height, uint32_t(rt.samples), size);
  Transition(rt.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  auto regions = EdramTileCopies(rt, offset);
  vkCmdCopyBufferToImage(cb, edram.buffer, rt.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         uint32_t(regions.size()), regions.data());
}

void VulkanRenderer::Impl::EnsureRendering(RenderTargetImage* color, RenderTargetImage* depth) {
  EnsureRendering(&color, 1, depth);
}

// Stores every render target that currently owns EDRAM colour tiles. Call
// before anything that reads EDRAM behind the render targets' backs. Ownership
// is kept by default: the single-owner code this replaced stored without
// releasing, and the depth-clear alias path depends on that.
void VulkanRenderer::Impl::StoreAllEdramOwners(bool release) {
  for (RenderTargetImage*& owner : edram_color_owners) {
    if (!owner) continue;
    StoreTargetToEdram(*owner);
    if (release) owner = nullptr;
  }
}

void VulkanRenderer::Impl::EnsureRendering(RenderTargetImage* const* colors, uint32_t color_count,
                                           RenderTargetImage* depth) {
  color_count = std::min(color_count, uint32_t(xenos::kMaxColorRenderTargets));
  RenderTargetImage* color = nullptr;
  for (uint32_t i = 0; i < color_count && !color; ++i) color = colors[i];
  bool same = rendering_active && active_depth == depth && active_color_count == color_count;
  for (uint32_t i = 0; same && i < color_count; ++i) same = active_colors[i] == colors[i];
  if (same) return;
  EndRendering();
  // Only colour passes own the colour tile buffer. A depth-only SDK clear
  // uses a dummy colour view; storing it would overwrite real colour tiles.
  // XN_VK_NO_EDRAM_ALIAS remains available for diagnostic comparisons.
  static const bool no_alias = std::getenv("XN_VK_NO_EDRAM_ALIAS") != nullptr;
  if (no_alias) {
    static bool announced = false;
    if (!announced) {
      announced = true;
      REXLOG_INFO("xn-vk: NOEDRAMALIAS active - EDRAM ownership transfer disabled, "
                  "each view keeps its own storage");
    }
  } else if (regs.Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kDepthOnly) {
    // Ownership is per target: each attachment mirrors its own EDRAM range, so
    // hand back only the targets leaving the set and fetch only the ones
    // entering it.
    auto bound = [&](const RenderTargetImage* rt) {
      for (uint32_t i = 0; i < color_count; ++i)
        if (colors[i] == rt) return true;
      return false;
    };
    for (RenderTargetImage*& owner : edram_color_owners) {
      if (owner && !bound(owner)) {
        StoreTargetToEdram(*owner);
        owner = nullptr;
      }
    }
    for (uint32_t i = 0; i < color_count; ++i) {
      bool owned = false;
      for (RenderTargetImage* owner : edram_color_owners) owned = owned || owner == colors[i];
      if (owned || !colors[i]) continue;
      LoadTargetFromEdram(*colors[i]);
      for (RenderTargetImage*& slot : edram_color_owners) {
        if (!slot) { slot = colors[i]; break; }
      }
    }
  }
  VkRenderingAttachmentInfo color_atts[xenos::kMaxColorRenderTargets] = {};
  for (uint32_t i = 0; i < color_count; ++i) {
    RenderTargetImage* rt = colors[i];
    // A gap stays an unused attachment: a null view keeps slot numbering
    // aligned with the shader's oC indices.
    color_atts[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_atts[i].imageView = VK_NULL_HANDLE;
    if (!rt) continue;
    Transition(rt->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    color_atts[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_atts[i].imageView = rt->image.view;
    color_atts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_atts[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (rt->samples != VK_SAMPLE_COUNT_1_BIT && rt->resolved.image) {
      // Resolve at pass end, so the companion every reader uses is current the
      // moment the pass closes.
      Transition(rt->resolved, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      color_atts[i].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
      color_atts[i].resolveImageView = rt->resolved.view;
      color_atts[i].resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
  }
  if (depth) Transition(depth->image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo depth_att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (depth) {
    depth_att.imageView = depth->image.view;
    depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (depth->samples != VK_SAMPLE_COUNT_1_BIT && depth->resolved.image) {
      Transition(depth->resolved, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
      depth_att.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
      depth_att.resolveImageView = depth->resolved.view;
      depth_att.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
  }
  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {color->image.width, color->image.height}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = color_count;
  ri.pColorAttachments = color_atts;
  ri.pDepthAttachment = depth ? &depth_att : nullptr;
  ri.pStencilAttachment = depth ? &depth_att : nullptr;
  if (auto* profile = WorkProfile()) render_work_token = profile->Begin(cb, "draw_pass",
      guest_swap_number, color->id, color->image.width, color->image.height, uint32_t(color->samples));
  vkCmdBeginRendering(cb, &ri);
  ++render_pass_starts_;
  rendering_active = true;
  active_color = color;
  active_color_count = color_count;
  active_colors = {};
  for (uint32_t i = 0; i < color_count; ++i) active_colors[i] = colors[i];
  active_depth = depth;
}

// D3D9 zeroes complete EDRAM tiles using SDK rectangles, sometimes through a
// different sample layout from the following draws. Propagate depth/stencil
// values across those views, including images created after the clear. A
// depth-only clear also aliases colour tiles (GW/SG), including nonzero packed
// colour values (MCLA's grey minimap background). Restrict this to verified
// passthrough shaders, uniform values and complete physical tiles.
bool VulkanRenderer::Impl::ApplyDepthClearAliases(const DrawParams& p, const Shader& vs) {
  const uint64_t hash = vs.ucode_data_hash();
  if (hash != 0x0A6D1DD7767FDF27ull && hash != 0x6DD9DD041F740550ull &&
      hash != 0x1E6883FCCDE1F688ull) return false;
  auto rejected = [&](uint32_t line) {
    if (debug_frame_active && launch_diagnostics.depth_clear &&
        RenderTargetBase(regs.Get<reg::RB_DEPTH_INFO>().value) == 0x400)
      REXLOG_INFO("xn-vk: SDKCLEAR rejected line={} surface={:#x} depth={:#x}",
                  line, regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO]);
    return false;
  };
  const bool clip_disabled = regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  if (debug_frame_active && launch_diagnostics.depth_clear) {
    REXLOG_INFO("xn-vk: SDKCLEAR swap={} vs={:016X} prim={} indexed={} count={} "
                "index_bounds={:#x},{:#x},{:#x} vte={:#x} clip={:#x} win={:#x} "
                "dc={:#x} sr={:#x} cc={:#x} su={:#x} surface={:#x} depth={:#x}",
                guest_swap_number, hash, p.primitive_type, p.indexed, p.index_count,
                regs[XE_GPU_REG_VGT_MIN_VTX_INDX], regs[XE_GPU_REG_VGT_MAX_VTX_INDX],
                regs[XE_GPU_REG_VGT_INDX_OFFSET], regs[XE_GPU_REG_PA_CL_VTE_CNTL],
                regs[XE_GPU_REG_PA_CL_CLIP_CNTL], regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
                regs[XE_GPU_REG_RB_DEPTHCONTROL], regs[XE_GPU_REG_RB_STENCILREFMASK],
                regs[XE_GPU_REG_RB_COLORCONTROL], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
                regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO]);
  }
  const auto mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if ((mode != xenos::EdramMode::kDepthOnly && mode != xenos::EdramMode::kColorDepth) ||
      p.primitive_type != uint32_t(xenos::PrimitiveType::kRectangleList) ||
      p.indexed || p.index_count % 3 || regs[XE_GPU_REG_VGT_INDX_OFFSET] ||
      regs[XE_GPU_REG_VGT_MIN_VTX_INDX] ||
      regs[XE_GPU_REG_VGT_MAX_VTX_INDX] < p.index_count - 1 ||
      regs[XE_GPU_REG_PA_CL_VTE_CNTL] != 0x300 ||
      !clip_disabled ||
      regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET]) return rejected(__LINE__);
  const auto dc = regs.Get<reg::RB_DEPTHCONTROL>();
  const auto sr = regs.Get<reg::RB_STENCILREFMASK>();
  const auto cc = regs.Get<reg::RB_COLORCONTROL>();
  const auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (!dc.z_enable || !dc.z_write_enable || dc.zfunc != xenos::CompareFunction::kAlways ||
      !dc.stencil_enable || dc.backface_enable ||
      dc.stencilfunc != xenos::CompareFunction::kAlways ||
      dc.stencilzpass != xenos::StencilOp::kReplace ||
      sr.stencilwritemask != 255 ||
      (mode == xenos::EdramMode::kColorDepth &&
       (cc.alpha_test_enable || cc.alpha_to_mask_enable)) ||
      sc.cull_front || sc.cull_back) return rejected(__LINE__);
  if (vs.vertex_bindings().size() != 1 || vs.vertex_bindings()[0].stride_words != 7) return rejected(__LINE__);
  const auto fetch = regs.GetVertexFetch(vs.vertex_bindings()[0].fetch_constant);
  const uint64_t address = uint64_t(fetch.address) << 2;
  const uint64_t bytes = uint64_t(p.index_count) * 28;
  if (fetch.type != xenos::FetchConstantType::kVertex || !guest_base ||
      bytes > uint64_t(fetch.size) * 4 || address + bytes > guest_size) return rejected(__LINE__);
  const auto si = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t sx = si.msaa_samples == xenos::MsaaSamples::k4X ? 2 : 1;
  const uint32_t sy = si.msaa_samples == xenos::MsaaSamples::k1X ? 1 : 2;
  const uint32_t pitch_tiles = (si.surface_pitch * sx + 79) / 80;
  if (!pitch_tiles) return rejected(__LINE__);
  const uint32_t base = RenderTargetBase(regs.Get<reg::RB_DEPTH_INFO>().value);
  draw_util::Scissor scissors;
  draw_util::GetScissor(regs, scissors);
  if (debug_frame_active && launch_diagnostics.depth_clear)
    REXLOG_INFO("xn-vk: SDKCLEAR shape stride={} bytes={} fetch_bytes={} endian={} sc=({},{} {}x{}) physical_scale={},{}",
        vs.vertex_bindings()[0].stride_words, bytes, uint64_t(fetch.size) * 4,
        uint32_t(fetch.endian), scissors.offset[0], scissors.offset[1],
        scissors.extent[0], scissors.extent[1], sx, sy);
  // Work in physical tiles. A zero tile has identical contents in all formats,
  // including guest 7e3 stored in a wider, half-float Vulkan image.
  std::vector<std::pair<uint32_t, uint32_t>> spans;
  float clear_depth = -1.0f;
  for (uint32_t v = 0; v < p.index_count; v += 3) {
    float xy[3][2];
    for (uint32_t i = 0; i < 3; ++i) {
      PrepareGuestRead(uint32_t(address+(v+i)*28),12);
      for (uint32_t c = 0; c < 3; ++c) {
        const uint32_t bits = ConvertIndex32(guest_base + address + (v + i) * 28 + c * 4,
                                             fetch.endian);
        float f;
        std::memcpy(&f, &bits, 4);
        // Older D3D clear programs (Space Giraffe) use -0.5..N-0.5
        // coordinates with integer pixel centres. Align them to pixel edges.
        if (c < 2 && uint32_t(regs.Get<reg::PA_SU_VTX_CNTL>().pix_center) == 0 && f != std::floor(f)) f += 0.5f;
        // With clipping disabled the SDK's slightly negative clear depth clamps to zero.
        if (!std::isfinite(f) || (c < 2 && (f < 0 || f > 8192 || f != std::floor(f))))
          return rejected(__LINE__);
        if (c < 2) xy[i][c] = f;
        else {
          const float clamped = std::clamp(f, 0.0f, 1.0f);
          if (clear_depth >= 0.0f && clamped != clear_depth) return rejected(__LINE__);
          clear_depth = clamped;
        }
      }
    }
    uint32_t left = std::min({xy[0][0], xy[1][0], xy[2][0]});
    uint32_t right = std::max({xy[0][0], xy[1][0], xy[2][0]});
    uint32_t top = std::min({xy[0][1], xy[1][1], xy[2][1]});
    uint32_t bottom = std::max({xy[0][1], xy[1][1], xy[2][1]});
    if (debug_frame_active && launch_diagnostics.depth_clear)
      REXLOG_INFO("xn-vk: SDKCLEAR vertices ({},{}) ({},{}) ({},{}) depth={}",
          xy[0][0], xy[0][1], xy[1][0], xy[1][1], xy[2][0], xy[2][1], clear_depth);
    // Exactly three distinct corners of an axis-aligned rectangle.
    uint32_t corners = 0;
    for (const auto& pos : xy) {
      if ((pos[0] != left && pos[0] != right) || (pos[1] != top && pos[1] != bottom)) return rejected(__LINE__);
      corners |= 1u << ((pos[0] == right ? 1 : 0) | (pos[1] == bottom ? 2 : 0));
    }
    if (__builtin_popcount(corners) != 3) return rejected(__LINE__);
    left = std::max(left, scissors.offset[0]);
    top = std::max(top, scissors.offset[1]);
    right = std::min(right, scissors.offset[0] + scissors.extent[0]);
    bottom = std::min(bottom, scissors.offset[1] + scissors.extent[1]);
    if (left >= right || top >= bottom) continue;
    if ((left * sx) % 80 || (right * sx) % 80 || (top * sy) % 16 || (bottom * sy) % 16 ||
        right > si.surface_pitch) return rejected(__LINE__);
    for (uint32_t y = top * sy / 16; y < bottom * sy / 16; ++y) {
      const uint64_t first = uint64_t(base) + uint64_t(y) * pitch_tiles + left * sx / 80;
      const uint64_t last = uint64_t(base) + uint64_t(y) * pitch_tiles + right * sx / 80;
      // SDK clears may describe a larger rectangle than the remaining EDRAM
      // at this base. Its in-bounds tiles still change. Rejecting the whole
      // clear leaves smaller aliased depth views stale between Forza UI strips.
      const uint32_t end = edram_tile_capacity;
      if (first < end) spans.emplace_back(uint32_t(first), uint32_t(std::min<uint64_t>(last, end)));
    }
  }
  if (spans.empty()) return rejected(__LINE__);
  const bool float_depth = regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
      xenos::DepthRenderTargetFormat::kD24FS8;
  const uint32_t depth_bits = float_depth ? xenos::Float32To20e4(clear_depth, true) :
      uint32_t(std::round(double(clear_depth) * 16777215.0));
  const uint32_t packed_clear = (depth_bits << 8) | sr.stencilref;
  std::bitset<kMaxVirtualEdramTileCapacity> cleared;
  for (const auto& [first, last] : spans)
    for (uint32_t tile = first; tile < last; ++tile) {
      cleared.set(tile);
      depth_clear_values[tile] = packed_clear;
    }
  known_depth_clear_tiles |= cleared;
  ++depth_clear_generation;
  for (auto& [key, target] : depth_targets) ClearDepthTiles(target, cleared, packed_clear);
  // Colour+depth clears write a colour value as well, rather than interpreting
  // the zero depth bits as colour. Only the depth-only alias clears colour.
  if (mode != xenos::EdramMode::kDepthOnly) return true;
  EndRendering();
  StoreAllEdramOwners();
  if (edram_aliasing && edram.buffer) {
    for (const auto& [first, last] : spans)
      vkCmdFillBuffer(cb, edram.buffer, VkDeviceSize(first) * kEdramTileBytes,
                      VkDeviceSize(last - first) * kEdramTileBytes, packed_clear);
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cb, &di);
  }
  for (auto& [key, rt] : color_targets) {
    // Nonzero raw aliases currently support the byte-addressed colour views.
    // Zero has the same representation in every supported colour format.
    if (packed_clear && rt.xenos_format != uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8) &&
        rt.xenos_format != uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) continue;
    const uint32_t words = xenos::IsColorRenderTargetFormat64bpp(
        xenos::ColorRenderTargetFormat(rt.xenos_format)) ? 2 : 1;
    const uint32_t tx = 80 / (words * (rt.guest_msaa == xenos::MsaaSamples::k4X ? 2 : 1));
    const uint32_t ty = rt.guest_msaa == xenos::MsaaSamples::k1X ? 16 : 8;
    const uint32_t row_tiles = (rt.pitch + tx - 1) / tx;
    const uint32_t end = rt.edram_base + row_tiles * ((rt.image.height + ty - 1) / ty);
    std::vector<VkClearRect> rects;
    for (const auto& [first, last] : spans) {
      uint32_t tile = std::max(first, rt.edram_base);
      const uint32_t limit = std::min(last, end);
      while (tile < limit) {
        const uint32_t relative = tile - rt.edram_base;
        const uint32_t x = relative % row_tiles * tx;
        const uint32_t y = relative / row_tiles * ty;
        const uint32_t count = std::min(limit - tile, row_tiles - relative % row_tiles);
        rects.push_back({{{int32_t(x), int32_t(y)},
                          {std::min(count * tx, rt.image.width - x),
                           std::min(ty, rt.image.height - y)}}, 0, 1});
        tile += count;
      }
    }
    if (rects.empty()) continue;
    CoalesceClearRects(rects);
    Transition(rt.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = rt.image.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (rt.resolved.image) {
      Transition(rt.resolved, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      att.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
      att.resolveImageView = rt.resolved.view;
      att.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = {rt.image.width, rt.image.height};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);
    VkClearAttachment zero = {};
    zero.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    if (packed_clear) {
      for (uint32_t c = 0; c < 4; ++c)
        zero.clearValue.color.float32[c] = float((packed_clear >> (c * 8)) & 255) / 255.0f;
    }
    vkCmdClearAttachments(cb, 1, &zero, uint32_t(rects.size()), rects.data());
    vkCmdEndRendering(cb);
  }
  if (debug_frame_active) {
    REXLOG_INFO("xn-vk: depth clear {:#010x} aliases {} tile rows at base={}",
                packed_clear, spans.size(), base);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Shaders / pipelines
// ---------------------------------------------------------------------------
ShaderEntry* VulkanRenderer::Impl::GetShader(const ShaderRef& ref, xenos::ShaderType type) {
  uint64_t hash = ref.hash;
  std::vector<uint32_t> swapped;
  if (!hash) {
    if (ref.big_endian) {
      hash = XXH3_64bits(ref.ucode, size_t(ref.dword_count) * 4);
    } else {
      swapped.resize(ref.dword_count);
      for (uint32_t i = 0; i < ref.dword_count; ++i) swapped[i] = __builtin_bswap32(ref.ucode[i]);
      hash = XXH3_64bits(swapped.data(), swapped.size() * 4);
    }
  }
  const uint64_t key = hash ^ (type == xenos::ShaderType::kPixel ? 1ull : 0ull);
  auto it = shaders.find(key);
  if (it != shaders.end()) return &it->second;
  ShaderEntry entry;
  entry.shader = std::make_unique<SpirvShader>(type, hash, ref.ucode, ref.dword_count,
                                               ref.big_endian ? std::endian::big
                                                              : std::endian::native);
  rex::string::StringBuffer disasm;
  entry.shader->AnalyzeUcode(disasm);
  if (type == xenos::ShaderType::kVertex) {
    entry.bounded_vertex_fetches = AnalyzeDirectVertexFetches(entry.shader->ucode_data());
    const auto scaled = AnalyzeScaledVertexFetch(entry.shader->ucode_data());
    if (scaled.slot < 96) {
      entry.bounded_vertex_fetches.set(scaled.slot);
      entry.scaled_vertex_fetch_slot = scaled.slot;
      entry.scaled_vertex_index_constant = scaled.constant_component;
    }
    entry.palette_vertex_fetch = AnalyzePaletteVertexFetch(entry.shader->ucode_data());
    if (entry.palette_vertex_fetch.slot < 96)
      entry.bounded_vertex_fetches.set(entry.palette_vertex_fetch.slot);
    entry.affine_vertex_fetch = AnalyzeAffineVertexFetch(entry.shader->ucode_data());
    if (entry.affine_vertex_fetch.slot < 96)
      entry.bounded_vertex_fetches.set(entry.affine_vertex_fetch.slot);
    // Fetch layout is immutable shader metadata. Validate and calculate its
    // extent once, rather than revisiting every attribute for every draw.
    for (const auto& binding : entry.shader->vertex_bindings()) {
      const auto slot = binding.fetch_constant;
      if (slot >= entry.bounded_vertex_fetches.size() || !entry.bounded_vertex_fetches[slot]) continue;
      for (const auto& attribute : binding.attributes) {
        const auto& instruction = attribute.fetch_instr;
        if (instruction.attributes.stride != binding.stride_words ||
            instruction.operands[1].storage_index != slot || instruction.attributes.offset < 0) {
          entry.bounded_vertex_fetches.reset(slot);
          break;
        }
        const auto mask = xenos::GetVertexFormatNeededWords(
            instruction.attributes.data_format, instruction.result.GetUsedResultComponents());
        const uint32_t words = mask ? 32u - uint32_t(std::countl_zero(mask)) : 0;
        entry.bounded_vertex_extents[slot] = std::max(entry.bounded_vertex_extents[slot],
            uint32_t(instruction.attributes.offset) + words);
      }
    }
  }
  REXLOG_INFO("xn-vk: new {} shader {:016X} ({} dwords)",
              type == xenos::ShaderType::kVertex ? "vertex" : "pixel", hash, ref.dword_count);
  return &shaders.emplace(key, std::move(entry)).first->second;
}

static bool type_is_pixel(const ShaderEntry& e) {
  return e.shader && e.shader->type() == xenos::ShaderType::kPixel;
}

VkShaderModule VulkanRenderer::Impl::GetShaderModule(ShaderEntry& entry, uint64_t modification,
                                                     Shader::Translation* t) {
  auto it = entry.modules.find(modification);
  if (it != entry.modules.end()) return it->second;
  const std::vector<uint8_t>& spirv = t->translated_binary();
  // XN_DEBUG_DUMP_SPV=<dir>: the TRANSLATED shader plus the modification bits
  // that produced it, so it can be compared byte for byte against the
  // emulator's translation of the same ucode. Both runtimes use the same
  // translator, so identical inputs must give identical output and any
  // divergence is itself the finding.
  if (const char* sd = std::getenv("XN_DEBUG_DUMP_SPV")) {
    const std::string path =
        fmt::format("{}/spv_{:016X}_{:016X}.{}.bin", sd, entry.shader->ucode_data_hash(),
                    modification, type_is_pixel(entry) ? "frag" : "vert");
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(spirv.data(), 1, spirv.size(), f);
      std::fclose(f);
    }
    REXLOG_INFO("xn-vk: SPVDUMP {:016X} modification={:016X} bytes={}",
                entry.shader->ucode_data_hash(), modification, spirv.size());
  }
  VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = spirv.size();
  ci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS) {
    REXLOG_ERROR("xn-vk: vkCreateShaderModule failed for {:016X}", entry.shader->ucode_data_hash());
    return VK_NULL_HANDLE;
  }
  // Conservatively discard preparation results across new translations,
  // including a later selection of a previously used module.
  ++descriptor_prepare_epoch;
  ++stats.shaders_translated;
  if (type_is_pixel(entry) && std::getenv("XN_DEBUG_CENTROID")) {
    const auto mode = SpirvShaderTranslator::Modification(modification);
    if (mode.pixel.interpolators_centroid)
      REXLOG_INFO("xn-vk: pixel shader {:016X} centroid interpolators={:#x}",
                  entry.shader->ucode_data_hash(), uint32_t(mode.pixel.interpolators_centroid));
  }
  entry.modules.emplace(modification, module);
  module_sources.emplace(module, std::make_pair(&entry, modification));
  return module;
}

VkDescriptorSetLayout VulkanRenderer::Impl::GetTextureLayout(uint32_t texture_count,
                                                             uint32_t sampler_count) {
  const uint32_t key = (texture_count << 16) | sampler_count;
  auto it = texture_layouts.find(key);
  if (it != texture_layouts.end()) return it->second;
  const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  for (uint32_t i = 0; i < texture_count; ++i) {
    bindings.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stages, nullptr});
  }
  for (uint32_t j = 0; j < sampler_count; ++j) {
    bindings.push_back({texture_count + j, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, nullptr});
  }
  VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  ci.bindingCount = uint32_t(bindings.size());
  ci.pBindings = bindings.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  vkCreateDescriptorSetLayout(device, &ci, nullptr, &layout);
  texture_layouts.emplace(key, layout);
  return layout;
}

VkPipelineLayout VulkanRenderer::Impl::GetPipelineLayout(uint32_t vs_tex, uint32_t vs_samp,
                                                         uint32_t ps_tex, uint32_t ps_samp) {
  const uint64_t key = (uint64_t(vs_tex) << 48) | (uint64_t(vs_samp) << 32) |
                       (uint64_t(ps_tex) << 16) | ps_samp;
  auto it = pipeline_layouts.find(key);
  if (it != pipeline_layouts.end()) return it->second;
  VkDescriptorSetLayout layouts[SpirvShaderTranslator::kDescriptorSetCount] = {};
  layouts[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] = layout_shared_memory;
  layouts[SpirvShaderTranslator::kDescriptorSetConstants] = layout_constants;
  layouts[SpirvShaderTranslator::kDescriptorSetTexturesVertex] = GetTextureLayout(vs_tex, vs_samp);
  layouts[SpirvShaderTranslator::kDescriptorSetTexturesPixel] = GetTextureLayout(ps_tex, ps_samp);
  VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  ci.setLayoutCount = SpirvShaderTranslator::kDescriptorSetCount;
  ci.pSetLayouts = layouts;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  vkCreatePipelineLayout(device, &ci, nullptr, &layout);
  pipeline_layouts.emplace(key, layout);
  return layout;
}

size_t PipelineKeyHash::operator()(const PipelineKey& key) const {
  // This lookup runs for every draw. Hash whole words instead of an 80-step
  // serial byte recurrence; full key equality still resolves hash collisions.
  return size_t(XXH3_64bits(&key, sizeof(key)));
}

VkPipeline VulkanRenderer::Impl::GetPipeline(const PipelineKey& key, VkPipelineLayout layout) {
  // The full, already-built key is cheap to compare and avoids hashing the
  // common repeated state. Only successful pipelines can take this path.
  if (cache_prepared_draw_state && last_pipeline_valid && last_pipeline &&
      last_pipeline_key == key) {
    ++stats.prepared_pipeline_lookups_avoided;
    return last_pipeline;
  }
  auto it = pipelines.find(key);
  if (it != pipelines.end()) {
    if (cache_prepared_draw_state && it->second) {
      last_pipeline_key = key;
      last_pipeline = it->second;
      last_pipeline_valid = true;
    }
    return it->second;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  uint32_t stage_count = 0;
  stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[stage_count].module = key.vs;
  stages[stage_count].pName = "main";
  ++stage_count;
  if (key.ps) {
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stage_count].module = key.ps;
    stages[stage_count].pName = "main";
    ++stage_count;
  }
  // No vertex input: the translated vertex shader fetches from the mirror.
  VkPipelineVertexInputStateCreateInfo vi = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VkPrimitiveTopology(key.topology);
  ia.primitiveRestartEnable = key.primitive_restart;
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = key.cull_mode;
  rs.frontFace = VkFrontFace(key.front_face);
  rs.depthClampEnable = key.depth_clamp ? VK_TRUE : VK_FALSE;
  rs.depthBiasEnable = key.depth_format != VK_FORMAT_UNDEFINED;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VkSampleCountFlagBits(key.samples ? key.samples : 1);

  reg::RB_DEPTHCONTROL dc;
  dc.value = key.depth_control;
  reg::RB_STENCILREFMASK srm;
  srm.value = key.stencil_ref_mask;
  VkPipelineDepthStencilStateCreateInfo ds = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = dc.z_enable;
  ds.depthWriteEnable = dc.z_enable && dc.z_write_enable;
  ds.depthCompareOp = CompareOp(dc.zfunc);
  ds.stencilTestEnable = dc.stencil_enable;
  ds.front.failOp = StencilOp(dc.stencilfail);
  ds.front.passOp = StencilOp(dc.stencilzpass);
  ds.front.depthFailOp = StencilOp(dc.stencilzfail);
  ds.front.compareOp = CompareOp(dc.stencilfunc);
  ds.front.compareMask = srm.stencilmask;
  ds.front.writeMask = srm.stencilwritemask;
  if (dc.backface_enable) {
    ds.back.failOp = StencilOp(dc.stencilfail_bf);
    ds.back.passOp = StencilOp(dc.stencilzpass_bf);
    ds.back.depthFailOp = StencilOp(dc.stencilzfail_bf);
    ds.back.compareOp = CompareOp(dc.stencilfunc_bf);
  } else {
    ds.back = ds.front;
  }
  ds.back.compareMask = srm.stencilmask;
  ds.back.writeMask = srm.stencilwritemask;

  static const bool force_no_blend = std::getenv("XN_VK_NO_BLEND") != nullptr;
  // One blend state per attachment. Each target carries its own RB_BLENDCONTROL
  // and its own nibble of the colour mask.
  VkPipelineColorBlendAttachmentState att[xenos::kMaxColorRenderTargets] = {};
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    reg::RB_BLENDCONTROL bc;
    bc.value = key.blend_controls[i];
    const bool blend_enabled = bc.value != 0x00010001u && bc.value != 0;
    att[i].blendEnable = blend_enabled && !force_no_blend;
    att[i].srcColorBlendFactor = BlendFactor(bc.color_srcblend);
    att[i].dstColorBlendFactor = BlendFactor(bc.color_destblend);
    att[i].colorBlendOp = BlendOp(bc.color_comb_fcn);
    att[i].srcAlphaBlendFactor = BlendFactor(bc.alpha_srcblend);
    att[i].dstAlphaBlendFactor = BlendFactor(bc.alpha_destblend);
    att[i].alphaBlendOp = BlendOp(bc.alpha_comb_fcn);
    att[i].colorWriteMask = (key.color_write_mask >> (i * 4)) & 0xF;
  }
  VkPipelineColorBlendStateCreateInfo cbs = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbs.attachmentCount = key.color_attachment_count;
  cbs.pAttachments = att;

  const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                    VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                    VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = uint32_t(std::size(dynamic));
  dyn.pDynamicStates = dynamic;

  VkPipelineRenderingCreateInfo rend = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rend.colorAttachmentCount = key.color_attachment_count;
  rend.pColorAttachmentFormats = key.color_formats;
  rend.depthAttachmentFormat = key.depth_format;
  rend.stencilAttachmentFormat = key.depth_format;

  VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.pNext = &rend;
  ci.stageCount = stage_count;
  ci.pStages = stages;
  ci.pVertexInputState = &vi;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &ms;
  ci.pDepthStencilState = &ds;
  ci.pColorBlendState = &cbs;
  ci.pDynamicState = &dyn;
  ci.layout = layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(device, vk_pipeline_cache, 1, &ci, nullptr, &pipeline);
  if (r != VK_SUCCESS) {
    REXLOG_ERROR("xn-vk: vkCreateGraphicsPipelines failed: {}", int(r));
    pipeline = VK_NULL_HANDLE;
  } else {
    ++stats.pipelines;
    RememberPipeline(key);
  }
  pipelines.emplace(key, pipeline);
  if (cache_prepared_draw_state && pipeline) {
    last_pipeline_key = key;
    last_pipeline = pipeline;
    last_pipeline_valid = true;
  }
  return pipeline;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
TextureEntry* VulkanRenderer::Impl::GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  const uint32_t base_address = fetch.base_address << 12;
  const uint32_t fetch_format = uint32_t(fetch.format);
  const bool volume = fetch.dimension == xenos::DataDimension::k3D;
  const uint32_t req_w = (volume ? fetch.size_3d.width : fetch.size_2d.width) + 1;
  const uint32_t req_h = (volume ? fetch.size_3d.height : fetch.size_2d.height) + 1;
  static const bool no_mips = std::getenv("XN_VK_NO_MIPS") != nullptr;
  const bool has_mips = !no_mips && fetch.mip_address &&
      (fetch.mip_min_level || fetch.mip_max_level);

  // A texture the game resolved into: the image already holds the data.
  auto addr_it = resolved_by_addr.find(base_address);
  if (!has_mips && fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked &&
      addr_it != resolved_by_addr.end()) {
    for (auto it = addr_it->second.rbegin(); it != addr_it->second.rend(); ++it) {
      TextureEntry* candidate = *it;
      // A depth copy overwrites the same guest bytes as a colour copy. MCLA
      // deliberately fetches D24S8 as 8_8_8_8 to obtain its stencil object ID.
      // Use the packed bytes from the latest write, not an older colour resolve
      // or the numerical depth expanded into a colour channel.
      if (candidate->packed_depth &&
          AreFormatsEquivalent(uint32_t(xenos::TextureFormat::k_8_8_8_8), fetch_format))
        candidate = candidate->packed_depth;
      if (!AreFormatsEquivalent(candidate->key.format, fetch_format) ||
          candidate->image.width < req_w || candidate->image.height < req_h ||
          candidate->image.width - req_w >= 32 || candidate->image.height - req_h >= 32)
        continue;
      if (!candidate->fully_resolved) break;
      candidate->last_frame_used = frame_number;
      if (candidate->image.width == req_w && candidate->image.height == req_h) return candidate;

      // Resolves are tile-aligned while fetch dimensions describe visible
      // texels (MCLA's minimap resolves 224x220, then fetches 220x220). A Vulkan
      // view cannot crop an image. Copy the visible region so normalized UVs,
      // filtering and clamp-to-edge use the guest's dimensions, not padding.
      TextureKey sample_key = candidate->key;
      sample_key.width = req_w;
      sample_key.height = req_h;
      auto& sampled = resolve_texture_views[sample_key];
      if (!sampled) {
        auto e = std::make_unique<TextureEntry>();
        e->key = sample_key;
        e->resolved = true;
        if (!CreateImage(req_w, req_h, candidate->image.format,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, e->image)) {
          resolve_texture_views.erase(sample_key);
          return nullptr;
        }
        sampled = std::move(e);
        ++stats.resolve_images_created;
      }
      if (sampled->resolve_serial != candidate->resolve_serial) {
        EndRendering();
        Transition(candidate->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(sampled->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy = {};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {req_w, req_h, 1};
        vkCmdCopyImage(cb, candidate->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            sampled->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Transition(candidate->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(sampled->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        sampled->resolve_serial = candidate->resolve_serial;
      }
      sampled->host_swizzle = candidate->host_swizzle;
      sampled->last_frame_used = frame_number;
      return sampled.get();
    }
  }

  TextureKey key;
  key.base_address = base_address;
  key.format = fetch_format;
  key.width = req_w;
  key.height = req_h;
  key.pitch = fetch.pitch << 5;
  key.tiled = fetch.tiled;
  key.endian = uint32_t(fetch.endianness);
  key.packed_mips = fetch.packed_mips;
  // Levels 1.. live at mip_address; with no mip_address there is no chain,
  // whatever mip_max_level claims. Volumes and stacked/cube surfaces keep the
  // base level only for now - their chains are laid out per slice and none of
  // the measured titles sample a minified one.
  {
    const uint32_t mip_address = fetch.mip_address << 12;
    const uint32_t requested = std::max(uint32_t(fetch.mip_min_level),
                                        uint32_t(fetch.mip_max_level));
    const bool simple_2d = fetch.dimension == xenos::DataDimension::k2DOrStacked &&
                           !fetch.stacked && !volume;
    if (!no_mips && mip_address && requested && simple_2d) {
      // A level smaller than one texel does not exist; cap by the base size.
      uint32_t last_level = 0;
      for (uint32_t axis = std::max(req_w, req_h); axis > 1; axis >>= 1) ++last_level;
      const uint32_t levels = std::min(requested, last_level) + 1;
      key.mip_levels = std::min(levels, uint32_t(xenos::kTextureMaxMips));
      key.mip_address = mip_address;
    }
  }
  key.dimension = uint32_t(fetch.dimension);
  key.depth = volume ? fetch.size_3d.depth + 1 :
      fetch.dimension == xenos::DataDimension::kCube ? 6 :
      fetch.stacked ? fetch.size_2d.stack_depth + 1 : 1;
  auto it = textures.find(key);
  if (it != textures.end()) {
    it->second->last_frame_used = frame_number;
    // As with a resolved 2D texture, GPU content owns fully resolved cubes.
    // Every face is replaced by its resolve, so hashing the underlying CPU
    // allocation cannot affect any texel. Partial cubes still validate RAM.
    if (!RefreshResolvedCubeFaces(*it->second) && !RefreshResolvedTextureLevels(*it->second)) {
      const uint64_t uploads = stats.textures_reuploaded;
      RefreshTextureIfChanged(*it->second);
      if (stats.textures_reuploaded != uploads) {
        RefreshResolvedCubeFaces(*it->second);
        RefreshResolvedTextureLevels(*it->second);
      }
    }
    return it->second.get();
  }
  auto entry = std::make_unique<TextureEntry>();
  entry->key = key;
  entry->last_frame_used = frame_number;
  if (!LoadTexture(*entry)) {
    entry->unsupported = true;
  }
  RefreshResolvedCubeFaces(*entry);
  RefreshResolvedTextureLevels(*entry);
  TextureEntry* raw = entry.get();
  textures.emplace(key, std::move(entry));
  return raw;
}

bool VulkanRenderer::Impl::RefreshResolvedCubeFaces(TextureEntry& entry) {
  const auto& k = entry.key;
  if (k.dimension != uint32_t(xenos::DataDimension::kCube) || entry.unsupported ||
      !entry.image.image) return false;
  TexFormatInfo fi;
  if (!GetTexFormatInfo(xenos::TextureFormat(k.format), fi)) return false;
  const uint64_t pitch = ((std::max(k.pitch, k.width) / fi.block_w) + 31u) & ~31u;
  const uint64_t rows = (((k.height + fi.block_h - 1) / fi.block_h) + 31u) & ~31u;
  const uint64_t face_stride = (pitch * rows * fi.bytes_per_block + 4095u) & ~uint64_t(4095);
  // GPU resolves don't write back into guest RAM. Games build dynamic cube
  // maps by resolving each face to a separate page-aligned guest address.
  // Overlay those live images on the uploaded cube, retaining guest data for
  // faces that haven't been rendered. UploadTexture invalidates these serials
  // if a CPU write refreshes the underlying allocation.
  uint32_t resolved_faces = 0;
  for (uint32_t face = 0; face < 6; ++face) {
    const uint64_t address = uint64_t(k.base_address) + face * face_stride;
    if (address >= kGuestPhysicalSize) break;
    auto found = resolved_by_addr.find(uint32_t(address));
    if (found == resolved_by_addr.end()) continue;
    for (auto it = found->second.rbegin(); it != found->second.rend(); ++it) {
      TextureEntry& source = **it;
      if (!AreFormatsEquivalent(source.key.format, k.format) ||
          source.image.format != entry.image.format || source.image.width < k.width ||
          source.image.height < k.height || source.image.width - k.width >= 32 ||
          source.image.height - k.height >= 32) continue;
      if (entry.cube_face_resolve_serials[face] == source.resolve_serial) {
        ++resolved_faces;
        break;
      }
      BeginFrameIfNeeded();
      EndRendering();
      if (source.host_swizzle == entry.host_swizzle) {
        Transition(source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy = {};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
        copy.extent = {k.width, k.height, 1};
        vkCmdCopyImage(cb, source.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      entry.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      } else {
        // Resolves retain copy_dest_swap in their sampling view, not their
        // pixels. Normalize this face into the cube's storage order; changing
        // the cube-wide swizzle would also change untouched CPU-loaded faces.
        // Invert the destination format mapping so the copy writes physical
        // host components, including formats with fewer than four channels.
        uint32_t storage_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
        for (uint32_t host = 0; host < 4; ++host) {
          for (uint32_t guest = 0; guest < 4; ++guest) {
            if (((entry.host_swizzle >> (guest * 3)) & 7) != host) continue;
            storage_swizzle = (storage_swizzle & ~(7u << (host * 3))) | (guest << (host * 3));
            break;
          }
        }
        const VkImageView source_view = GetTextureView(
            source, storage_swizzle, false, VK_IMAGE_VIEW_TYPE_2D);
        // GetTextureView keys use bits 0..27. Reserve the high bit for
        // identity attachment views, which share the texture's lifetime.
        const uint32_t face_view_key = 0x80000000u | face;
        auto view_it = entry.views.find(face_view_key);
        if (view_it == entry.views.end()) {
          VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
          ci.image = entry.image.image;
          ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
          ci.format = entry.image.format;
          ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, face, 1};
          VkImageView view = VK_NULL_HANDLE;
          if (vkCreateImageView(device, &ci, nullptr, &view) != VK_SUCCESS) break;
          view_it = entry.views.emplace(face_view_key, view).first;
        }
        Transition(source.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(entry.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        const bool copied = source_view && source_view != dummy_view && resolve_bias.Init(device) &&
            resolve_bias.Apply(cb, frames[frame_index].texture_pool, source_view,
                view_it->second, entry.image.format, 0, 0, 0, 0, k.width, k.height, 1.0f);
        if (!copied) {
          Transition(entry.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          LogSkip("cube face channel conversion unavailable", stats.draws);
          break;
        }
      }
      Transition(source.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      Transition(entry.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      entry.cube_face_resolve_serials[face] = source.resolve_serial;
      ++resolved_faces;
      source.last_frame_used = frame_number;
      if (debug_frame_active)
        REXLOG_INFO("xn-vk: cube {:#x} face {} updated from resolve {:#x}",
                    k.base_address, face, address);
      break;
    }
  }
  return resolved_faces == 6;
}

namespace {
// Where a mip level lives and how big it is, following the guest's layout
// (Xenia's TextureInfo::GetMipLocation / GetMipExtent, BSD licence).
//
// Level 0 is at base_address; 1.. are at mip_address. The guest lays each
// level out at POWER-OF-TWO dimensions, and once the shorter side reaches 16
// the remaining levels share one tile - the "packed tail" - at an offset
// within it rather than at an address of their own. Walking the chain
// therefore stops accumulating addresses at the tail and asks
// GetPackedMipOffset where inside it the level sits.
struct MipLocation {
  uint32_t address = 0;      // byte address of the level's tile data
  uint32_t off_x = 0, off_y = 0;  // offset in blocks within that data
  uint32_t guest_w = 0, guest_h = 0;  // power-of-two level size
};

uint32_t NextPow2(uint32_t v) {
  uint32_t r = 1;
  while (r < v) r <<= 1;
  return r;
}

// Blocks a level occupies in guest memory, tile-aligned exactly as
// TextureExtent::Calculate does for a guest surface.
uint64_t GuestLevelBytes(uint32_t w, uint32_t h, const TexFormatInfo& fi, bool tiled) {
  uint32_t bw = (w + fi.block_w - 1) / fi.block_w;
  uint32_t bh = (h + fi.block_h - 1) / fi.block_h;
  uint32_t pitch_blocks = (bw + 31) & ~31u;
  const uint32_t rows = (bh + 31) & ~31u;
  if (!tiled) {
    // Linear rows are a multiple of 256 bytes.
    const uint32_t byte_pitch = (pitch_blocks * fi.bytes_per_block + 255) & ~255u;
    pitch_blocks = byte_pitch / fi.bytes_per_block;
  }
  // Subresources start on 4 KB pages even when an R8/R8G8 tile occupies
  // less space. Omitting the padding shifts every following small mip.
  return (uint64_t(pitch_blocks) * rows * fi.bytes_per_block + 4095) & ~uint64_t(4095);
}

MipLocation LocateMip(const TextureKey& k, const TexFormatInfo& fi, uint32_t mip) {
  MipLocation loc;
  const uint32_t w_pow2 = NextPow2(std::max(k.width, 1u));
  const uint32_t h_pow2 = NextPow2(std::max(k.height, 1u));
  loc.guest_w = std::max(w_pow2 >> mip, 1u);
  loc.guest_h = std::max(h_pow2 >> mip, 1u);
  if (mip == 0) {
    loc.address = k.base_address;
    if (k.packed_mips)
      untile::GetPackedMipOffset(k.width, k.height, 0, fi.block_w, fi.block_h, loc.off_x, loc.off_y);
    return loc;
  }
  uint64_t offset = 0;
  for (uint32_t i = 1; i < mip; ++i) {
    const uint32_t mw = std::max(w_pow2 >> i, 1u);
    const uint32_t mh = std::max(h_pow2 >> i, 1u);
    if (k.packed_mips && std::min(mw, mh) <= 16) break;  // the tail shares one tile
    offset += GuestLevelBytes(mw, mh, fi, k.tiled != 0);
  }
  loc.address = uint32_t(k.mip_address + offset);
  if (k.packed_mips)
    untile::GetPackedMipOffset(k.width, k.height, mip, fi.block_w, fi.block_h, loc.off_x,
                               loc.off_y);
  return loc;
}
}  // namespace

bool VulkanRenderer::Impl::RefreshResolvedTextureLevels(TextureEntry& entry) {
  const auto& k = entry.key;
  if (k.dimension != uint32_t(xenos::DataDimension::k2DOrStacked) ||
      k.depth != 1 || entry.unsupported || !entry.image.image) return false;
  TexFormatInfo fi;
  if (!GetTexFormatInfo(xenos::TextureFormat(k.format), fi) || fi.block_w != 1) return false;
  if (entry.mip_resolve_serials.size() != entry.image.levels)
    entry.mip_resolve_serials.assign(entry.image.levels, 0);
  uint32_t resolved_levels = 0;
  for (uint32_t mip = 0; mip < entry.image.levels; ++mip) {
    const MipLocation loc = LocateMip(k, fi, mip);
    const uint32_t width = std::max(k.width >> mip, 1u);
    const uint32_t height = std::max(k.height >> mip, 1u);
    auto found = resolved_by_addr.find(loc.address);
    if (found == resolved_by_addr.end()) continue;
    for (auto it = found->second.rbegin(); it != found->second.rend(); ++it) {
      TextureEntry* source = *it;
      if (source->packed_depth &&
          AreFormatsEquivalent(uint32_t(xenos::TextureFormat::k_8_8_8_8), k.format))
        source = source->packed_depth;
      if (!AreFormatsEquivalent(source->key.format, k.format) ||
          source->image.format != entry.image.format ||
          source->image.width < loc.off_x + width || source->image.height < loc.off_y + height ||
          source->image.width - loc.off_x - width >= 32 ||
          source->image.height - loc.off_y - height >= 32) continue;
      std::vector<VkImageCopy> copies;
      uint64_t covered_area = 0;
      const bool changed = entry.mip_resolve_serials[mip] != source->resolve_serial;
      for (const auto& region : source->resolved_regions) {
        const uint32_t x0 = std::max(loc.off_x, uint32_t(region.offset.x));
        const uint32_t y0 = std::max(loc.off_y, uint32_t(region.offset.y));
        const uint32_t x1 = std::min(loc.off_x + width,
            uint32_t(region.offset.x) + region.extent.width);
        const uint32_t y1 = std::min(loc.off_y + height,
            uint32_t(region.offset.y) + region.extent.height);
        if (x0 >= x1 || y0 >= y1) continue;
        if (changed) {
          VkImageCopy copy = {};
          copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          copy.srcOffset = {int32_t(x0), int32_t(y0), 0};
          copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
          copy.dstOffset = {int32_t(x0 - loc.off_x), int32_t(y0 - loc.off_y), 0};
          copy.extent = {x1 - x0, y1 - y0, 1};
          copies.push_back(copy);
        }
        covered_area += uint64_t(x1 - x0) * (y1 - y0);
      }
      if (!covered_area) break;
      resolved_levels += covered_area == uint64_t(width) * height;
      source->last_frame_used = frame_number;
      if (!mip) entry.host_swizzle = source->host_swizzle;
      if (!changed) break;
      // The guest resolves each generated level to its own address. Assemble
      // the sampling chain on the GPU; returning just the resolved base image
      // silently clamped every requested LOD to zero. Queue ordering keeps
      // these copies current without a GPU wait or readback to guest RAM.
      BeginFrameIfNeeded();
      EndRendering();
      Transition(source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      Transition(entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      vkCmdCopyImage(cb, source->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    entry.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    uint32_t(copies.size()), copies.data());
      Transition(source->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      Transition(entry.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      entry.mip_resolve_serials[mip] = source->resolve_serial;
      if (debug_frame_active)
        REXLOG_INFO("xn-vk: texture {:#x} mip {} updated from resolve {:#x}",
                    k.base_address, mip, loc.address);
      break;
    }
  }
  return resolved_levels == entry.image.levels;
}

bool VulkanRenderer::Impl::BuildTextureUpload(TextureEntry& entry, std::vector<uint8_t>& out,
                                             uint64_t& hash_out, uint32_t mip) {
  const TextureKey& k = entry.key;
  TexFormatInfo fi;
  if (!GetTexFormatInfo(xenos::TextureFormat(k.format), fi)) return false;
  // The host level is the base size halved; the guest stores it at power-of-two
  // dimensions, which is what decides its pitch and where it sits.
  const uint32_t level_w = std::max(k.width >> mip, 1u);
  const uint32_t level_h = std::max(k.height >> mip, 1u);
  const MipLocation loc = LocateMip(k, fi, mip);
  const uint32_t width_blocks = (level_w + fi.block_w - 1) / fi.block_w;
  const uint32_t height_blocks = (level_h + fi.block_h - 1) / fi.block_h;
  const bool volume = k.dimension == uint32_t(xenos::DataDimension::k3D);
  const bool layered = k.depth > 1 || volume;
  uint32_t pitch_blocks = mip ? (loc.guest_w + fi.block_w - 1) / fi.block_w
                              : std::max(k.pitch, k.width) / fi.block_w;
  if (k.tiled || layered) pitch_blocks = (pitch_blocks + 31) & ~31u;
  if (mip && !k.tiled && !layered) {
    const uint32_t byte_pitch = (pitch_blocks * fi.bytes_per_block + 255) & ~255u;
    pitch_blocks = byte_pitch / fi.bytes_per_block;
  }
  const uint32_t rows = k.tiled || layered
                            ? ((mip ? ((loc.guest_h + fi.block_h - 1) / fi.block_h) : height_blocks)
                               + 31) & ~31u
                            : height_blocks;
  const uint64_t slice_bytes = uint64_t(pitch_blocks) * rows * fi.bytes_per_block;
  const uint64_t layer_stride = (slice_bytes + 4095) & ~uint64_t(4095);
  uint64_t texture_bytes = volume ? slice_bytes * ((k.depth + 3) & ~3u) :
      layered ? layer_stride * k.depth : slice_bytes;

  // Small levels live in the shared mip tail when the fetch enables packing.
  uint32_t off_x = loc.off_x, off_y = loc.off_y;
  // Measurement bracket (XN_VK_PACKED_OFFSET=0 disables it) - see docs/RENDERER.md.
  if (const char* po = std::getenv("XN_VK_PACKED_OFFSET")) {
    if (po[0] == '0') {
      off_x = 0;
      off_y = 0;
    } else if (po[0] == 't') {  // "tiled": apply it only to tiled surfaces
      if (!k.tiled) {
        off_x = 0;
        off_y = 0;
      }
    }
  }
  // XN_VK_OFFX_16W=<n>: override the x offset for 16-wide textures only, so the
  // glow strip can be bracketed without disturbing the 512x512 atlas.
  if (const char* ov = std::getenv("XN_VK_OFFX_16W")) {
    if (k.width == 16 && !mip) off_x = uint32_t(std::atoi(ov));
  }
  if (const char* ov = std::getenv("XN_VK_OFFY_16W")) {
    if (k.width == 16 && !mip) off_y = uint32_t(std::atoi(ov));
  }

  if (k.tiled && !volume) {
    const uint64_t extent = untile::TiledExtent2D(off_x + width_blocks, off_y + height_blocks,
        pitch_blocks, std::countr_zero(fi.bytes_per_block));
    texture_bytes = std::max(texture_bytes, (uint64_t(k.depth) - 1) * layer_stride + extent);
  }
  // Use the physical extent for both bounds checks and subsequent hashing /
  // page watches. width*height alone misses writes in the high tile banks.
  if (!guest_base || uint64_t(loc.address) + texture_bytes > uint64_t(guest_size)) {
    REXLOG_WARN("xn-vk: texture at {:#x} mip {} ({} bytes) outside guest memory ({} bytes)",
                loc.address, mip, texture_bytes, guest_size);
    return false;
  }
  const uint8_t* src = guest_base + loc.address;
  if (!mip && config.texture_source) {
    const auto revision = config.texture_source->Revision(k.base_address, k.width, k.height, k.format);
    if (revision) {
      if (!config.texture_source->Read(k.base_address, k.width, k.height, k.format, out) ||
          fi.expand_to_rgba8 || out.size() != uint64_t(width_blocks) * height_blocks * fi.bytes_per_block * k.depth)
        return false;
      entry.guest_size = uint32_t(texture_bytes);
      entry.source_revision = revision;
      hash_out = XXH3_64bits(out.data(), out.size());
      return true;
    }
    entry.source_revision = 0;
  }
  PrepareGuestRead(loc.address,uint32_t(texture_bytes));
  uint64_t prepared_texture_bytes = texture_bytes;

  // 1. Linearise (untile) into a tightly packed block buffer.
  const uint32_t linear_pitch = width_blocks * fi.bytes_per_block;
  const uint64_t linear_bytes = uint64_t(linear_pitch) * height_blocks * k.depth;
  if (linear_bytes > kGuestPhysicalSize) return false;
  std::vector<uint8_t> linear(size_t(linear_bytes), 0);
  if (layered) {
    const uint32_t log2_bpp = std::countr_zero(fi.bytes_per_block);
    for (uint32_t z = 0; z < k.depth; ++z) {
      for (uint32_t y = 0; y < height_blocks; ++y) {
        for (uint32_t x = 0; x < width_blocks; ++x) {
          uint64_t offset;
          if (k.tiled && volume) {
            offset = untile::TiledOffset3D(x + off_x, y + off_y, z,
                                           pitch_blocks, rows, log2_bpp);
          } else {
            offset = k.tiled ? untile::TiledOffset2DColumn(x + off_x, y + off_y, log2_bpp,
                untile::TiledOffset2DRow(y + off_y, pitch_blocks, log2_bpp)) :
                (uint64_t(y + off_y) * pitch_blocks + x + off_x) * fi.bytes_per_block;
            offset += uint64_t(z) * (volume ? slice_bytes : layer_stride);
          }
          if (uint64_t(k.base_address) + offset + fi.bytes_per_block >
              std::min<uint64_t>(guest_size, kGuestPhysicalSize)) return false;
          if (config.prepare_guest_read && offset+fi.bytes_per_block > prepared_texture_bytes) {
            const uint64_t end = offset+fi.bytes_per_block;
            PrepareGuestRead(uint32_t(uint64_t(loc.address)+prepared_texture_bytes),
                             uint32_t(end-prepared_texture_bytes));
            prepared_texture_bytes = end;
          }
          std::memcpy(linear.data() + (uint64_t(z) * height_blocks + y) * linear_pitch +
                          x * fi.bytes_per_block, src + offset, fi.bytes_per_block);
          texture_bytes = std::max(texture_bytes, offset + fi.bytes_per_block);
        }
      }
    }
  } else if (k.tiled) {
    untile::Untile2D(linear.data(), src, width_blocks, height_blocks, fi.bytes_per_block,
                     pitch_blocks, linear_pitch, off_x, off_y);
  } else {
    for (uint32_t y = 0; y < height_blocks; ++y) {
      const uint64_t offset = (uint64_t(off_y+y)*pitch_blocks+off_x)*fi.bytes_per_block;
      if (config.prepare_guest_read && offset+linear_pitch > prepared_texture_bytes) {
        const uint64_t end = offset+linear_pitch;
        PrepareGuestRead(uint32_t(uint64_t(loc.address)+prepared_texture_bytes),
                         uint32_t(end-prepared_texture_bytes));
        prepared_texture_bytes = end;
      }
      std::memcpy(linear.data() + size_t(y) * linear_pitch,
                  src+offset,linear_pitch);
    }
  }
  if (!mip) {
    entry.guest_size = uint32_t(texture_bytes);
  } else {
    if (entry.mip_memory.size() < mip) entry.mip_memory.resize(mip);
    entry.mip_memory[mip - 1].address = loc.address;
    entry.mip_memory[mip - 1].size = uint32_t(texture_bytes);
  }
  hash_out = XXH3_64bits(src, size_t(texture_bytes));
  // 2. Endian swap as the fetch constant says.
  SwapEndian(linear.data(), linear.size(), xenos::Endian(k.endian));
  if (xenos::TextureFormat(k.format) == xenos::TextureFormat::k_6_5_5 ||
      xenos::TextureFormat(k.format) == xenos::TextureFormat::k_5_6_5) {
    for (size_t i = 0; i < linear.size(); i += 2) {
      uint16_t v;
      std::memcpy(&v, linear.data() + i, 2);
      v = xenos::TextureFormat(k.format) == xenos::TextureFormat::k_6_5_5
          ? ((v & 31u) << 11) | ((v >> 5) & 0x7E0u) | ((v >> 5) & 31u)
          : ((v & 31u) << 11) | (v & 0x7E0u) | (v >> 11);
      std::memcpy(linear.data() + i, &v, 2);
    }
  }
  // 3. Optional expansion of 16-bit packed formats to RGBA8, or depth to float32.
  if (fi.expand_to_rgba8) {
    out.resize(size_t(level_w) * level_h * k.depth * 4);
    ExpandToRgba8(xenos::TextureFormat(k.format), reinterpret_cast<const uint16_t*>(linear.data()),
                  out.data(), size_t(level_w) * level_h * k.depth);
  } else if (xenos::TextureFormat(k.format) == xenos::TextureFormat::k_24_8) {
    out.resize(size_t(level_w) * level_h * k.depth * 4);
    const uint32_t* src32 = reinterpret_cast<const uint32_t*>(linear.data());
    float* dst32 = reinterpret_cast<float*>(out.data());
    const size_t count = size_t(level_w) * level_h * k.depth;
    for (size_t i = 0; i < count; ++i) {
      dst32[i] = xenos::UNorm24To32(src32[i] >> 8);
    }
  } else if (xenos::TextureFormat(k.format) == xenos::TextureFormat::k_24_8_FLOAT) {
    out.resize(size_t(level_w) * level_h * k.depth * 4);
    const uint32_t* src32 = reinterpret_cast<const uint32_t*>(linear.data());
    float* dst32 = reinterpret_cast<float*>(out.data());
    const size_t count = size_t(level_w) * level_h * k.depth;
    for (size_t i = 0; i < count; ++i) {
      dst32[i] = xenos::Float20e4To32(src32[i] >> 8);
    }
  } else {
    out = std::move(linear);
  }
  return true;
}

void VulkanRenderer::Impl::UploadTextureLevels(TextureEntry& entry,
                                              const std::vector<uint8_t>& blob,
                                              const std::vector<LevelCopy>& levels) {
  if (blob.empty() || levels.empty()) return;
  BeginFrameIfNeeded();
  static const bool dedicated = std::getenv("XN_VK_DEDICATED_TEXTURE_UPLOADS") != nullptr;
  VkDeviceSize offset = 0;
  const VkDeviceSize alignment =
      std::max<VkDeviceSize>(16, properties.limits.optimalBufferCopyOffsetAlignment);
  uint8_t* mapped = dedicated ? nullptr : RingAllocate(mirror_upload_ring, blob.size(), alignment, offset);
  VkBuffer buffer = mirror_upload_ring.buf.buffer;
  if (mapped) {
    ++texture_uploads_ring;
  } else {
    HostBuffer staging;
    if (!CreateHostBuffer(blob.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, false)) return;
    mapped = staging.mapped;
    buffer = staging.buffer;
    frames[frame_index].garbage.push_back(staging);
    ++texture_uploads_dedicated;
  }
  std::memcpy(mapped, blob.data(), blob.size());
  if (std::getenv("XN_VK_WHITE_TEX")) std::memset(mapped, 0xFF, blob.size());
  EndRendering();
  GpuWorkScope profile(WorkProfile(), cb, "texture_upload", guest_swap_number, 0, entry.image.width, entry.image.height, 1, blob.size());
  Transition(entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  std::vector<VkBufferImageCopy> regions;
  regions.reserve(levels.size());
  for (const LevelCopy& level : levels) {
    VkBufferImageCopy region = {};
    region.bufferOffset = offset + level.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level.mip, 0, entry.image.layers};
    region.imageExtent = {level.width, level.height, std::max(entry.image.depth >> level.mip, 1u)};
    regions.push_back(region);
  }
  vkCmdCopyBufferToImage(cb, buffer, entry.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         uint32_t(regions.size()), regions.data());
  entry.cube_face_resolve_serials.fill(0);
  std::fill(entry.mip_resolve_serials.begin(), entry.mip_resolve_serials.end(), 0);
  Transition(entry.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanRenderer::Impl::UploadTexture(TextureEntry& entry, const std::vector<uint8_t>& bytes,
                                        uint32_t mip) {
  if (bytes.empty()) return;
  BeginFrameIfNeeded();
  // Texture streaming used to allocate, bind and map a new Vulkan allocation
  // for every upload, including tiny 4x4 textures. Reuse the coherent staging
  // ring already retired by each frame's fence. Each upload owns its bytes
  // until that fence signals, just like the geometry snapshots.
  static const bool dedicated = std::getenv("XN_VK_DEDICATED_TEXTURE_UPLOADS") != nullptr;
  VkDeviceSize offset = 0;
  const VkDeviceSize alignment = std::max<VkDeviceSize>(16,properties.limits.optimalBufferCopyOffsetAlignment);
  uint8_t* mapped = dedicated ? nullptr : RingAllocate(mirror_upload_ring,bytes.size(),alignment,offset);
  VkBuffer buffer = mirror_upload_ring.buf.buffer;
  if (mapped) {
    ++texture_uploads_ring;
  } else {
    HostBuffer staging;
    if (!CreateHostBuffer(bytes.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,staging,false)) return;
    mapped = staging.mapped;
    buffer = staging.buffer;
    frames[frame_index].garbage.push_back(staging);
    ++texture_uploads_dedicated;
  }
  std::memcpy(mapped,bytes.data(),bytes.size());
  if (std::getenv("XN_VK_WHITE_TEX")) std::memset(mapped,0xFF,bytes.size());
  EndRendering();
  GpuWorkScope profile(WorkProfile(), cb, "texture_upload", guest_swap_number, 0, entry.image.width, entry.image.height, 1, bytes.size());
  Transition(entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy region = {};
  region.bufferOffset = offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, entry.image.layers};
  region.imageExtent = {std::max(entry.key.width >> mip, 1u),
                        std::max(entry.key.height >> mip, 1u),
                        std::max(entry.image.depth >> mip, 1u)};
  vkCmdCopyBufferToImage(cb, buffer, entry.image.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  if (!mip) entry.cube_face_resolve_serials.fill(0);
  if (mip < entry.mip_resolve_serials.size()) entry.mip_resolve_serials[mip] = 0;
  Transition(entry.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

bool VulkanRenderer::Impl::LoadTexture(TextureEntry& entry) {
  const TextureKey& k = entry.key;
  TexFormatInfo fi;
  if (!GetTexFormatInfo(xenos::TextureFormat(k.format), fi)) {
    static int logged = 0;
    if (logged++ < 8) {
      REXLOG_WARN("xn-vk: unsupported texture format {} at {:#x} ({}x{})", k.format,
                  k.base_address, k.width, k.height);
    }
    return false;
  }
  std::vector<uint8_t> bytes;
  uint64_t hash = 0;
  if (!BuildTextureUpload(entry, bytes, hash)) return false;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (std::getenv("XN_DEBUG_DUMP_BOUND_TEXTURES")) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (k.dimension == uint32_t(xenos::DataDimension::kCube)) {
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(physical_device, fi.vk, &properties);
    if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  if (!CreateImage(k.width, k.height, fi.vk,
                   usage,
                   VK_IMAGE_ASPECT_COLOR_BIT, entry.image, VK_SAMPLE_COUNT_1_BIT,
                   k.dimension == uint32_t(xenos::DataDimension::k3D) ? VK_IMAGE_VIEW_TYPE_3D :
                   k.dimension == uint32_t(xenos::DataDimension::kCube) ? VK_IMAGE_VIEW_TYPE_CUBE :
                   VK_IMAGE_VIEW_TYPE_2D_ARRAY, k.depth, k.mip_levels)) {
    return false;
  }
  entry.host_swizzle = fi.host_swizzle;
  entry.content_hash = hash;
  entry.last_hash_frame = frame_number;
  // Gather the guest's whole chain into ONE staging allocation. A level that
  // cannot be read (a chain running outside guest memory, say) stops the walk
  // rather than failing the texture: the levels already gathered are correct
  // and the sampler clamps to what exists.
  std::vector<LevelCopy> levels;
  levels.push_back({0, 0, std::max(k.width, 1u), std::max(k.height, 1u)});
  for (uint32_t mip = 1; mip < k.mip_levels; ++mip) {
    std::vector<uint8_t> level_bytes;
    uint64_t ignored = 0;
    if (!BuildTextureUpload(entry, level_bytes, ignored, mip) || level_bytes.empty()) {
      entry.image.levels = mip;
      break;
    }
    entry.mip_memory[mip - 1].content_hash = ignored;
    const uint64_t at = (bytes.size() + 15) & ~uint64_t(15);
    bytes.resize(size_t(at));
    bytes.insert(bytes.end(), level_bytes.begin(), level_bytes.end());
    levels.push_back({at, mip, std::max(k.width >> mip, 1u), std::max(k.height >> mip, 1u)});
  }
  UploadTextureLevels(entry, bytes, levels);
  ++stats.textures_loaded;
  // XN_DEBUG_DUMP_TEX=<dir>: write the linearised RGBA8 bytes of every uploaded
  // texture, so a texture can be compared byte-for-byte against the emulator's
  // own --dump_textures output.
  DumpTextureBytes(k, bytes);
  REXLOG_INFO("xn-vk: texture {:#x} {}x{} fmt={} tiled={} endian={} pitch={}", k.base_address,
              k.width, k.height, k.format, k.tiled, k.endian, k.pitch);
  return true;
}

void VulkanRenderer::Impl::DumpTextureBytes(const TextureKey& k,
                                            const std::vector<uint8_t>& bytes) {
  // XN_DEBUG_DUMP_TEX=<dir>: write the linearised bytes of a texture as it is
  // uploaded, so it can be compared against the emulator's own --dump_textures
  // output for the same guest address.
  const char* dir = std::getenv("XN_DEBUG_DUMP_TEX");
  if (!dir) return;
  static const char* address = std::getenv("XN_DEBUG_DUMP_TEX_ADDRESS");
  if (address && k.base_address != std::strtoul(address, nullptr, 16)) return;
  const std::string path =
      fmt::format("{}/t_{:08X}_{}x{}_f{}_{}_F{}.bin", dir, k.base_address, k.width, k.height,
                  k.format, k.tiled ? "tiled" : "linear", frame_number);
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
  }
  // Also the untouched guest bytes around the base address, so an alternative
  // reading of the same memory (different offset, pitch or orientation) can be
  // tried offline without another run.
  if (guest_base && uint64_t(k.base_address) + 65536 <= uint64_t(guest_size)) {
    const std::string raw =
        fmt::format("{}/raw_{:08X}_pitch{}_F{}.bin", dir, k.base_address, k.pitch, frame_number);
    if (FILE* f = std::fopen(raw.c_str(), "wb")) {
      std::fwrite(guest_base + k.base_address, 1, 65536, f);
      std::fclose(f);
    }
  }
}

void VulkanRenderer::Impl::RefreshTextureIfChanged(TextureEntry& entry) {
  if (entry.resolved || entry.unsupported || !entry.image.image) return;
  if (config.texture_source) {
    const auto& k = entry.key;
    const auto revision = config.texture_source->Revision(k.base_address, k.width, k.height, k.format);
    if (revision) {
      if (revision == entry.source_revision) return;
      std::vector<uint8_t> pixels;
      uint64_t hash = 0;
      if (!BuildTextureUpload(entry, pixels, hash)) return;
      entry.content_hash = hash;
      entry.last_hash_frame = frame_number;
      entry.write_tokens.clear();
      UploadTexture(entry, pixels);
      DumpTextureBytes(entry.key, pixels);
      ++stats.textures_reuploaded;
      return;
    }
    if (entry.source_revision) {
      entry.source_revision = 0;
      entry.last_hash_frame = UINT64_MAX;
      entry.write_tokens.clear();
    }
  }
  if (entry.last_hash_frame == frame_number) return;  // already checked this frame
  entry.last_hash_frame = frame_number;
  if (!guest_base || !entry.guest_size) return;
  static const bool no_refresh = std::getenv("XN_VK_NO_TEX_REFRESH") != nullptr;
  if (no_refresh) return;
  auto refresh = [&](uint32_t address, uint32_t size, uint64_t& previous_hash,
                     std::vector<uint64_t>& tokens, uint32_t mip) {
    if (!size || uint64_t(address) + size > guest_size) return;
    PrepareGuestRead(address,size);
    if (write_watch) {
      const uint32_t first = address / kMirrorPageSize;
      const uint32_t count = uint32_t((uint64_t(address % kMirrorPageSize) + size +
                                      kMirrorPageSize - 1) / kMirrorPageSize);
      bool unchanged = tokens.size() == count;
      if (!unchanged) tokens.assign(count, 0);
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t token = write_watch->Token(first + i);
        unchanged &= token && token == tokens[i];
        tokens[i] = token;
      }
      if (unchanged) return;
    }
    const uint64_t hash = XXH3_64bits(guest_base + address, size);
    if (hash == previous_hash) return;
    std::vector<uint8_t> bytes;
    uint64_t new_hash = 0;
    if (!BuildTextureUpload(entry, bytes, new_hash, mip)) { tokens.clear(); return; }
    previous_hash = new_hash;
    UploadTexture(entry, bytes, mip);
    if (mip) {
      static const bool trace = std::getenv("XN_DEBUG_MIP_REFRESH") != nullptr;
      static uint32_t logged = 0;
      if (trace && logged < 128) {
        ++logged;
        REXLOG_INFO("xn-vk: refreshed mip {} of texture {:#x} ({}x{}, format {}, source {:#x})",
                    mip, entry.key.base_address, entry.key.width, entry.key.height,
                    entry.key.format, address);
      }
    }
    if (!mip) DumpTextureBytes(entry.key, bytes);
    ++stats.textures_reuploaded;
  };
  refresh(entry.key.base_address, entry.guest_size, entry.content_hash, entry.write_tokens, 0);
  // Streamed mip allocations can change independently of the base. Packed
  // tails may share pages; every affected host level still needs uploading.
  for (uint32_t mip = 1; mip < entry.image.levels && mip <= entry.mip_memory.size(); ++mip) {
    auto& memory = entry.mip_memory[mip - 1];
    refresh(memory.address, memory.size, memory.content_hash, memory.write_tokens, mip);
  }
}

VkImageView VulkanRenderer::Impl::GetTextureView(TextureEntry& entry, uint32_t guest_swizzle,
                                                bool is_signed, VkImageViewType view_type) {
  if (entry.unsupported || !entry.image.image) return dummy_view;
  const uint32_t host_swizzle = GuestToHostSwizzle(guest_swizzle, entry.host_swizzle);
  const uint32_t view_key = host_swizzle | (uint32_t(is_signed) << 16) |
      ((view_type == VK_IMAGE_VIEW_TYPE_MAX_ENUM ? 15u : uint32_t(view_type)) << 24);
  auto it = entry.views.find(view_key);
  if (it != entry.views.end()) return it->second;
  VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  ci.image = entry.image.image;
  // The translator declares 2D textures as arrayed images, so the view must be
  // 2D_ARRAY; XN_VK_2D_VIEW=1 goes back to a plain 2D view for bisecting.
  ci.viewType = std::getenv("XN_VK_2D_VIEW") ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  if (entry.key.dimension == uint32_t(xenos::DataDimension::k3D)) ci.viewType = VK_IMAGE_VIEW_TYPE_3D;
  if (entry.key.dimension == uint32_t(xenos::DataDimension::kCube)) ci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  if (view_type != VK_IMAGE_VIEW_TYPE_MAX_ENUM) ci.viewType = view_type;
  ci.format = is_signed ? SignedSamplingFormat(entry.image.format) : entry.image.format;
  ci.components = {ComponentSwizzle(host_swizzle, 0), ComponentSwizzle(host_swizzle, 1),
                   ComponentSwizzle(host_swizzle, 2), ComponentSwizzle(host_swizzle, 3)};
  ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, entry.image.levels, 0, entry.image.layers};
  if (std::getenv("XN_DEBUG_VIEW_SWIZZLE")) {
    // ALPHA is the one sampled component never validated by any measurement so
    // far: the band draw blends ONE/ONE and ignores it, while the fill pass
    // blends src*srcAlpha + dst, so a wrong alpha scales that pass's light
    // without changing which texels it lights - exactly the signature measured
    // (coverage ratio 1.01, intensity ratio 2-3x).
    static const char* kNames[] = {"IDENTITY", "ZERO", "ONE", "R", "G", "B", "A"};
    auto nm = [&](VkComponentSwizzle c) { return kNames[uint32_t(c) <= 6 ? uint32_t(c) : 0]; };
    REXLOG_INFO("xn-vk: VIEWSWIZZLE tex={:#x} {}x{} guest_sw={:#x} host_sw={:#x} entry_sw={:#x} "
                "-> view rgba = {} {} {} {}",
                entry.key.base_address, entry.key.width, entry.key.height, guest_swizzle,
                host_swizzle, entry.host_swizzle, nm(ci.components.r), nm(ci.components.g),
                nm(ci.components.b), nm(ci.components.a));
  }
  VkImageView view = VK_NULL_HANDLE;
  if (vkCreateImageView(device, &ci, nullptr, &view) != VK_SUCCESS) return dummy_view;
  entry.views.emplace(view_key, view);
  return view;
}

VkSampler VulkanRenderer::Impl::GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch,
                                           const SpirvShader::SamplerBinding& binding) {
  xenos::TextureFilter mag = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                 ? fetch.mag_filter
                                 : binding.mag_filter;
  xenos::TextureFilter min = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                 ? fetch.min_filter
                                 : binding.min_filter;
  xenos::TextureFilter mip = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                 ? fetch.mip_filter
                                 : binding.mip_filter;
  xenos::AnisoFilter aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                 ? fetch.aniso_filter
                                 : binding.aniso_filter;
  static const bool no_mips = std::getenv("XN_VK_NO_MIPS") != nullptr;
  uint32_t longest_axis = fetch.dimension == xenos::DataDimension::k1D
      ? fetch.size_1d.width + 1
      : fetch.dimension == xenos::DataDimension::k3D
          ? std::max({fetch.size_3d.width, fetch.size_3d.height, fetch.size_3d.depth}) + 1
          : std::max(fetch.size_2d.width, fetch.size_2d.height) + 1;
  uint32_t last_level = 0;
  while (longest_axis >>= 1) ++last_level;
  uint32_t min_level = no_mips || !fetch.mip_address
      ? 0 : std::min(uint32_t(fetch.mip_min_level), last_level);
  if (!no_mips && !fetch.base_address && fetch.mip_address && fetch.mip_max_level)
    min_level = std::max(min_level, 1u);
  const uint64_t key = uint64_t(mag) | (uint64_t(min) << 2) | (uint64_t(mip) << 4) |
                       (uint64_t(aniso) << 6) | (uint64_t(fetch.clamp_x) << 9) |
                       (uint64_t(fetch.clamp_y) << 12) | (uint64_t(fetch.clamp_z) << 15) |
                       (uint64_t(fetch.border_color) << 18) | (uint64_t(min_level) << 20);
  auto it = samplers.find(key);
  if (it != samplers.end()) return it->second;
  VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  ci.magFilter = mag == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  ci.minFilter = min == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  // The guest's own mip chain is loaded (see BuildTextureLevel), so LOD is no
  // longer clamped to the base level. Without this every minified texture
  // sampled full-resolution texels, which is what made PGR4's road crawl.
  // XN_VK_NO_MIPS=1 goes back to base-level-only for comparison.
  ci.mipmapMode = mip == xenos::TextureFilter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                       : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  ci.minLod = float(min_level);
  ci.maxLod = no_mips ? 0.0f : mip == xenos::TextureFilter::kBaseMap
      ? ci.minLod + .25f : VK_LOD_CLAMP_NONE;
  ci.addressModeU = AddressMode(fetch.clamp_x);
  ci.addressModeV = AddressMode(fetch.clamp_y);
  ci.addressModeW = AddressMode(fetch.clamp_z);
  if (aniso != xenos::AnisoFilter::kDisabled && aniso != xenos::AnisoFilter::kUseFetchConst &&
      features.samplerAnisotropy) {
    ci.anisotropyEnable = VK_TRUE;
    ci.maxAnisotropy = float(1u << (uint32_t(aniso) - 1));
  }
  ci.borderColor = uint32_t(fetch.border_color) == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                     : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSampler sampler = VK_NULL_HANDLE;
  vkCreateSampler(device, &ci, nullptr, &sampler);
  samplers.emplace(key, sampler);
  return sampler;
}

VkDescriptorSet VulkanRenderer::Impl::AllocateTextureSet(SpirvShader* shader,
                                                         const RegisterFile& r,
                                                         VkShaderModule module) {
  last_texture_log.clear();
  const bool track_dirty = cache_texture_dirty && !config.texture_source &&
                           !debug_frame_active && module != VK_NULL_HANDLE;
  const uint32_t stage = shader->type() == xenos::ShaderType::kPixel ? 1 : 0;
  auto& prepared = texture_dirty_cache[stage];
  if (track_dirty && prepared.shader == shader && prepared.module == module &&
      prepared.epoch == descriptor_prepare_epoch && prepared.frame_number == frame_number &&
      prepared.frame_index == frame_index && prepared.resolves == stats.resolves &&
      !(pending_texture_dirty[stage] & prepared.used_fetch_mask)) {
    pending_texture_dirty[stage] = 0;
    ++stats.texture_dirty_hits;
    ++stats.texture_sets_reused;
    return prepared.set;
  }
  const auto& tex = shader->GetTextureBindingsAfterTranslation();
  const auto& samp = shader->GetSamplerBindingsAfterTranslation();
  if (tex.empty() && samp.empty()) return VK_NULL_HANDLE;
  uint32_t used_fetch_mask = 0;
  bool cacheable = track_dirty;
  if (track_dirty) {
    ++stats.texture_dirty_misses;
    const auto use_fetch = [&](uint32_t fetch) {
      if (fetch < 32) used_fetch_mask |= uint32_t(1) << fetch;
      else cacheable = false;
    };
    for (const auto& binding : tex) use_fetch(binding.fetch_constant);
    for (const auto& binding : samp) use_fetch(binding.fetch_constant);
  }
  const uint64_t prepared_epoch = descriptor_prepare_epoch;
  const uint64_t prepared_frame = frame_number, prepared_resolves = stats.resolves;
  const uint32_t prepared_frame_index = frame_index;
  const auto remember = [&](VkDescriptorSet set) {
    // Upload/resolve helpers can submit a batch or replace images while this
    // miss is prepared. Only admit a result from an unchanged resource epoch.
    if (cacheable && set && descriptor_prepare_epoch == prepared_epoch &&
        frame_number == prepared_frame && frame_index == prepared_frame_index &&
        stats.resolves == prepared_resolves) {
      prepared = {shader, module, used_fetch_mask, set, prepared_frame,
                  prepared_frame_index, prepared_resolves, prepared_epoch};
      pending_texture_dirty[stage] = 0;
    }
    return set;
  };
  VkDescriptorSetLayout layout = GetTextureLayout(uint32_t(tex.size()), uint32_t(samp.size()));
  // Most draws reuse a descriptor set. Keep lookup storage on the stack and
  // prepare Vulkan writes only on a cache miss; the old path allocated three
  // vectors and filled every write structure even for unchanged bindings.
  constexpr size_t kLocalBindings = 128;
  const size_t binding_count = tex.size() + samp.size();
  std::array<VkDescriptorImageInfo, kLocalBindings> local_infos;
  std::vector<VkDescriptorImageInfo> extra_infos;
  if (binding_count > local_infos.size()) extra_infos.resize(binding_count);
  std::span<VkDescriptorImageInfo> infos(
      extra_infos.empty() ? local_infos.data() : extra_infos.data(), binding_count);
  for (size_t i = 0; i < tex.size(); ++i) {
    xenos::xe_gpu_texture_fetch_t fetch = r.GetTextureFetch(tex[i].fetch_constant);
    const auto binding_dimension = tex[i].dimension;
    auto& cached_binding = texture_binding_cache[
        (tex[i].fetch_constant * 4 + uint32_t(binding_dimension)) * 2 + uint32_t(tex[i].is_signed)];
    static const bool force_dummy = std::getenv("XN_VK_DUMMY_TEX") != nullptr;
    if (cached_binding.frame != frame_number || cached_binding.resolves != stats.resolves ||
        std::memcmp(&cached_binding.fetch, &fetch, sizeof(fetch))) {
      VkImageView view = binding_dimension == xenos::FetchOpDimension::kCube ? dummy_cube.view :
          binding_dimension == xenos::FetchOpDimension::k3DOrStacked ? dummy_volume.view : dummy_view;
      const char* kind = "dummy";
      // Translation emits separate 2D-array and 3D bindings for a tfetch3D.
      // Bind the guest resource only to its actual dimension, and a compatible
      // dummy to the unused interpretation.
      if (!force_dummy && fetch.type == xenos::FetchConstantType::kTexture &&
          uint32_t(fetch.dimension) == uint32_t(binding_dimension) &&
          fetch.dimension != xenos::DataDimension::k1D) {
        TextureEntry* entry = GetTexture(fetch);
        if (entry) {
          if (!entry->unsupported) view = GetTextureView(*entry, fetch.swizzle, tex[i].is_signed);
          kind = entry->unsupported ? "UNSUPPORTED" : (entry->resolved ? "resolved" : "guest");
        }
      }
      cached_binding = {fetch, frame_number, stats.resolves, view, kind};
    }
    const VkImageView view = cached_binding.view;
    const char* kind = cached_binding.kind;
    if (debug_frame_active) {
      last_texture_log += fmt::format(
          " [tf{} dw={:#010x},{:#010x},{:#010x},{:#010x},{:#010x},{:#010x} signs={:#x} "
          "clamp={},{} border={} filt={},{} exp_adjust={} signed_binding={}]",
          tex[i].fetch_constant, fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3,
          fetch.dword_4, fetch.dword_5, SwizzleSigns(fetch), uint32_t(fetch.clamp_x),
          uint32_t(fetch.clamp_y), uint32_t(fetch.border_color), uint32_t(fetch.mag_filter),
          uint32_t(fetch.min_filter), int32_t(fetch.exp_adjust), uint32_t(tex[i].is_signed));
      last_texture_log +=
          fmt::format(" tf{}={:#x}({},{}x{},fmt={},sw={:#x},{})", tex[i].fetch_constant,
                      uint32_t(fetch.base_address) << 12, kind, uint32_t(fetch.size_2d.width) + 1,
                      uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.format),
                      uint32_t(fetch.swizzle), fetch.tiled ? "tiled" : "linear");
    }
    infos[i] = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
  for (size_t j = 0; j < samp.size(); ++j) {
    xenos::xe_gpu_texture_fetch_t fetch = r.GetTextureFetch(samp[j].fetch_constant);
    VkSampler sampler = GetSampler(fetch, samp[j]);
    infos[tex.size() + j] = {sampler ? sampler : dummy_sampler, VK_NULL_HANDLE,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
  // Descriptor contents are immutable across batches. Reusing an identical
  // binding is safe even if the image's pixels change on the GPU timeline.
  // Image destruction invalidates lookup keys; pool reset waits for its fence.
  std::array<uint64_t, kLocalBindings + 1> local_key;
  std::vector<uint64_t> extra_key;
  if (binding_count + 1 > local_key.size()) extra_key.resize(binding_count + 1);
  std::span<uint64_t> key(extra_key.empty() ? local_key.data() : extra_key.data(), binding_count + 1);
  key[0] = uint64_t(uintptr_t(layout));
  for (size_t i = 0; i < infos.size(); ++i)
    key[i + 1] = infos[i].imageView ? uint64_t(uintptr_t(infos[i].imageView)) :
                                    uint64_t(uintptr_t(infos[i].sampler));
  auto& frame = frames[frame_index];
  auto cached = frame.texture_sets.find(key);
  if (cached != frame.texture_sets.end()) {
    ++stats.texture_sets_reused;
    return remember(cached->second);
  }
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = frame.persistent_texture_pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
  std::array<VkWriteDescriptorSet, kLocalBindings> local_writes;
  std::vector<VkWriteDescriptorSet> extra_writes;
  if (binding_count > local_writes.size()) extra_writes.resize(binding_count);
  std::span<VkWriteDescriptorSet> writes(
      extra_writes.empty() ? local_writes.data() : extra_writes.data(), binding_count);
  for (size_t i = 0; i < writes.size(); ++i) {
    auto& write = writes[i];
    write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = uint32_t(i);
    write.descriptorCount = 1;
    write.descriptorType = i < tex.size() ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
                                           VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &infos[i];
  }
  vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
  ++frame.texture_set_count;
  ++stats.texture_sets_allocated;
  frame.image_descriptor_count += uint32_t(tex.size());
  frame.sampler_descriptor_count += uint32_t(samp.size());
  frame.texture_sets.emplace(std::vector<uint64_t>(key.begin(), key.end()), set);
  return remember(set);
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
void VulkanRenderer::Impl::FlushNativeDraws() {
  const uint32_t count = pending_native_draw_count;
  if (!count) return;
  // No mapped-buffer reads: upload memory may be write-combined PCIe memory.
  if (count == 1) {
    const auto& draw = pending_native_first;
    if (pending_native_indexed)
      vkCmdDrawIndexed(cb, draw.indexCount, 1, draw.firstIndex, 0, draw.firstInstance);
    else vkCmdDraw(cb, draw.indexCount, 1, 0, draw.firstInstance);
  } else {
    const VkDeviceSize command_offset = pending_native_draw_offset + offsetof(NativeDrawRecord, command);
    if (pending_native_indexed)
      vkCmdDrawIndexedIndirect(cb, draw_record_ring.buf.buffer, command_offset, count, sizeof(NativeDrawRecord));
    else vkCmdDrawIndirect(cb, draw_record_ring.buf.buffer, command_offset, count, sizeof(NativeDrawRecord));
    ++stats.multi_draw_calls;
    stats.multi_draw_draws += count;
    stats.multi_draw_calls_saved += count - 1;
  }
  pending_native_draw_count = 0;
}

void VulkanRenderer::Impl::DrawImpl(const DrawParams& p, ShaderEntry* prepared_vs,
                                    ShaderEntry* prepared_ps) {
  const uint64_t draw_number = stats.draws++;
  auto skip = [&](const char* reason) {
    ++stats.draws_skipped;
    LogSkip(reason, draw_number);
  };
  if (!device) return skip("not_initialised");
  // Visibility-query proxies stop after the hierarchical depth test. Their
  // pixel shader / colour mask are deliberately left live by D3D (Forza uses
  // magenta boxes around wheels), but must never reach the framebuffer.
  // Match the command processor's query-only suppression in the SDK backend.
  // TODO: execute vertex memexport for these draws when query support is added.
  const auto viz_query = regs.Get<reg::PA_SC_VIZ_QUERY>();
  if (viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)
    return skip("visibility_query_only");
  // XN_VK_DRAW_LIMIT=<n>: drop every non-resolve draw after the first n in each
  // frame, so the presented frame shows the state after exactly n draws.
  // Sweeping n gives a per-draw pixel sequence comparable with the emulator's
  // REX_DRAW_LIMIT, produced the same way on both sides.
  // Per-frame non-resolve draw index, maintained unconditionally so diagnostics
  // downstream of this point can name the step they are reporting on.
  if (regs.Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kCopy) {
    static uint64_t step_frame = ~uint64_t(0);
    if (step_frame != frame_number) {
      step_frame = frame_number;
      cur_limit_step = 0;
    } else {
      ++cur_limit_step;
    }
  }
  static const char* draw_limit = std::getenv("XN_VK_DRAW_LIMIT");
  if (const char* dl = draw_limit) {
    if (regs.Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kCopy) {
      static uint64_t limit_frame = ~uint64_t(0);
      static uint32_t limit_index = 0;
      if (limit_frame != frame_number) {
        limit_frame = frame_number;
        limit_index = 0;
      }
      const uint32_t step = limit_index++;
      // Log the GUEST primitive type and vertex count of every admitted draw.
      // The step number alone does not identify a draw across the two
      // runtimes - we expand quads to triangles and emit resolve helpers their
      // tracer consumes silently - so the sequences must be aligned on these
      // stable properties, never on the index.
      if (step < 64) {
        const auto si = regs.Get<reg::RB_SURFACE_INFO>();
        const auto ci = regs.Get<reg::RB_COLOR_INFO>();
        // Blend state for THIS step, read here rather than reused from any
        // other diagnostic: a constant blend factor is the one thing that can
        // apply a per-channel linear scale without touching any input data.
        // Sampler state of the first bound texture. The composite magnifies a
        // 320-wide level over the full screen and then applies a concave power
        // curve; for a concave f, f(mean) >= mean(f), so filtering before the
        // curve yields systematically MORE light than sampling before it.
        // Filter mode is therefore load-bearing here, not cosmetic.
        {
          std::string ts = " tex=none";
          for (uint32_t t = 0; t < 32; ++t) {
            xenos::xe_gpu_texture_fetch_t tf = regs.GetTextureFetch(t);
            if (tf.type != xenos::FetchConstantType::kTexture || !tf.base_address) continue;
            ts = fmt::format(" tf{}={:#x}/{}x{}/magf={}/minf={}/mipf={}/aniso={}/clamp={},{}"
                             "/border={}/lodbias={}",
                             t, tf.base_address << 12, tf.size_2d.width + 1,
                             tf.size_2d.height + 1, uint32_t(tf.mag_filter),
                             uint32_t(tf.min_filter), uint32_t(tf.mip_filter),
                             uint32_t(tf.aniso_filter), uint32_t(tf.clamp_x),
                             uint32_t(tf.clamp_y), uint32_t(tf.border_size),
                             uint32_t(tf.lod_bias));
            break;
          }
          REXLOG_INFO("xn-vk: LIMITTEX F{} step={}{}", frame_number, step, ts);
        }
        // Viewport / scissor / coordinate scaling as the GUEST states it. This
        // title's vertex shaders emit screen-pixel coordinates rather than NDC,
        // so the relationship between those coordinates and the target matters,
        // and a quarter-scale pass has a different one from the main pass.
        {
          const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
          REXLOG_INFO(
              "xn-vk: LIMITVP F{} step={} VTE={:#x}(xs={} xo={} ys={} yo={} zs={} zo={}) "
              "vport=({:.4g},{:.4g} {:.4g},{:.4g} {:.4g},{:.4g}) "
              "win_scissor_tl={:#x} br={:#x} win_off={:#x}",
              frame_number, step, regs[XE_GPU_REG_PA_CL_VTE_CNTL],
              uint32_t(vte.vport_x_scale_ena), uint32_t(vte.vport_x_offset_ena),
              uint32_t(vte.vport_y_scale_ena), uint32_t(vte.vport_y_offset_ena),
              uint32_t(vte.vport_z_scale_ena), uint32_t(vte.vport_z_offset_ena),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET),
              regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
              regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET]);
        }
        const auto bcw = regs.Get<reg::RB_BLENDCONTROL>(XE_GPU_REG_RB_BLENDCONTROL0);
        REXLOG_INFO(
            "xn-vk: LIMITBLEND F{} step={} raw={:#010x} csrc={} cdst={} cop={} asrc={} adst={} "
            "aop={} const=({:.5g},{:.5g},{:.5g},{:.5g})",
            frame_number, step, bcw.value, uint32_t(bcw.color_srcblend),
            uint32_t(bcw.color_destblend), uint32_t(bcw.color_comb_fcn),
            uint32_t(bcw.alpha_srcblend), uint32_t(bcw.alpha_destblend),
            uint32_t(bcw.alpha_comb_fcn), regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
            regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN), regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
            regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA));
        REXLOG_INFO(
            "xn-vk: LIMITSTEP F{} step={} prim={} guest_count={} color_base={} pitch={} "
            "fmt={} msaa={}",
            frame_number, step, p.primitive_type, p.index_count, RenderTargetBase(ci.value),
            uint32_t(si.surface_pitch), uint32_t(ci.color_format), uint32_t(si.msaa_samples));
      }
      if (step >= uint32_t(std::atoi(dl))) return skip("draw_limit");
    }
  }
  if (!p.vertex_shader.ucode || !p.vertex_shader.dword_count) return skip("no_vertex_shader");
  if (!p.index_count) return skip("zero_count");
  BeginFrameIfNeeded();
  const auto prim_type = xenos::PrimitiveType(p.primitive_type);
  bool topology_supported = false;
  VkPrimitiveTopology topology = TopologyFor(prim_type, topology_supported);
  if (!topology_supported) return skip("primitive_type");
  const bool expand_rects = prim_type == xenos::PrimitiveType::kRectangleList;
  const bool expand_points = prim_type == xenos::PrimitiveType::kPointList;
  const bool primitive_restart = p.indexed && regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
      (topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
       topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
       topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
  const bool prepared_index_count_eligible =
      cache_prepared_indices && p.index_count >= prepared_index_min_count;
  const bool plain_dma_indices = write_watch && p.indexed &&
      !p.index_host_data && !expand_rects && !expand_points &&
      prim_type != xenos::PrimitiveType::kQuadList && !primitive_restart;
  const bool cache_index_draw = (reuse_dma_indices || prepared_index_count_eligible) &&
      plain_dma_indices;
  const bool resident_index_draw = cache_resident_indices &&
      resident_index_buffer.buffer && resident_index_buffer.mapped &&
      !gpu_draw_constants && !multi_draw && plain_dma_indices &&
      p.index_count >= resident_index_min_count;
  const bool reuse_index_draw = cache_index_draw && reuse_dma_indices;
  const bool prepared_index_draw = cache_index_draw && prepared_index_count_eligible;
  const DmaIndexCache::Key index_cache_key{
      p.index_base_guest,p.index_count,(p.index_format & 1) ? 4u : 2u,p.index_endian & 3};
  const PreparedIndexCache::Key prepared_cache_key{
      index_cache_key.address,index_cache_key.count,index_cache_key.width,
      index_cache_key.endian,guest_base,write_watch};
  const auto index_token = [&](uint32_t page) { return write_watch->Token(page); };
  if ((cache_index_draw || resident_index_draw) && (!guest_base || uint64_t(p.index_base_guest) +
      uint64_t(p.index_count)*index_cache_key.width > std::min<uint64_t>(guest_size,kGuestPhysicalSize)))
    return skip("index_range");
  if (cache_index_draw || resident_index_draw)
    PrepareGuestRead(p.index_base_guest,p.index_count*index_cache_key.width);
  ResidentIndexCache::Lease resident_indices;
  bool resident_index_hit = false;
  if (resident_index_draw) {
    const ResidentIndexCache::Key resident_key{
        index_cache_key.address,index_cache_key.count,index_cache_key.width,
        index_cache_key.endian,guest_base,write_watch};
    resident_indices = resident_index_cache.Lookup(resident_key,index_token);
    resident_index_hit = bool(resident_indices);
    if (!resident_indices) {
      std::vector<uint64_t> tokens;
      if (resident_index_cache.SnapshotTokens(resident_key,tokens,index_token)) {
        resident_indices = resident_index_cache.Allocate(resident_key,tokens);
        if (resident_indices) {
          // This allocation has never been submitted. It is pinned through
          // helper-triggered batch changes and written exactly once.
          uint8_t* dst = resident_index_buffer.mapped + resident_indices.offset();
          const uint8_t* input = guest_base + p.index_base_guest;
          const auto endian = xenos::Endian(p.index_endian & 3);
          const auto range = index_cache_key.width == 4 ?
              CopyVertexIndices<uint32_t,true>(reinterpret_cast<uint32_t*>(dst),input,
                                               p.index_count,endian) :
              CopyVertexIndices<uint16_t,true>(reinterpret_cast<uint16_t*>(dst),input,
                                               p.index_count,endian);
          // A concurrent write may invalidate admission. The converted bytes
          // are still a private snapshot for this draw; release fences them.
          resident_index_cache.Publish(resident_indices,range,index_token);
        }
      }
    }
  }
  const bool ring_cache_index_draw = cache_index_draw && !resident_indices;
  if (resident_index_draw && !resident_indices) ++stats.resident_index_fallbacks;
  const DmaIndexCache::Entry* reused_indices = nullptr;
  VkDeviceSize prepared_index_offset = 0;
  VertexIndexRange prepared_index_range;
  bool prepared_index_hit = false;
  bool prepared_cpu_hit = false;

  // A guest frame may contain more draws than one upload-ring segment can
  // hold (MCLA's city does). Submit the completed batch before allocating any
  // resources for this draw, then reuse only fence-retired ring space. Doing
  // this inside RingAllocate would invalidate this draw's earlier uploads and
  // transient descriptors; dropping the draw loses the final scene composite.
  auto fits = [&](const RingBuffer& ring, VkDeviceSize bytes, VkDeviceSize align) {
    const VkDeviceSize offset = (ring.head + align - 1) & ~(align - 1);
    const VkDeviceSize limit = std::min(ring.buf.size,
        ring.frame_start[frame_index] + ring.buf.size / kFramesInFlight);
    return offset <= limit && bytes <= limit - offset;
  };
  VkDeviceSize constant_bytes = 0;
  for (VkDeviceSize size : ubo_sizes)
    constant_bytes += (size + ubo_align - 1) & ~(ubo_align - 1);
  // Point expansion is the largest index conversion: six uint32 indices per
  // input vertex. This conservative bound also covers rectangles and quads.
  for (unsigned attempt=0;; ++attempt) {
    reused_indices = ring_cache_index_draw && reuse_index_draw ?
        dma_index_cache.Find(index_cache_key,index_token) : nullptr;
    const VkDeviceSize index_bytes = resident_indices ? 0 : ring_cache_index_draw ?
        (reused_indices ? 0 : VkDeviceSize(p.index_count)*index_cache_key.width) :
        VkDeviceSize(p.index_count)*6*sizeof(uint32_t);
    if ((!gpu_draw_constants || fits(draw_record_ring, sizeof(NativeDrawRecord), 64)) &&
        fits(constant_ring, constant_bytes, ubo_align) &&
        (resident_indices ||
         fits(index_ring, index_bytes, ring_cache_index_draw ? index_cache_key.width : 4)) &&
        frames[frame_index].texture_set_count + 2 <= 4096 &&
        frames[frame_index].image_descriptor_count + 64 <= 16384 &&
        frames[frame_index].sampler_descriptor_count + 64 <= 16384) break;
    if (attempt) break; // an oversized draw uses the ordinary allocation failure path
    if (!SubmitFrame(false, 0)) return skip("submit_failed");
    BeginFrameIfNeeded();
    // The fence-retired batch reset invalidates all old ring offsets. Recheck
    // both tokens and actual allocation size before any transient draw state.
  }

  // Snapshot the ordinary DMA indices here, before textures/constants consume
  // transient state. A hit needs no allocation; a miss always owns a fresh
  // immutable range even if a CPU alias changes the source during conversion.
  const uint64_t index_cache_generation = dma_index_cache.generation();
  const auto prepare_dma_indices = [&](const DmaIndexCache::Entry* cached) {
    prepared_index_hit = cached != nullptr;
    prepared_cpu_hit = false;
    if (cached) {
      prepared_index_offset = cached->offset;
      prepared_index_range = cached->range;
    } else {
      std::vector<uint64_t> ring_tokens, cpu_tokens;
      const bool ring_tracked = reuse_index_draw &&
          dma_index_cache.SnapshotTokens(index_cache_key,ring_tokens,index_token);
      uint8_t* dst = RingAllocate(index_ring,VkDeviceSize(p.index_count)*index_cache_key.width,
                                 index_cache_key.width,prepared_index_offset);
      if (!dst) return false;
      prepared_cpu_hit = prepared_index_draw &&
          prepared_index_cache.TryCopy(prepared_cache_key,dst,prepared_index_range,index_token);
      if (!prepared_cpu_hit) {
        const bool cpu_tracked = prepared_index_draw &&
            prepared_index_cache.SnapshotTokens(prepared_cache_key,cpu_tokens,index_token);
        const uint8_t* input = guest_base+p.index_base_guest;
        const auto endian = xenos::Endian(p.index_endian & 3);
        const auto convert = [&](uint8_t* output) {
          return index_cache_key.width == 4 ?
              CopyVertexIndices<uint32_t,true>(reinterpret_cast<uint32_t*>(output),input,p.index_count,endian) :
              CopyVertexIndices<uint16_t,true>(reinterpret_cast<uint16_t*>(output),input,p.index_count,endian);
        };
        if (cpu_tracked) {
          // Stage on ordinary CPU memory. Reading a write-combined index ring
          // back into the cache on every miss can be very expensive.
          const size_t bytes = size_t(p.index_count)*index_cache_key.width;
          std::vector<uint32_t> staged((bytes+3)/4);
          prepared_index_range = convert(reinterpret_cast<uint8_t*>(staged.data()));
          std::memcpy(dst,staged.data(),bytes);
          prepared_index_cache.Store(prepared_cache_key,std::move(staged),prepared_index_range,
                                     std::move(cpu_tokens),index_token);
        } else prepared_index_range = convert(dst);
      }
      if (ring_tracked)
        dma_index_cache.Store(index_cache_key,prepared_index_offset,prepared_index_range,
                              std::move(ring_tokens),index_token);
    }
    return true;
  };
  if (ring_cache_index_draw && !prepare_dma_indices(reused_indices)) return skip("index_ring_full");
  {
    // The draw initiator is not in the device shadow; the helpers read it.
    reg::VGT_DRAW_INITIATOR di;
    di.value = 0;
    di.prim_type = prim_type;
    di.source_select = p.indexed ? xenos::SourceSelect::kDMA : xenos::SourceSelect::kAutoIndex;
    di.index_size = xenos::IndexFormat(p.index_format & 1);
    di.num_indices = p.index_count & 0xFFFF;
    regs[XE_GPU_REG_VGT_DRAW_INITIATOR] = di.value;
  }
  const bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal)) {
    return skip("rasterization_off");
  }

  // --- Shaders. ---
  ShaderEntry* vs_entry = prepared_vs ? prepared_vs
      : GetShader(p.vertex_shader, xenos::ShaderType::kVertex);
  if (prepared_vs) ++stats.prepared_shader_lookups_avoided;
  ShaderEntry* ps_entry = nullptr;
  if (p.pixel_shader.ucode) {
    ps_entry = prepared_ps ? prepared_ps
        : GetShader(p.pixel_shader, xenos::ShaderType::kPixel);
    if (prepared_ps) ++stats.prepared_shader_lookups_avoided;
  }
  const bool use_dirty_preparation = cache_dirty_preparation;
  auto& dirty = preparation_dirty;
  const auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  const auto sq_context_misc = regs.Get<reg::SQ_CONTEXT_MISC>();
  const auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  SpirvShader* vshader = vs_entry->shader.get();
  SpirvShader* pshader = nullptr;
  uint32_t ps_param_gen_pos = UINT32_MAX, interpolator_mask = 0;
  VkShaderModule vs_module = VK_NULL_HANDLE, ps_module = VK_NULL_HANDLE;
  ShaderEntry* const input_ps = ps_entry;
  const bool shader_preparation_hit = use_dirty_preparation && prepared_shaders.valid &&
      !(dirty.pending & DrawPreparationDirtyState::kShader) &&
      prepared_shaders.input_vs == vs_entry && prepared_shaders.input_ps == input_ps &&
      prepared_shaders.primitive == p.primitive_type;
  if (shader_preparation_hit) {
    ps_entry = prepared_shaders.selected_ps;
    pshader = ps_entry ? ps_entry->shader.get() : nullptr;
    ps_param_gen_pos = prepared_shaders.param_gen_pos;
    interpolator_mask = prepared_shaders.interpolator_mask;
    vs_module = prepared_shaders.vs;
    ps_module = prepared_shaders.ps;
    ++stats.preparation_shader_hits;
  } else {
    // Depth-only EDRAM mode bypasses the pixel shader, including inherited
    // alpha tests. Forza leaves its UI shader bound while issuing SDK clears.
    // Also omit shaders with no colour, coverage, depth or memory-export effect.
    if (ps_entry && !draw_util::IsPixelShaderNeededWithRasterization(*ps_entry->shader, regs))
      ps_entry = nullptr;
    pshader = ps_entry ? ps_entry->shader.get() : nullptr;
    interpolator_mask =
        pshader ? (vshader->writes_interpolators() &
                   pshader->GetInterpolatorInputMask(sq_program_cntl, sq_context_misc,
                                                     ps_param_gen_pos))
                : 0;

    SpirvShaderTranslator::Modification vmod(translator->GetDefaultVertexShaderModification(
        vshader->GetDynamicAddressableRegisterCount(sq_program_cntl.vs_num_reg),
        expand_rects ? Shader::HostVertexShaderType::kRectangleListAsTriangleStrip
                     : expand_points ? Shader::HostVertexShaderType::kPointListAsTriangleStrip
                                     : Shader::HostVertexShaderType::kVertex));
    vmod.vertex.interpolator_mask = interpolator_mask;
    vmod.vertex.output_point_parameters = uint32_t(expand_points && ps_param_gen_pos < xenos::kMaxInterpolators);
    const auto ucp = rex::graphics::native::ComputeUserClipPlanes(
        pa_cl_clip_cntl.clip_disable != 0, pa_cl_clip_cntl.ucp_ena,
        pa_cl_clip_cntl.ucp_cull_only_ena != 0, supports_clip_distance, supports_cull_distance);
    vmod.vertex.user_clip_plane_count = ucp.count;
    vmod.vertex.user_clip_plane_cull = ucp.cull;
    vmod.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
    vmod.vertex.vertex_kill_and = uint32_t(rex::graphics::native::ComputeVertexKillAnd(
        supports_cull_distance, vshader->writes_point_size_edge_flag_kill_vertex(),
        pa_cl_clip_cntl.vtx_kill_or != 0));
    Shader::Translation* vtrans = vshader->GetOrCreateTranslation(vmod.value);
    if (!vtrans->is_translated() && !translator->TranslateAnalyzedShader(*vtrans)) {
      return skip("vs_translate_failed");
    }
    if (!vtrans->is_valid()) return skip("vs_invalid");
    vs_module = GetShaderModule(*vs_entry, vmod.value, vtrans);
    if (!vs_module) return skip("vs_module");

    if (pshader) {
      SpirvShaderTranslator::Modification pmod(translator->GetDefaultPixelShaderModification(
          pshader->GetDynamicAddressableRegisterCount(sq_program_cntl.ps_num_reg)));
      pmod.pixel.interpolator_mask = interpolator_mask;
      // Xenos can request centroid interpolation for only some inputs. Using
      // centres for those inputs extrapolates values outside partially covered
      // triangles, which is especially visible on fading or cutout layers.
      static const bool centres_only = [] {
        const char* value = std::getenv("XN_VK_CENTROID");
        return value && std::strcmp(value, "0") == 0;
      }();
      pmod.pixel.interpolators_centroid = centres_only ? 0 : interpolator_mask &
          ~xenos::GetInterpolatorSamplingPattern(
              regs.Get<reg::RB_SURFACE_INFO>().msaa_samples, sq_context_misc.sc_sample_cntl,
              regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
      if (ps_param_gen_pos < xenos::kMaxInterpolators) {
        pmod.pixel.param_gen_enable = 1;
        pmod.pixel.param_gen_interpolator = ps_param_gen_pos;
        pmod.pixel.param_gen_point = uint32_t(prim_type == xenos::PrimitiveType::kPointList);
      }
      pmod.pixel.depth_stencil_mode =
          SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers;
      Shader::Translation* ptrans = pshader->GetOrCreateTranslation(pmod.value);
      if (!ptrans->is_translated() && !translator->TranslateAnalyzedShader(*ptrans)) {
        return skip("ps_translate_failed");
      }
      if (!ptrans->is_valid()) return skip("ps_invalid");
      ps_module = GetShaderModule(*ps_entry, pmod.value, ptrans);
      if (!ps_module) return skip("ps_module");
    }

    if (use_dirty_preparation) {
      prepared_shaders = {true, vs_entry, input_ps, ps_entry,
          p.primitive_type, interpolator_mask, ps_param_gen_pos, vs_module, ps_module};
      dirty.pending &= ~DrawPreparationDirtyState::kShader;
      // Downstream caches key the shader properties they actually consume:
      // depth output, used texture mask, modules and descriptor layout.
    }
  }
  if (expand_points) topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  // XN_VK_DEBUG_PS=1: run the guest vertex shader with a solid-colour pixel
  // shader, to tell "the vertex shader produces no geometry" from "the pixel
  // shader produces no colour".
  // XN_VK_DEBUG_PS=quads restricts the substitution to quad lists, so a
  // full-target rect "wipe" drawn with the guest shader does not mask the
  // result of the draw being investigated.
  static const char* debug_ps_mode = std::getenv("XN_VK_DEBUG_PS");
  if (debug_ps_mode && (std::strcmp(debug_ps_mode, "quads") != 0 ||
                        prim_type == xenos::PrimitiveType::kQuadList)) {
    static VkShaderModule solid = VK_NULL_HANDLE;
    if (!solid) {
      VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      ci.codeSize = sizeof(kDebugSolidFrag);
      ci.pCode = kDebugSolidFrag;
      vkCreateShaderModule(device, &ci, nullptr, &solid);
    }
    ps_module = solid;
  }

  // XN_VK_NO_BLOOM=1: drop the two tonemap composite draws, to measure how
  // much of the brightness excess over the oracle they account for.
  static const bool no_bloom = std::getenv("XN_VK_NO_BLOOM") != nullptr;
  if (pshader && no_bloom) {
    const uint64_t ps_hash = pshader->ucode_data_hash();
    if (ps_hash == 0x5C4BCB6B0AC7678Eull || ps_hash == 0xEC8FDC749974EDBAull) {
      return skip("bloom_composite_disabled");
    }
  }

  const bool sdk_depth_clear =
      (!pshader || (!pshader->kills_pixels() && !pshader->writes_depth())) &&
      ApplyDepthClearAliases(p, *vshader);

  // --- Render targets (every bound colour target + depth). ---
  // A pixel shader may export up to four colour targets. Burnout Revenge's
  // scene pass writes colour to oC0 and per-pixel motion vectors to oC1; with
  // only attachment 0 bound the motion buffer stays at its cleared value, and
  // the title's composite reads "no motion" as mid-grey, so an all-zero buffer
  // asks for a full-length blur on every pixel.
  const auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const auto rb_color_info = regs.Get<reg::RB_COLOR_INFO>();
  const auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  const uint32_t normalized_color_mask =
      draw_util::GetNormalizedColorMask(regs, pshader ? pshader->writes_color_targets() : 0);
  // Slots stay at their guest index: a shader that writes oC1 but not oC0 must
  // still land in attachment 1, so gaps are bound as unused attachments rather
  // than closed up.
  // Multiple render targets are OPT-IN (XN_VK_MRT=1; Burnout Revenge's title
  // sets it). Binding every written target is what a title with an oC1 export
  // needs, but it also changes how often the attachment set turns over, and on
  // the heavier titles - Midnight Club LA and PGR4 - that regressed badly
  // enough to be unusable. Default off until that is understood; Burnout needs
  // it and is unaffected.
  // XN_VK_MRT=0 forces it off even for a title that opts itself in, so the cost
  // of the second attachment can be measured against the same build.
  static const bool no_mrt = [] {
    const char* v = std::getenv("XN_VK_MRT");
    return !v || v[0] == '0';
  }();
  RenderTargetImage* colors[xenos::kMaxColorRenderTargets] = {};
  uint32_t color_count = 0;
  RenderTargetImage* shape = nullptr;
  for (uint32_t i = 0; !no_mrt && i < xenos::kMaxColorRenderTargets; ++i) {
    if (!((normalized_color_mask >> (i * 4)) & 0xF)) continue;
    const auto info =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    RenderTargetImage* rt =
        GetColorTarget(RenderTargetBase(info.value), info.color_format,
                       rb_surface_info.surface_pitch);
    if (!rt) continue;
    // Vulkan wants one render area for the whole pass; a target of a different
    // shape would make it invalid. Shapes come from the shared surface pitch
    // and scissor, so this is a guard, not an expected case.
    if (shape && (rt->image.width != shape->image.width ||
                  rt->image.height != shape->image.height || rt->samples != shape->samples)) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        REXLOG_WARN("xn-vk: colour target {} is {}x{} against {}x{} on the first bound target; "
                    "leaving it unbound",
                    i, rt->image.width, rt->image.height, shape->image.width,
                    shape->image.height);
      }
      continue;
    }
    if (!shape) shape = rt;
    colors[i] = rt;
    color_count = i + 1;
  }
  // Attachment 0 must exist even when the shader writes nothing (clears, and
  // draws whose colour mask is empty but which still run depth).
  if (!color_count) {
    RenderTargetImage* rt = GetColorTarget(RenderTargetBase(rb_color_info.value), rb_color_info.color_format,
                                           rb_surface_info.surface_pitch);
    if (!rt) return skip("color_target");
    colors[0] = rt;
    color_count = 1;
  }
  // The shape/viewport source: the first target actually bound.
  RenderTargetImage* color = nullptr;
  for (uint32_t i = 0; i < color_count && !color; ++i) color = colors[i];
  if (!color) return skip("color_target");
  RenderTargetImage* depth =
      GetDepthTarget(RenderTargetBase(rb_depth_info.value), color->image.width, color->image.height,
                     color->samples);
  if (!depth) return skip("depth_target");

  // --- Textures (may record uploads: before the render pass). ---
  VkDescriptorSet vs_textures = AllocateTextureSet(vshader, regs, vs_module);
  VkDescriptorSet ps_textures = pshader ? AllocateTextureSet(pshader, regs, ps_module) : VK_NULL_HANDLE;

  // --- Index buffer -> host index ring. ---
  bool indexed = false;
  VkDeviceSize index_offset = 0;
  VkBuffer index_buffer = index_ring.buf.buffer;
  VkIndexType index_type = VK_INDEX_TYPE_UINT16;
  uint32_t draw_count = p.index_count;
  GuestIndexSource src;
  if (p.indexed) {
    const bool is32 = (p.index_format & 1) != 0;
    const uint64_t bytes = uint64_t(p.index_count) * (is32 ? 4 : 2);
    if (p.index_host_data) {
      src.data = p.index_host_data;
    } else {
      if (!guest_base || uint64_t(p.index_base_guest) + bytes > uint64_t(guest_size)) {
        return skip("index_range");
      }
      PrepareGuestRead(p.index_base_guest,uint32_t(bytes));
      src.data = guest_base + p.index_base_guest;
    }
    src.is_32bit = is32;
    src.endianness = xenos::Endian(p.index_endian & 3);
  }
  // Proven direct or scaled fetches read a bounded range derived from this
  // draw's indices. Most city fetch constants describe the remaining buffer
  // capacity; hashing all of it for every material draw wastes bandwidth.
  uint32_t first_vertex = UINT32_MAX, last_vertex = 0;
  const uint32_t vertex_min = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);
  const uint32_t vertex_max = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  const uint32_t vertex_base = regs.Get<uint32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  static const bool full_vertex_ranges = std::getenv("XN_VK_FULL_VERTEX_RANGES") != nullptr;
  const bool bound_vertices = !full_vertex_ranges && vs_entry->bounded_vertex_fetches.any() &&
      vertex_min <= vertex_max && vertex_max <= xenos::kVertexIndexMask;
  if (bound_vertices && !p.indexed) {
    const auto range = RemapVertexIndices({0, p.index_count - 1}, vertex_base, vertex_min, vertex_max);
    first_vertex = range.first;
    last_vertex = range.last;
  }
  if (expand_points) {
    // The translator expands each point using the low two index bits for its
    // corners. This supplies SG's particle size and generated texture UVs;
    // native Vulkan points would require a different gl_PointSize/PointCoord ABI.
    const uint32_t n = p.index_count * 6;
    uint8_t* dst = RingAllocate(index_ring, VkDeviceSize(n) * 4, 4, index_offset);
    if (!dst) return skip("index_ring_full");
    auto* out = reinterpret_cast<uint32_t*>(dst);
    constexpr uint32_t corners[] = {0, 1, 2, 2, 1, 3};
    for (uint32_t i = 0; i < p.index_count; ++i)
      for (uint32_t corner : corners) *out++ = (src.Read(i) << 2) | corner;
    index_type = VK_INDEX_TYPE_UINT32;
    indexed = true;
    draw_count = n;
  } else if (expand_rects) {
    const uint32_t n = RectangleListExpandedCount(p.index_count);
    if (!n) return skip("rect_count_zero");
    uint8_t* dst = RingAllocate(index_ring, VkDeviceSize(n) * 4, 4, index_offset);
    if (!dst) return skip("index_ring_full");
    ExpandRectangleList(p.index_count, src, reinterpret_cast<uint32_t*>(dst));
    index_type = VK_INDEX_TYPE_UINT32;
    indexed = true;
    draw_count = n;
  } else if (prim_type == xenos::PrimitiveType::kQuadList) {
    // XN_VK_PRIM_LIMIT=<n>: draw only the first n quads of a large quad-list
    // pass, so ONE primitive can be isolated and its footprint and deposited
    // light measured directly. Tests the assumption (does a single primitive
    // deposit the same light?) and the conclusion (is the excess overdraw?) in
    // one measurement.
    uint32_t quad_index_count = p.index_count;
    static const char* primitive_limit = std::getenv("XN_VK_PRIM_LIMIT");
    if (const char* pl = primitive_limit) {
      if (p.index_count > 10000) {
        const uint32_t quads = uint32_t(std::atoi(pl));
        quad_index_count = std::min(p.index_count, quads * 4);
        static bool announced = false;
        if (!announced) {
          announced = true;
          REXLOG_INFO("xn-vk: PRIMLIMIT active - large quad passes clamped to {} quad(s)", quads);
        }
      }
    }
    const uint32_t n = QuadListExpandedCount(quad_index_count);
    if (!n) return skip("quad_count_zero");
    const bool out32 = QuadListNeeds32Bit(quad_index_count, src);
    const VkDeviceSize elem = out32 ? 4 : 2;
    uint8_t* dst = RingAllocate(index_ring, VkDeviceSize(n) * elem, elem, index_offset);
    if (!dst) return skip("index_ring_full");
    if (out32) {
      ExpandQuadList(quad_index_count, src, reinterpret_cast<uint32_t*>(dst));
      index_type = VK_INDEX_TYPE_UINT32;
    } else {
      ExpandQuadList(quad_index_count, src, reinterpret_cast<uint16_t*>(dst));
    }
    indexed = true;
    draw_count = n;
  } else if (primitive_restart) {
    uint8_t* dst = RingAllocate(index_ring, VkDeviceSize(p.index_count) * 4, 4, index_offset);
    if (!dst) return skip("index_ring_full");
    auto* out = reinterpret_cast<uint32_t*>(dst);
    const uint32_t reset_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    if (bound_vertices) {
      auto range = CopyRestartVertexIndices<true>(out, src, p.index_count, reset_index);
      if (range.first != UINT32_MAX) {
        range = RemapVertexIndices(range, vertex_base, vertex_min, vertex_max);
        first_vertex = range.first; last_vertex = range.last;
      }
    } else CopyRestartVertexIndices<false>(out, src, p.index_count, reset_index);
    index_type = VK_INDEX_TYPE_UINT32;
    indexed = true;
  } else if (resident_indices) {
    index_buffer = resident_index_buffer.buffer;
    index_offset = resident_indices.offset();
    index_type = index_cache_key.width == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (bound_vertices) {
      const auto range = RemapVertexIndices(resident_indices.range(),vertex_base,vertex_min,vertex_max);
      first_vertex = range.first;
      last_vertex = range.last;
    }
    const uint64_t bytes = uint64_t(p.index_count)*index_cache_key.width;
    if (resident_index_hit) {
      ++stats.resident_index_hits;
      stats.resident_index_bytes_reused += bytes;
    } else {
      ++stats.resident_index_misses;
      stats.resident_index_upload_bytes += bytes;
    }
    indexed = true;
  } else if (ring_cache_index_draw) {
    // BeginFrameIfNeeded ends the ring-offset cache lifetime, including
    // batches within one guest frame. A helper may have retired this ring
    // allocation; recopy or relookup in the new ring before recording the draw.
    if (dma_index_cache.generation() != index_cache_generation &&
        !prepare_dma_indices(reuse_index_draw ?
            dma_index_cache.Find(index_cache_key,index_token) : nullptr))
      return skip("index_ring_full");
    index_offset = prepared_index_offset;
    index_type = index_cache_key.width == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (bound_vertices) {
      const auto range = RemapVertexIndices(prepared_index_range,vertex_base,vertex_min,vertex_max);
      first_vertex = range.first; last_vertex = range.last;
    }
    if (prepared_index_hit) {
      ++stats.index_cache_hits;
      const uint64_t bytes = uint64_t(p.index_count)*index_cache_key.width;
      stats.converted_bytes_avoided += bytes;
      stats.index_ring_bytes_avoided += bytes;
    } else if (prepared_cpu_hit) {
      ++stats.index_cache_hits;
      stats.converted_bytes_avoided += uint64_t(p.index_count)*index_cache_key.width;
    } else ++stats.index_cache_misses;
    indexed = true;
  } else if (p.indexed) {
    const VkDeviceSize elem = src.is_32bit ? 4 : 2;
    uint8_t* dst = RingAllocate(index_ring, VkDeviceSize(p.index_count) * elem, elem, index_offset);
    if (!dst) return skip("index_ring_full");
    if (src.is_32bit) {
      auto* out = reinterpret_cast<uint32_t*>(dst);
      if (bound_vertices) {
        const auto range = RemapVertexIndices(
            CopyVertexIndices<uint32_t, true>(out, src.data, p.index_count, src.endianness),
            vertex_base, vertex_min, vertex_max);
        first_vertex = range.first; last_vertex = range.last;
      } else CopyVertexIndices<uint32_t, false>(out, src.data, p.index_count, src.endianness);
      index_type = VK_INDEX_TYPE_UINT32;
    } else {
      auto* out = reinterpret_cast<uint16_t*>(dst);
      if (bound_vertices) {
        const auto range = RemapVertexIndices(
            CopyVertexIndices<uint16_t, true>(out, src.data, p.index_count, src.endianness),
            vertex_base, vertex_min, vertex_max);
        first_vertex = range.first; last_vertex = range.last;
      } else CopyVertexIndices<uint16_t, false>(out, src.data, p.index_count, src.endianness);
    }
    indexed = true;
  }

  // Expanded DMA-indexed primitives keep the full range: their host indices
  // encode points/corners (and indexed rectangles currently ignore the DMA
  // indices). The ordinary index-copy path above bounds exactly its snapshot.
  for (const Shader::VertexBinding& vb : vshader->vertex_bindings()) {
    const auto fetch = regs.GetVertexFetch(vb.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex || !fetch.size) continue;
    uint32_t address = fetch.address << 2, bytes = fetch.size << 2;
    // Floating point index rounding is exact below 2^23. Validate the original
    // allocation first so narrowing cannot wrap an invalid address into memory.
    if (first_vertex != UINT32_MAX && last_vertex < (1u << 23) &&
        vs_entry->bounded_vertex_fetches.test(vb.fetch_constant) &&
        uint64_t(address) + bytes <= std::min<uint64_t>(guest_size, kGuestPhysicalSize)) {
      const auto extent_words = vs_entry->bounded_vertex_extents[vb.fetch_constant];
      VertexIndexRange range{first_vertex, last_vertex};
      bool valid = vb.fetch_constant != vs_entry->scaled_vertex_fetch_slot ||
          ScaleVertexIndexRange(range, regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X +
                                                       vs_entry->scaled_vertex_index_constant));
      if (valid && vb.fetch_constant == vs_entry->palette_vertex_fetch.slot)
        valid = BoundPaletteVertexFetch(range, vs_entry->palette_vertex_fetch, [&](uint32_t component) {
          return regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + component);
        });
      if (valid && vb.fetch_constant == vs_entry->affine_vertex_fetch.slot)
        valid = BoundAffineVertexFetch(range,
            regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + vs_entry->affine_vertex_fetch.offset_constant),
            regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + vs_entry->affine_vertex_fetch.scale_constant));
      const uint64_t lo = uint64_t(range.first) * vb.stride_words * 4;
      const uint64_t hi = (uint64_t(range.last) * vb.stride_words + extent_words) * 4;
      if (valid && lo < hi && hi <= bytes && hi - lo < bytes) {
        address += uint32_t(lo);
        bytes = uint32_t(hi - lo);
        ++stats.vertex_ranges_bounded;
      }
    }
    static const bool profile_vertex_reads = std::getenv("XN_VERTEX_PROFILE") != nullptr;
    if (profile_vertex_reads) {
      auto& profile = vs_entry->vertex_read_profile[vb.fetch_constant];
      ++profile.calls;
      profile.requested_bytes += uint64_t(fetch.size) << 2;
      profile.mirrored_bytes += ((uint64_t(address) + bytes + kMirrorPageSize - 1) /
          kMirrorPageSize - address / kMirrorPageSize) * kMirrorPageSize;
    }
    if (!MirrorGuestRange(address, bytes)) return skip("vertex_range");
  }

  if (debug_frame_active && draw_number - debug_frame_first_draw < 8) {
    std::string vtx, idx;
    if (!vshader->vertex_bindings().empty()) {
      const auto& vb = vshader->vertex_bindings()[0];
      xenos::xe_gpu_vertex_fetch_t f = regs.GetVertexFetch(vb.fetch_constant);
      const uint32_t base = f.address << 2;
      if (uint64_t(base) + 32 <= uint64_t(guest_size)) {
        // The mirror holds raw guest (big-endian) dwords; the shader swaps per
        // the fetch constant's endian. Show the swapped view, which is what
        // the vertex shader actually sees.
        const uint32_t* raw = reinterpret_cast<const uint32_t*>(guest_base + base);
        float sw[8];
        for (int i = 0; i < 8; ++i) {
          uint32_t v = __builtin_bswap32(raw[i]);
          std::memcpy(&sw[i], &v, 4);
        }
        // First four vertices' positions, and where the mad(c1,c0) in the
        // vertex shader puts them, so a degenerate quad is obvious.
        const uint32_t stride = vb.stride_words ? vb.stride_words : 8;
        const float c1x = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 4);
        const float c1y = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 5);
        const float c0x = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 0);
        const float c0y = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 1);
        vtx = fmt::format(" stride={} vtx@{:#x}", stride, base);
        for (uint32_t v = 0; v < 4; ++v) {
          const uint64_t off = uint64_t(base) + uint64_t(v) * stride * 4;
          if (off + 8 * sizeof(uint32_t) > uint64_t(guest_size)) break;
          const uint32_t* r2 = reinterpret_cast<const uint32_t*>(guest_base + off);
          float px, py;
          uint32_t bx = __builtin_bswap32(r2[0]), by = __builtin_bswap32(r2[1]);
          std::memcpy(&px, &bx, 4);
          std::memcpy(&py, &by, 4);
          // uv at dword 2-3, colour at 4-7 (the shader's vfetch_mini offsets)
          float f[8];
          for (int i = 0; i < 8; ++i) {
            uint32_t bv = __builtin_bswap32(r2[i]);
            std::memcpy(&f[i], &bv, 4);
          }
          // Raw words too: the stride-7 layout (VS 6DD9: xyz + rgba) puts the
          // colour at 3..6, the stride-8 one (VS 1139: xy + uv + rgba) at 4..7.
          vtx += fmt::format(" v{}=({:.1f},{:.1f})->({:.0f},{:.0f}) w=[{:.3f},{:.3f},{:.3f},{:.3f},"
                             "{:.3f},{:.3f},{:.3f},{:.3f}]",
                             v, px, py, px * c1x + c0x, py * c1y + c0y, f[0], f[1], f[2], f[3],
                             f[4], f[5], f[6], f[7]);
        }
      }
    }
    const HostBuffer& chosen_index_buffer = resident_indices ? resident_index_buffer : index_ring.buf;
    const uint32_t index_width = index_type == VK_INDEX_TYPE_UINT16 ? 2 : 4;
    const uint32_t dump_indices = std::min(8u, draw_count);
    if (indexed && chosen_index_buffer.mapped && index_offset <= chosen_index_buffer.size &&
        uint64_t(dump_indices) * index_width <= chosen_index_buffer.size - index_offset) {
      idx = fmt::format(" idx{}=[", index_width * 8);
      for (uint32_t i = 0; i < dump_indices; ++i) {
        const uint8_t* data = chosen_index_buffer.mapped + index_offset + uint64_t(i) * index_width;
        uint32_t value = 0;
        if (index_width == 2) { uint16_t narrow; std::memcpy(&narrow, data, 2); value = narrow; }
        else std::memcpy(&value, data, 4);
        idx += fmt::format("{}{}", i ? "," : "", value);
      }
      idx += "]";
    }
    REXLOG_INFO("xn-vk: F{} d{} inputs: indexed={} base={:#x} fmt={} endian={}{}{}", frame_number,
                draw_number, indexed, p.index_base_guest, p.index_format, p.index_endian, vtx, idx);
  }

  // --- Viewport / scissor. ---
  draw_util::ViewportInfo viewport_info;
  VkViewport viewport;
  const bool half_pixel = REXCVAR_GET(half_pixel_offset);
  const bool writes_depth = pshader && pshader->writes_depth();
  const bool viewport_preparation_hit = use_dirty_preparation && prepared_viewport.valid &&
      !(dirty.pending & DrawPreparationDirtyState::kViewport) &&
      prepared_viewport.width == color->image.width && prepared_viewport.height == color->image.height &&
      prepared_viewport.writes_depth == writes_depth && prepared_viewport.half_pixel == half_pixel;
  if (viewport_preparation_hit) {
    viewport_info = prepared_viewport.info;
    viewport = prepared_viewport.viewport;
    ++stats.preparation_viewport_hits;
  } else {
    draw_util::GetHostViewportInfo(regs, 1, 1, false, properties.limits.maxViewportDimensions[0],
                                   properties.limits.maxViewportDimensions[1], true, depth_control,
                                   false, false, pshader && pshader->writes_depth(), viewport_info);
    if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
      viewport.x = float(viewport_info.xy_offset[0]);
      viewport.y = float(viewport_info.xy_offset[1]);
      viewport.width = float(viewport_info.xy_extent[0]);
      viewport.height = float(viewport_info.xy_extent[1]);
    } else {
      viewport = {-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    }
    viewport.minDepth = viewport_info.z_min;
    viewport.maxDepth = viewport_info.z_max;
  }
  if (debug_frame_active && launch_diagnostics.depth_clear &&
      pshader && pshader->ucode_data_hash() == 0x89F06E34104DF40Cull) {
    REXLOG_INFO("xn-vk: WORLDDEPTH swap={} raw={:#x} base={:#x} rt={}x{} samples={} guest_msaa={} "
                "format={} z={}..{} ndc_z={},{} guest_z={},{} dc={:#x}",
                guest_swap_number, rb_depth_info.value, depth->edram_base,
                depth->image.width, depth->image.height, uint32_t(depth->samples),
                uint32_t(depth->guest_msaa), depth->xenos_format, viewport.minDepth,
                viewport.maxDepth, viewport_info.ndc_scale[2], viewport_info.ndc_offset[2],
                regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
                regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET), depth_control.value);
  }
  // XN_DEBUG_FILLPASS=<n>: for the first n draws of each frame, the shaders and
  // the constants that set this pass's geometry scale, read from BOTH constant
  // banks so the bank cannot be silently wrong. Vertex constants live at
  // 0x4000 and pixel constants at 0x4400 - exactly 256 quads apart - and
  // reading a pixel shader's constants out of the vertex bank yields another
  // shader's numbers entirely. Printing both makes that visible rather than
  // plausible.
  static const uint32_t fillpass_steps = [] {
    const char* e = std::getenv("XN_DEBUG_FILLPASS");
    if (!e) return 0u;
    const int n = std::atoi(e);
    return n > 0 ? uint32_t(n) : 13u;
  }();
  if (fillpass_steps && cur_limit_step < fillpass_steps) {
    auto vb = [&](uint32_t i, uint32_t c) {
      return regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4 + c);
    };
    auto pb = [&](uint32_t i, uint32_t c) {
      return regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + c);
    };
    REXLOG_INFO(
        "xn-vk: FILLPASS F{} step={} rt={}x{} vs={:016X} ps={:016X} count={} "
        "VTXbank c0=({:.5g},{:.5g}) c1=({:.5g},{:.5g}) | c255 PIXbank=({:.5g},{:.5g}) "
        "VTXbank=({:.5g},{:.5g})",
        frame_number, cur_limit_step, color->image.width, color->image.height,
        vshader->ucode_data_hash(), pshader ? pshader->ucode_data_hash() : 0ull, p.index_count,
        vb(0, 0), vb(0, 1), vb(1, 0), vb(1, 1), pb(255, 0), pb(255, 1), vb(255, 0), vb(255, 1));
  }
  // XN_DEBUG_DERIVED_VP=<n>: report the first n draws of each frame (default
  // 13). The second title's bloom passes are draws 70-80, so the default window
  // does not reach them.
  static const uint32_t derived_vp_steps = [] {
    const char* e = std::getenv("XN_DEBUG_DERIVED_VP");
    if (!e) return 0u;
    const int n = std::atoi(e);
    return n > 0 ? uint32_t(n) : 13u;
  }();
  if (derived_vp_steps && cur_limit_step < derived_vp_steps) {
    REXLOG_INFO("xn-vk: DERIVEDVP F{} step={} rt={}x{} host_vp=({},{} {}x{}) ndc_scale=({},{},{}) "
                "ndc_offset=({},{},{})",
                frame_number, cur_limit_step, color->image.width, color->image.height, viewport.x,
                viewport.y, viewport.width, viewport.height, viewport_info.ndc_scale[0],
                viewport_info.ndc_scale[1], viewport_info.ndc_scale[2],
                viewport_info.ndc_offset[0], viewport_info.ndc_offset[1],
                viewport_info.ndc_offset[2]);
  }
  VkRect2D scissor_rect;
  if (viewport_preparation_hit) scissor_rect = prepared_viewport.scissor;
  else {
    draw_util::Scissor scissor;
    draw_util::GetScissor(regs, scissor);
    scissor_rect.offset.x = int32_t(std::min(scissor.offset[0], color->image.width));
    scissor_rect.offset.y = int32_t(std::min(scissor.offset[1], color->image.height));
    scissor_rect.extent.width =
        std::min(scissor.extent[0], color->image.width - uint32_t(scissor_rect.offset.x));
    scissor_rect.extent.height =
        std::min(scissor.extent[1], color->image.height - uint32_t(scissor_rect.offset.y));
    if (use_dirty_preparation) {
      prepared_viewport = {true, writes_depth, half_pixel, color->image.width,
          color->image.height, viewport_info, viewport, scissor_rect};
      dirty.pending &= ~DrawPreparationDirtyState::kViewport;
      dirty.pending |= DrawPreparationDirtyState::kSystem;
    }
  }
  if (!scissor_rect.extent.width || !scissor_rect.extent.height) return skip("empty_scissor");

  // --- System constants. ---
  SpirvShaderTranslator::SystemConstants uncached_system;
  auto& sc = use_dirty_preparation ? prepared_system.values : uncached_system;
  const auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  const uint32_t used_texture_mask = vshader->GetUsedTextureMaskAfterTranslation() |
      (pshader ? pshader->GetUsedTextureMaskAfterTranslation() : 0);
  const bool system_preparation_hit = use_dirty_preparation && prepared_system.valid &&
      !(dirty.pending & DrawPreparationDirtyState::kSystem) &&
      prepared_system.texture_mask == used_texture_mask &&
      prepared_system.primitive == p.primitive_type;
  if (system_preparation_hit) ++stats.preparation_system_hits;
  else {
    // A failed upload must never leave an older offset eligible for reuse
    // after the CPU-derived values have already been refreshed.
    if (use_dirty_preparation)
      dirty_constant_bindings[SpirvShaderTranslator::kConstantBufferSystem].batch = UINT64_MAX;
    std::memset(&sc, 0, sizeof(sc));
    uint32_t flags = 0;
    const auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
    if (pa_cl_vte_cntl.vtx_xy_fmt) flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
    if (pa_cl_vte_cntl.vtx_z_fmt) flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
    if (pa_cl_vte_cntl.vtx_w0_fmt) flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
    if (primitive_polygonal) flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
    if (draw_util::IsPrimitiveLine(regs)) flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
    flags |= uint32_t(rb_surface_info.msaa_samples)
             << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
    const xenos::CompareFunction alpha_func = rb_colorcontrol.alpha_test_enable
                                                  ? rb_colorcontrol.alpha_func
                                                  : xenos::CompareFunction::kAlways;
    flags |= uint32_t(alpha_func) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
    if (rb_color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma;
    }
    sc.flags = flags;
    sc.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
    // The translated pixel shader enables alpha-to-coverage on
    // `alpha_to_mask != 0` - the whole word, not bit 8 - so this must be exactly
    // zero when the guest has it disabled, or the dither offsets in bits 0:7
    // switch it on for every draw and dither away the low-alpha passes.
    // Matches ReXGlue's UpdateSystemConstantValues.
    sc.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                           ? ((rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8))
                           : 0;
    for (uint32_t i = 0; i < 3; ++i) {
      sc.ndc_scale[i] = viewport_info.ndc_scale[i];
      sc.ndc_offset[i] = viewport_info.ndc_offset[i];
    }
    if (prim_type == xenos::PrimitiveType::kPointList) {
      const auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
      const auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
      sc.point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
      sc.point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
      sc.point_constant_diameter[0] = float(pa_su_point_size.width) * (2.0f / 16.0f);
      sc.point_constant_diameter[1] = float(pa_su_point_size.height) * (2.0f / 16.0f);
      sc.point_screen_diameter_to_ndc_radius[0] =
          1.0f / std::max(viewport_info.xy_extent[0], 1u);
      sc.point_screen_diameter_to_ndc_radius[1] =
          1.0f / std::max(viewport_info.xy_extent[1], 1u);
    }
    for (uint32_t i = 0; i < 4; ++i) {
      const auto rt_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      const bool fixed16 = rt_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
                           rt_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16;
      sc.color_exp_bias[i] = rex::graphics::native::ColorExpBiasScale(
          rex::graphics::native::AdjustColorExpBiasForFormat(rt_info.color_exp_bias, fixed16,
                                                              false));
    }
    if (!pa_cl_clip_cntl.clip_disable) {
      float* ucp_write = sc.user_clip_planes[0];
      uint32_t remaining = pa_cl_clip_cntl.ucp_ena & 0x3Fu;
      uint32_t idx;
      while (rex::bit_scan_forward(remaining, &idx)) {
        remaining &= ~(1u << idx);
        std::memcpy(ucp_write, &regs[XE_GPU_REG_PA_CL_UCP_0_X + idx * 4], 4 * sizeof(float));
        ucp_write += 4;
      }
    }

    if (use_dirty_preparation) {
      prepared_system.valid = true;
      prepared_system.texture_mask = used_texture_mask;
      prepared_system.primitive = p.primitive_type;
      dirty.pending &= ~DrawPreparationDirtyState::kSystem;
    }
  }
  // Index bounds/base and fetch signedness change much more often than
  // viewport transforms and colour conversion. Patch only those fields;
  // unchanged writes still retain the immutable system upload offset.
  bool system_values_unchanged = system_preparation_hit;
  if (!system_preparation_hit || (dirty.pending & DrawPreparationDirtyState::kSystemDynamic)) {
    const int32_t vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
    const uint32_t vertex_index_min = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);
    const uint32_t vertex_index_max = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
    uint32_t signs[8]{};
    static_assert(sizeof(signs) == sizeof(sc.texture_swizzled_signs));
    uint32_t used = used_texture_mask, t;
    while (rex::bit_scan_forward(used, &t)) {
      used &= ~(1u << t);
      signs[t >> 2] |= uint32_t(SwizzleSigns(regs.GetTextureFetch(t))) << (8 * (t & 3));
    }
    system_values_unchanged = system_preparation_hit &&
        sc.vertex_base_index == vertex_base_index && sc.vertex_index_min == vertex_index_min &&
        sc.vertex_index_max == vertex_index_max &&
        !std::memcmp(sc.texture_swizzled_signs, signs, sizeof(signs));
    sc.vertex_base_index = vertex_base_index;
    sc.vertex_index_min = vertex_index_min;
    sc.vertex_index_max = vertex_index_max;
    std::memcpy(sc.texture_swizzled_signs, signs, sizeof(signs));
    if (use_dirty_preparation) {
      dirty.pending &= ~DrawPreparationDirtyState::kSystemDynamic;
      if (!system_values_unchanged)
        dirty_constant_bindings[SpirvShaderTranslator::kConstantBufferSystem].batch = UINT64_MAX;
    }
  }
  // XN_DEBUG_DEPTH=1: the depth/stencil state for each step, and whether we
  // bind a depth attachment at all. Depth and stencil are the one register
  // group nobody has read all night: every check so far has been colour state.
  // A depth or stencil test the console applies and we ignore rejects
  // overlapping fragments on their side and accumulates them on ours - more
  // light in proportion to overlap depth, none without overlap, invisible in
  // every input, colour register, geometry and coverage measurement.
  static const bool debug_depth = std::getenv("XN_DEBUG_DEPTH") != nullptr;
  if (debug_depth && cur_limit_step < 13) {
    const auto dc = regs.Get<reg::RB_DEPTHCONTROL>();
    const auto sm = regs.Get<reg::RB_STENCILREFMASK>();
    REXLOG_INFO(
        "xn-vk: DEPTHSTATE F{} step={} rt={}x{} depth_attachment={} RB_DEPTHCONTROL={:#x} "
        "(z_enable={} z_write={} z_func={} stencil_enable={} backface={}) "
        "RB_STENCILREFMASK={:#x} (ref={} mask={:#x} writemask={:#x}) RB_DEPTH_INFO={:#x}",
        frame_number, cur_limit_step, color->image.width, color->image.height,
        depth ? "yes" : "NO", dc.value, uint32_t(dc.z_enable), uint32_t(dc.z_write_enable),
        uint32_t(dc.zfunc), uint32_t(dc.stencil_enable), uint32_t(dc.backface_enable), sm.value,
        uint32_t(sm.stencilref), uint32_t(sm.stencilmask), uint32_t(sm.stencilwritemask),
        regs[XE_GPU_REG_RB_DEPTH_INFO]);
  }
  static const bool debug_color = std::getenv("XN_DEBUG_COLORCONTROL") != nullptr;
  if (debug_color && cur_limit_step < 13) {
    // Alpha-to-coverage changes deposited light without touching coverage,
    // extent, inputs, expansion or geometry - the exact residue left after
    // everything else in this pass has been verified identical. With a median
    // vertex alpha of 0.00 in the fill pass, it is a brutal reduction if on.
    REXLOG_INFO("xn-vk: COLORCTL F{} step={} rt={}x{} samples={} RB_COLORCONTROL={:#x} "
                "a2m_enable={} alpha_test_enable={} func={} ref={:.4g} -> sc.alpha_to_mask={:#x}",
                frame_number, cur_limit_step, color->image.width, color->image.height,
                uint32_t(GuestSampleCount(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples)), rb_colorcontrol.value,
                uint32_t(rb_colorcontrol.alpha_to_mask_enable),
                uint32_t(rb_colorcontrol.alpha_test_enable),
                uint32_t(rb_colorcontrol.alpha_func), sc.alpha_test_reference, sc.alpha_to_mask);
  }

  // --- Constant buffers (register-file ranges, verbatim). ---
  uint32_t dynamic_offsets[SpirvShaderTranslator::kConstantBufferCount];
  auto upload = [&](uint32_t binding, const void* data, size_t size) __attribute__((always_inline)) -> bool {
    // Opt-in experiment: reduces upload traffic, but has not demonstrated a
    // CPU-time/FPS gain in the MCLA city A/B capture. Keep the proven default.
    static const bool reuse = [] {
      const char* value = std::getenv("XN_VK_CONSTANT_REUSE");
      return value && value[0] == '1';
    }();
    auto& prepared = dirty_constant_bindings[binding];
    const uint32_t mask = binding == SpirvShaderTranslator::kConstantBufferBoolLoop
        ? DrawPreparationDirtyState::kBoolLoop
        : binding == SpirvShaderTranslator::kConstantBufferFetch ? DrawPreparationDirtyState::kFetch : 0;
    const bool unchanged = mask ? !(dirty.pending & mask) : system_values_unchanged;
    if (use_dirty_preparation && unchanged && prepared.batch == constant_batch_epoch) {
      dynamic_offsets[binding] = prepared.offset;
      ++stats.constant_uploads_reused;
      ++stats.dirty_constant_uploads_reused;
      return true;
    }
    const auto remember_prepared = [&] {
      if (use_dirty_preparation) {
        prepared.batch = constant_batch_epoch;
        prepared.offset = dynamic_offsets[binding];
        dirty.pending &= ~mask;
      }
    };
    auto& cached = constant_upload_cache[binding];
    if (reuse && cached.Find(data, size, dynamic_offsets[binding])) {
      ++stats.constant_uploads_reused;
      remember_prepared();
      return true;
    }
    VkDeviceSize offset;
    uint8_t* dst = RingAllocate(constant_ring, ubo_sizes[binding], ubo_align, offset);
    if (!dst) return false;
    std::memcpy(dst, data, size);
    dynamic_offsets[binding] = uint32_t(offset);
    stats.constant_upload_bytes += size;
    if (reuse) cached.Remember(data, size, uint32_t(offset));
    remember_prepared();
    return true;
  };
  // Float constants are PACKED, not uploaded at their register indices: the
  // translator emits a buffer holding only the constants the shader actually
  // reads, densely, in ascending order, and indexes that. A pixel shader whose
  // ucode says `c255` reads element 0 of its buffer. Uploading all 256 at
  // their natural indices therefore feeds every such shader the wrong values -
  // Geometry Wars' tonemap shaders read c0 (a viewport centre like 640) where
  // they wanted c255 (0.75), so exp2(log2(x) * 640) saturated the whole frame
  // to white. (Fetch constants, by contrast, ARE absolute - the same shader
  // reads fetch slot 95 as element 47 - and bool/loop likewise.)
  auto upload_float_constants = [&](uint32_t binding, const Shader* shader,
                                    uint32_t register_base) __attribute__((always_inline)) -> bool {
    VkDeviceSize offset = 0;
    // The translator packs only the constants the shader reads. Reserving all
    // 256 float4s for both stages wasted most of the ring on small shaders.
    // The conservative preflight above still leaves room for each descriptor's
    // full declared range at the end of the buffer.
    const VkDeviceSize used = shader ? shader->constant_register_map().float_count * 16 : 0;
    const uint32_t stage = register_base == XE_GPU_REG_SHADER_CONSTANT_000_X ? 0 : 1;
    auto& prepared = dirty_constant_bindings[binding];
    bool unchanged = false;
    if (use_dirty_preparation && reuse_float_constants) {
      unchanged = true;
      if (shader) {
        const auto& bitmap = shader->constant_register_map().float_bitmap;
        for (unsigned i = 0; i < 4; ++i) unchanged &= !(dirty.float_dirty[stage][i] & bitmap[i]);
      }
    }
    if (use_dirty_preparation && reuse_float_constants && unchanged &&
        prepared.shader == shader && prepared.batch == constant_batch_epoch) {
      dynamic_offsets[binding] = prepared.offset;
      ++stats.constant_uploads_reused;
      ++stats.float_constant_uploads_reused;
      stats.float_constant_bytes_avoided += used;
      ++stats.float_constant_packs_avoided;
      return true;
    }
    const auto remember_prepared = [&] {
      if (use_dirty_preparation) {
        prepared = {shader, constant_batch_epoch, dynamic_offsets[binding]};
        dirty.float_dirty[stage].fill(0);
      }
    };
    std::array<uint8_t, 4096> packed;
    uint8_t* dst = reuse_float_constants ? packed.data() :
        RingAllocate(constant_ring, std::max<VkDeviceSize>(16, used), ubo_align, offset);
    if (!dst) return false;
    size_t written = 0;
    if (shader) {
      const Shader::ConstantRegisterMap& map = shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t entry = map.float_bitmap[i];
        const uint32_t* base = &regs[register_base + (i << 8)];
        while (entry) {
          const uint32_t index = rex::tzcnt(entry);
          const uint32_t run = rex::tzcnt(~(entry >> index));
          const size_t bytes = size_t(run) * 4 * sizeof(float);
          if (written + bytes > size_t(ubo_sizes[binding])) break;
          std::memcpy(dst + written, base + (index << 2), bytes);
          written += bytes;
          const uint64_t run_mask =
              run >= 64 ? ~UINT64_C(0) : ((UINT64_C(1) << run) - 1);
          entry &= ~(run_mask << index);
        }
      }
    }
    if (reuse_float_constants) {
      auto& cached = constant_upload_cache[binding];
      if (written == used && cached.Find(packed.data(), written, dynamic_offsets[binding])) {
        ++stats.constant_uploads_reused;
        ++stats.float_constant_uploads_reused;
        stats.float_constant_bytes_avoided += written;
        remember_prepared();
        return true;
      }
      dst = RingAllocate(constant_ring, std::max<VkDeviceSize>(16, used), ubo_align, offset);
      if (!dst) return false;
      std::memcpy(dst, packed.data(), written);
      if (written == used) cached.Remember(packed.data(), written, uint32_t(offset));
    }
    dynamic_offsets[binding] = uint32_t(offset);
    stats.constant_upload_bytes += written;
    remember_prepared();
    return true;
  };
  if (!upload(SpirvShaderTranslator::kConstantBufferSystem, &sc, sizeof(sc)) ||
      !upload_float_constants(SpirvShaderTranslator::kConstantBufferFloatVertex, vshader,
                              XE_GPU_REG_SHADER_CONSTANT_000_X) ||
      !upload_float_constants(SpirvShaderTranslator::kConstantBufferFloatPixel, pshader,
                              XE_GPU_REG_SHADER_CONSTANT_256_X) ||
      !upload(SpirvShaderTranslator::kConstantBufferBoolLoop,
              &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 40 * 4) ||
      !upload(SpirvShaderTranslator::kConstantBufferFetch,
              &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 32 * 6 * 4)) {
    return skip("constant_ring_full");
  }

  // --- Pipeline. ---
  const auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  PipelineKey uncached_key;
  auto& key = use_dirty_preparation ? prepared_pipeline.key : uncached_key;
  bool pipeline_preparation_hit = use_dirty_preparation && prepared_pipeline.valid &&
      !(dirty.pending & DrawPreparationDirtyState::kPipeline) &&
      prepared_pipeline.vs_shader == vshader && prepared_pipeline.ps_shader == pshader &&
      prepared_pipeline.polygonal == primitive_polygonal &&
      key.color_write_mask == (no_mrt ? (normalized_color_mask & 0xF) : normalized_color_mask) &&
      key.vs == vs_module && key.ps == ps_module && key.depth_format == depth->image.format &&
      key.topology == uint32_t(topology) && key.primitive_restart == uint32_t(primitive_restart) &&
      key.color_attachment_count == color_count && key.samples == uint32_t(color->samples);
  for (uint32_t i = 0; pipeline_preparation_hit && i < color_count; ++i)
    pipeline_preparation_hit = key.color_formats[i] ==
        (colors[i] ? colors[i]->image.format : VK_FORMAT_UNDEFINED);
  if (!pipeline_preparation_hit) {
    if (use_dirty_preparation) prepared_pipeline.valid = false;
    key = {};
    key.vs = vs_module;
    key.ps = ps_module;
    for (uint32_t i = 0; i < color_count; ++i)
      key.color_formats[i] = colors[i] ? colors[i]->image.format : VK_FORMAT_UNDEFINED;
    key.depth_format = depth->image.format;
    key.topology = uint32_t(topology);
  }
  // XN_DEBUG_DUMP_VB=<dir>: dump the whole fetched vertex buffer of every
  // 4096-vertex quad-list draw. Every colour comparison so far has read only
  // the FIRST quad of a 1024-quad batch and assumed it is the one covering the
  // pixel being measured; this makes that checkable instead of assumed.
  static const char* dump_vb = std::getenv("XN_DEBUG_DUMP_VB");
  if (const char* vbdir = dump_vb) {
    if (uint32_t(prim_type) == 13 && (p.index_count == 4096 || p.index_count > 10000) && vshader &&
        !vshader->vertex_bindings().empty() && guest_base) {
      static uint32_t vb_seq = 0;
      const Shader::VertexBinding& vb = vshader->vertex_bindings()[0];
      xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(vb.fetch_constant);
      const uint32_t addr = vf.address << 2;
      const uint32_t size = uint32_t(vf.size) << 2;
      if (uint64_t(addr) + size <= uint64_t(guest_size)) {
        const std::string path = fmt::format("{}/vb_{}_{:08X}_{}_stride{}.bin", vbdir, vb_seq++,
                                             addr, size, vb.stride_words);
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
          std::fwrite(guest_base + addr, 1, size, f);
          std::fclose(f);
        }
      }
    }
  }
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (pipeline_preparation_hit) {
    layout = prepared_pipeline.layout;
    pipeline = prepared_pipeline.pipeline;
    ++stats.preparation_pipeline_hits;
  } else {
    // ONLY the bound attachments' blend state belongs in the key. Including the
    // unused slots puts registers the pipeline never reads into it, so every
    // change to RB_BLENDCONTROL1..3 mints another pipeline for a single-target
    // draw - thousands of them on a title that touches those registers, which
    // costs a compile each and stalls hardest exactly when a level loads.
    // XN_VK_KEY_ALL_BLEND=1 keeps the unused slots in the key, to measure what
    // including them costs.
    static const bool key_all_blend = std::getenv("XN_VK_KEY_ALL_BLEND") != nullptr;
    const uint32_t blend_slots = key_all_blend ? xenos::kMaxColorRenderTargets : color_count;
    for (uint32_t i = 0; i < blend_slots; ++i) {
      key.blend_controls[i] =
          regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]).value &
          0x1FFF1FFFu;
    }
    key.color_write_mask = no_mrt ? (normalized_color_mask & 0xF) : normalized_color_mask;
    // Xenos non-polygon primitives use the front stencil operations on both
    // sides. Expanded rectangles may have either Vulkan winding. Letting a
    // stale back-face ALWAYS test apply made Forza's cockpit depth mask write
    // near depth across its windows and reject the scenery drawn afterward.
    auto host_depth_control = depth_control;
    if (!primitive_polygonal) host_depth_control.backface_enable = 0;
    key.depth_control = host_depth_control.value;
    // Launch-time diagnostic mask isolates depth/stencil rejection while
    // retaining the real scene shaders. The ordinary mask leaves every bit.
    key.depth_control &= launch_diagnostics.depth_mask;
    // clip_disable also disables near/far clipping. SDK partial-tile clears
    // deliberately use a depth just below zero, which must reach the rasterizer.
    key.depth_clamp = features.depthClamp && pa_cl_clip_cntl.clip_disable;
    key.primitive_restart = primitive_restart;
    key.stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>().value & 0x00FFFF00u;
    key.color_attachment_count = color_count;
    key.samples = uint32_t(color->samples);
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.cull_front) key.cull_mode |= VK_CULL_MODE_FRONT_BIT;
      if (pa_su_sc_mode_cntl.cull_back) key.cull_mode |= VK_CULL_MODE_BACK_BIT;
    }
    key.front_face = pa_su_sc_mode_cntl.face ? VK_FRONT_FACE_CLOCKWISE
                                             : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    const auto& vs_tex = vshader->GetTextureBindingsAfterTranslation();
    const auto& vs_samp = vshader->GetSamplerBindingsAfterTranslation();
    const uint32_t ps_tex = pshader ? uint32_t(pshader->GetTextureBindingsAfterTranslation().size()) : 0;
    const uint32_t ps_samp = pshader ? uint32_t(pshader->GetSamplerBindingsAfterTranslation().size()) : 0;
    layout =
        GetPipelineLayout(uint32_t(vs_tex.size()), uint32_t(vs_samp.size()), ps_tex, ps_samp);

    pipeline = GetPipeline(key, layout);
    if (!pipeline) return skip("pipeline");
    if (use_dirty_preparation) {
      prepared_pipeline.valid = true;
      prepared_pipeline.vs_shader = vshader;
      prepared_pipeline.ps_shader = pshader;
      prepared_pipeline.polygonal = primitive_polygonal;
      prepared_pipeline.layout = layout;
      prepared_pipeline.pipeline = pipeline;
      dirty.pending &= ~DrawPreparationDirtyState::kPipeline;
    }
  }

  uint32_t ps_param_gen_pos_dbg = UINT32_MAX;
  if (debug_frame_active) {
    const uint32_t restart_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    uint32_t restart_matches = 0;
    if (p.indexed && pa_su_sc_mode_cntl.multi_prim_ib_ena) {
      for (uint32_t i = 0; i < p.index_count; ++i)
        restart_matches += (src.Read(i) & xenos::kVertexIndexMask) == restart_index;
    }
    REXLOG_INFO("xn-vk: F{} d{} assembly indexed={} restart={} index={:#x} matches={} "
                "su_sc={:#x} color_mask={:#x} rt1={:#x} rt2={:#x} rt3={:#x}",
                frame_number, draw_number, p.indexed,
                uint32_t(pa_su_sc_mode_cntl.multi_prim_ib_ena), restart_index, restart_matches,
                pa_su_sc_mode_cntl.value, regs[XE_GPU_REG_RB_COLOR_MASK],
                regs[XE_GPU_REG_RB_COLOR1_INFO], regs[XE_GPU_REG_RB_COLOR2_INFO],
                regs[XE_GPU_REG_RB_COLOR3_INFO]);
    if (std::getenv("XN_DEBUG_PROBE_DEPTH_AT")) {
      std::string constants;
      for (uint32_t i = 0; i < 10; ++i) {
        const uint32_t r = XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4;
        constants += fmt::format(" c{}=({:.9g},{:.9g},{:.9g},{:.9g})", i,
            regs.Get<float>(r), regs.Get<float>(r + 1),
            regs.Get<float>(r + 2), regs.Get<float>(r + 3));
      }
      REXLOG_INFO("xn-vk: VSDEPTH d{} vs={:016X} host_z={}..{} ndc_z={},{} guest_z={},{}{}",
          draw_number, vshader->ucode_data_hash(), viewport.minDepth, viewport.maxDepth,
          viewport_info.ndc_scale[2], viewport_info.ndc_offset[2],
          regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
          regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET), constants);
    }
    // PIXEL shader constants. The `c0=`/`c1=` in the line below read
    // SHADER_CONSTANT_000, which is the VERTEX constant space - pixel constants
    // start at SHADER_CONSTANT_256, which is why psc255 is addressed from that
    // base. Anyone reading a pixel shader's disassembly and reaching for the
    // `c0=` field is reading a different shader's constants; this is the one to
    // use. (Cost us a round on the second title's composite, which ends in
    // exp(log(x) * c2.x) - a power curve whose exponent is pixel c2.)
    {
      auto psc = [&](uint32_t i, uint32_t c) {
        return regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4 + c);
      };
      REXLOG_INFO("xn-vk: F{} d{} pix_center={} vtx_cntl={:#x} prim={}",
                  frame_number, draw_number,
                  uint32_t(regs.Get<reg::PA_SU_VTX_CNTL>().pix_center),
                  regs[XE_GPU_REG_PA_SU_VTX_CNTL], uint32_t(prim_type));
      REXLOG_INFO("xn-vk: F{} d{} ps={:016X} PIXELCONST c0=({:.5f},{:.5f},{:.5f},{:.5f}) "
                  "c1=({:.5f},{:.5f},{:.5f},{:.5f}) c2=({:.5f},{:.5f},{:.5f},{:.5f}) "
                  "c3=({:.5f},{:.5f},{:.5f},{:.5f})",
                  frame_number, draw_number,
                  pshader ? pshader->ucode_data_hash() : 0ull,
                  psc(0, 0), psc(0, 1), psc(0, 2), psc(0, 3), psc(1, 0), psc(1, 1), psc(1, 2),
                  psc(1, 3), psc(2, 0), psc(2, 1), psc(2, 2), psc(2, 3), psc(3, 0), psc(3, 1),
                  psc(3, 2), psc(3, 3));
    }
    REXLOG_INFO(
        "xn-vk: F{} d{} prim={} n={} vs={:016X} ps={:016X} rt#{} base={:#x}:{}x{} fmt={} blend={:#010x} "
        "mask={:#x} depth={:#x} vp=({},{} {}x{}) sc=({},{} {}x{}) mode={} exp={}{}",
        frame_number, draw_number, uint32_t(prim_type), draw_count,
        vshader->ucode_data_hash(), pshader ? pshader->ucode_data_hash() : 0ull,
        color->id, RenderTargetBase(rb_color_info.value), color->image.width, color->image.height,
        uint32_t(rb_color_info.color_format), key.blend_controls[0], key.color_write_mask,
        key.depth_control, viewport.x, viewport.y, viewport.width, viewport.height,
        scissor_rect.offset.x, scissor_rect.offset.y, scissor_rect.extent.width,
        scissor_rect.extent.height, uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode),
        sc.color_exp_bias[0],
        fmt::format(" psc255=({:.3f},{:.3f},{:.3f},{:.3f})"
                    " interp={:#x}(vs={:#x} ps_in={:#x} pgen={}) prog={:#x} ctxmisc={:#x}"
                    " colorctl={:#x} vte={:#x} c0=({},{},{},{}) c1=({},{},{},{}) "
                    "sysflags={:#x} vfetch0={:#010x},{:#010x}{}",
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + 255 * 4 + 0),
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + 255 * 4 + 1),
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + 255 * 4 + 2),
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + 255 * 4 + 3),
                    interpolator_mask, vshader->writes_interpolators(),
                    pshader ? pshader->GetInterpolatorInputMask(sq_program_cntl, sq_context_misc,
                                                                ps_param_gen_pos_dbg)
                            : 0u,
                    ps_param_gen_pos, regs[XE_GPU_REG_SQ_PROGRAM_CNTL],
                    regs[XE_GPU_REG_SQ_CONTEXT_MISC],
                    regs[XE_GPU_REG_RB_COLORCONTROL], regs[XE_GPU_REG_PA_CL_VTE_CNTL],
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 0),
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 1),
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 2),
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 3),
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 4),
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 5),
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 6),
                    regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 7), sc.flags,
                    regs.GetVertexFetch(vshader->vertex_bindings().empty()
                                            ? 0
                                            : vshader->vertex_bindings()[0].fetch_constant)
                        .dword_0,
                    regs.GetVertexFetch(vshader->vertex_bindings().empty()
                                            ? 0
                                            : vshader->vertex_bindings()[0].fetch_constant)
                        .dword_1,
                    last_texture_log));
  }
  // XN_DEBUG_COMPOSITE_CONSTANTS=1: for the tonemap composites, print the float
  // constants BOTH as the guest numbers them and as the packed buffer the draw
  // actually receives them in. The translator packs the float bank densely, so
  // a shader asking for c255 does not receive guest register 255 - it receives
  // whatever element c255 was packed into. Reading the declared index instead
  // of the delivered one is exactly how a shader's apparent arithmetic stops
  // matching its output.
  static const bool debug_composite = std::getenv("XN_DEBUG_COMPOSITE_CONSTANTS") != nullptr;
  if (pshader && debug_composite) {
    const uint64_t ps_hash = pshader->ucode_data_hash();
    if (ps_hash == 0x5C4BCB6B0AC7678Eull || ps_hash == 0xEC8FDC749974EDBAull) {
      static std::set<uint64_t> seen_cc;
      if (seen_cc.insert(ps_hash).second) {
        std::string cs;
        uint32_t packed_index = 0;
        const Shader::ConstantRegisterMap& cm = pshader->constant_register_map();
        for (uint32_t i = 0; i < 4; ++i) {
          uint64_t e = cm.float_bitmap[i];
          while (e) {
            const uint32_t b = rex::tzcnt(e);
            e &= e - 1;
            const uint32_t guest = (i << 6) + b;
            const uint32_t r = XE_GPU_REG_SHADER_CONSTANT_256_X + guest * 4;
            cs += fmt::format(" [packed{} = guest c{} = ({:.6g},{:.6g},{:.6g},{:.6g})]",
                              packed_index++, guest, regs.Get<float>(r), regs.Get<float>(r + 1),
                              regs.Get<float>(r + 2), regs.Get<float>(r + 3));
          }
        }
        REXLOG_INFO("xn-vk: COMPOSITECONST ps={:016X} reads {} constant(s):{}", ps_hash,
                    packed_index, cs.empty() ? " (none)" : cs);
      }
    }
  }
  // XN_DEBUG_BR_BLUR=1: Burnout Revenge's post chain. The 13-tap blur
  // (ps 6B97F1EC888F4E4D) takes its taps as base UV + VERTEX constants c50..c61,
  // so an over-wide smear shows up here directly as offsets far larger than a
  // texel; its weights are ps c52..c63 scaled by c50.z. The composite
  // (ps FB3A19D15B918C21) grades through a 3D LUT in tf2. Also prints the bound
  // textures and the colour target, because a source that is the target is a
  // feedback loop and smears without any constant being wrong.
  static const bool debug_br_blur = std::getenv("XN_DEBUG_BR_BLUR") != nullptr;
  if (pshader && debug_br_blur) {
    const uint64_t ps_hash = pshader->ucode_data_hash();
    if (ps_hash == 0x6B97F1EC888F4E4Dull || ps_hash == 0xFB3A19D15B918C21ull) {
      static uint32_t br_blur_shown = 0;
      if (br_blur_shown < 8) {
        ++br_blur_shown;
        auto bank = [&](uint32_t base, uint32_t first, uint32_t last) {
          std::string out;
          for (uint32_t i = first; i <= last; ++i) {
            const uint32_t r = base + i * 4;
            out += fmt::format(" c{}=({:.6g},{:.6g},{:.6g},{:.6g})", i, regs.Get<float>(r),
                               regs.Get<float>(r + 1), regs.Get<float>(r + 2),
                               regs.Get<float>(r + 3));
          }
          return out;
        };
        std::string tex_dump;
        for (uint32_t t = 0; t < 8; ++t) {
          xenos::xe_gpu_texture_fetch_t f = regs.GetTextureFetch(t);
          if (!f.base_address) continue;
          // How the surface was obtained matters more than its size: a fetch
          // the cache cannot satisfy is bound to a dummy, and this shader
          // reads mid-grey as "no motion", so a dummy reads as full motion.
          const char* kind = "dummy";
          if (f.type == xenos::FetchConstantType::kTexture) {
            if (TextureEntry* e = GetTexture(f)) {
              kind = e->unsupported ? "UNSUPPORTED" : (e->resolved ? "resolved" : "guest");
            } else {
              kind = "NO-ENTRY";
            }
          }
          tex_dump += fmt::format(" tf{}={:#x}({}x{} fmt={} type={} {})", t,
                                  uint32_t(f.base_address) << 12,
                                  uint32_t(f.size_2d.width) + 1, uint32_t(f.size_2d.height) + 1,
                                  uint32_t(f.format), uint32_t(f.type), kind);
        }
        // Where do the real values live? Print every non-zero constant in both
        // banks: if the tap offsets were written somewhere other than the
        // registers this shader reads, they show up here as small UV numbers.
        auto nonzero = [&](uint32_t base, const char* tag) {
          std::string out = fmt::format(" {}nz:", tag);
          uint32_t shown = 0;
          for (uint32_t i = 0; i < 256 && shown < 40; ++i) {
            const uint32_t r = base + i * 4;
            const float x = regs.Get<float>(r), y = regs.Get<float>(r + 1);
            const float z = regs.Get<float>(r + 2), w = regs.Get<float>(r + 3);
            if (x == 0.f && y == 0.f && z == 0.f && w == 0.f) continue;
            out += fmt::format(" c{}=({:.5g},{:.5g},{:.5g},{:.5g})", i, x, y, z, w);
            ++shown;
          }
          return out;
        };
        REXLOG_INFO("xn-vk: BRBLURNZ F{} d{} ps={:016X}{}{}", frame_number, draw_number, ps_hash,
                    nonzero(XE_GPU_REG_SHADER_CONSTANT_000_X, "vs"),
                    nonzero(XE_GPU_REG_SHADER_CONSTANT_256_X, "ps"));
        REXLOG_INFO("xn-vk: BRBLUR F{} d{} ps={:016X} vs={:016X} rt_base={:#x} {}x{} fmt={}"
                    " taps(vs){} weights(ps){}{}",
                    frame_number, draw_number, ps_hash, vshader->ucode_data_hash(),
                    RenderTargetBase(rb_color_info.value), color->image.width, color->image.height,
                    uint32_t(rb_color_info.color_format),
                    bank(XE_GPU_REG_SHADER_CONSTANT_000_X, 50, 61),
                    bank(XE_GPU_REG_SHADER_CONSTANT_256_X, 50, 53) +
                        // The composite decodes its blur step from tf1 using
                        // c253..c255 as bias and scale; a zero bias leaves a
                        // constant offset on every pixel, which is a uniform
                        // smear rather than a per-pixel one.
                        bank(XE_GPU_REG_SHADER_CONSTANT_256_X, 253, 255),
                    tex_dump);
      }
    }
  }
  if (draw_number < 6) {
    std::string vf_dump;
    for (const Shader::VertexBinding& vb : vshader->vertex_bindings()) {
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(vb.fetch_constant);
      vf_dump += fmt::format(" vf{}=[{:#010x},{:#010x}]", vb.fetch_constant, fetch.dword_0,
                             fetch.dword_1);
    }
    REXLOG_DEBUG("xn-vk: draw #{} regs: VTE={:#x} CLIP={:#x} SU_SC={:#x} PROG={:#x} CTXMISC={:#x} "
                 "COLORCTL={:#x} MODE={:#x} OUTPATH={:#x} WIN_OFF={:#x} WIN_TL={:#x} WIN_BR={:#x} "
                 "SCR_TL={:#x} SCR_BR={:#x} SU_VTX={:#x} AA={:#x} LINE={:#x} MINIDX={:#x} "
                 "MAXIDX={:#x} IDXOFF={:#x} SURF={:#x} CINFO={:#x} DINFO={:#x} VP=[{},{},{},{},{},{}]{}",
                 draw_number, regs[XE_GPU_REG_PA_CL_VTE_CNTL], regs[XE_GPU_REG_PA_CL_CLIP_CNTL],
                 regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL], regs[XE_GPU_REG_SQ_PROGRAM_CNTL],
                 regs[XE_GPU_REG_SQ_CONTEXT_MISC], regs[XE_GPU_REG_RB_COLORCONTROL],
                 regs[XE_GPU_REG_RB_MODECONTROL], regs[XE_GPU_REG_VGT_OUTPUT_PATH_CNTL],
                 regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
                 regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR], regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL],
                 regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR], regs[XE_GPU_REG_PA_SU_VTX_CNTL],
                 regs[XE_GPU_REG_PA_SC_AA_CONFIG], regs[XE_GPU_REG_PA_SC_LINE_CNTL],
                 regs[XE_GPU_REG_VGT_MIN_VTX_INDX], regs[XE_GPU_REG_VGT_MAX_VTX_INDX],
                 regs[XE_GPU_REG_VGT_INDX_OFFSET], regs[XE_GPU_REG_RB_SURFACE_INFO],
                 regs[XE_GPU_REG_RB_COLOR_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO],
                 regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE),
                 regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET),
                 regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE),
                 regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET),
                 regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
                 regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET), vf_dump);
  }
  if (draw_number < 32) {
    REXLOG_DEBUG("xn-vk: draw #{} prim={} count={} indexed={} vs={:016X} ps={:016X} rt={}x{} "
                 "vp=({},{} {}x{} z {}..{}) ndc_scale=({},{},{}) ndc_offset=({},{},{}) "
                 "scissor=({},{} {}x{}) blend={:#x} mask={:#x} depth={:#x} cull={}",
                 draw_number, uint32_t(prim_type), draw_count, indexed, vshader->ucode_data_hash(),
                 pshader ? pshader->ucode_data_hash() : 0ull, color->image.width,
                 color->image.height, viewport.x, viewport.y, viewport.width, viewport.height,
                 viewport.minDepth, viewport.maxDepth, sc.ndc_scale[0], sc.ndc_scale[1],
                 sc.ndc_scale[2], sc.ndc_offset[0], sc.ndc_offset[1], sc.ndc_offset[2],
                 scissor_rect.offset.x, scissor_rect.offset.y, scissor_rect.extent.width,
                 scissor_rect.extent.height, key.blend_controls[0], key.color_write_mask,
                 key.depth_control, key.cull_mode);
  }

  // A normal draw can replace some samples in a formerly uniform clear tile.
  // Forget the initialisation hint; other views must not resurrect an old clear.
  if (!sdk_depth_clear && (depth_control.z_write_enable ||
      (depth_control.stencil_enable && regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask)))
    ForgetDepthClearTiles(*depth);
  if (!sdk_depth_clear) {
    for (uint32_t i = 0; i < color_count; ++i)
      if (colors[i] && ((key.color_write_mask >> (i * 4)) & 0xF)) ForgetDepthClearTiles(*colors[i]);
  }

  // --- Record. ---
  EnsureRendering(colors, color_count, depth);
  float depth_bias_slope, depth_bias_offset;
  draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal,
                                            depth_bias_slope, depth_bias_offset);
  depth_bias_slope *= xenos::kPolygonOffsetScaleSubpixelUnit;
  depth_bias_offset *= depth->image.format == VK_FORMAT_D24_UNORM_S8_UINT
      ? draw_util::kD3D10PolygonOffsetFactorUnorm24
      : draw_util::kD3D10PolygonOffsetFactorFloat24;
  const std::array<float, 3> depth_bias{depth_bias_offset, 0.0f, depth_bias_slope};
  float blend_constants[4];
  for (uint32_t i = 0; i < 4; ++i)
    blend_constants[i] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED + i);
  const uint32_t stencil_reference = regs.Get<reg::RB_STENCILREFMASK>().stencilref;

  if (profile_draw_batches || (profile_instance_batches && cache_resident_indices)) {
    DrawBatchKey batch_key;
    batch_key.pipeline = uint64_t(uintptr_t(pipeline));
    batch_key.layout = uint64_t(uintptr_t(layout));
    batch_key.vs_textures = uint64_t(uintptr_t(vs_textures));
    batch_key.ps_textures = uint64_t(uintptr_t(ps_textures));
    batch_key.index_buffer = indexed ? uint64_t(uintptr_t(index_buffer)) : 0;
    batch_key.index_type = indexed ? uint32_t(index_type) : 0;
    batch_key.indexed = indexed;
    batch_key.render_epoch = draw_batch_epoch;
    std::memcpy(batch_key.viewport_bits.data(), &viewport, sizeof(viewport));
    std::memcpy(batch_key.scissor.data(), &scissor_rect, sizeof(scissor_rect));
    std::memcpy(batch_key.depth_bias.data(), depth_bias.data(), sizeof(depth_bias));
    std::memcpy(batch_key.blend.data(), blend_constants, sizeof(blend_constants));
    batch_key.stencil_ref = stencil_reference;
    static_assert(sizeof(dynamic_offsets) == sizeof(batch_key.constant_offsets));
    std::memcpy(batch_key.constant_offsets.data(), dynamic_offsets, sizeof(dynamic_offsets));
    if (profile_draw_batches) draw_batch_profile.Observe(batch_key);
    if (profile_instance_batches && cache_resident_indices) {
      // Each future instance would select its own constants and resident
      // index address. Count only adjacent draws with identical host state
      // and index count; the existing strict profile compares this slot.
      batch_key.constant_offsets = {};
      batch_key.constant_offsets[0] = draw_count;
      const bool eligible = resident_indices && indexed &&
          topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && !primitive_restart &&
          !viz_query.viz_query_ena && !debug_frame_active &&
          !vshader->memexport_eM_written() &&
          !(pshader && pshader->memexport_eM_written());
      instance_batch_profile.Observe(batch_key, eligible);
    }
  }

  VkDeviceSize record_offset = 0;
  uint32_t record_index = 0;
  const uint32_t first_index = indexed ? uint32_t(index_offset /
      (index_type == VK_INDEX_TYPE_UINT32 ? 4 : 2)) : 0;
  if (gpu_draw_constants) {
    auto* destination = RingAllocate(draw_record_ring, sizeof(NativeDrawRecord), 64, record_offset);
    if (!destination) return skip("draw_record_ring_full");
    record_index = uint32_t(record_offset / sizeof(NativeDrawRecord));
    NativeDrawRecord record{};
    for (unsigned i = 0; i < 5; ++i)
      record.addresses[i] = constant_ring.buf.address + dynamic_offsets[i];
    if (indexed) {
      const VkDrawIndexedIndirectCommand command{draw_count, 1, first_index, 0, record_index};
      std::memcpy(record.command, &command, sizeof(command));
    } else {
      const VkDrawIndirectCommand command{draw_count, 1, 0, record_index};
      std::memcpy(record.command, &command, sizeof(command));
    }
    std::memcpy(destination, &record, sizeof(record));
    ++stats.gpu_constant_draws;
  }
  const bool batch_eligible = multi_draw && !debug_frame_active && !viz_query.viz_query_ena &&
      !vshader->memexport_eM_written() && !(pshader && pshader->memexport_eM_written());
  auto& recorded = draw_command_state;
  const bool compatible_pipeline = recorded.valid && recorded.pipeline == pipeline && recorded.layout == layout;
  const bool equal_viewport = compatible_pipeline && !std::memcmp(&recorded.viewport, &viewport, sizeof(viewport));
  const bool equal_scissor = compatible_pipeline && !std::memcmp(&recorded.scissor, &scissor_rect, sizeof(scissor_rect));
  const bool equal_bias = compatible_pipeline && !std::memcmp(recorded.depth_bias.data(), depth_bias.data(), sizeof(depth_bias));
  const bool equal_blend = compatible_pipeline && !std::memcmp(recorded.blend_constants.data(), blend_constants, sizeof(blend_constants));
  const bool equal_stencil = compatible_pipeline && recorded.stencil_reference == stencil_reference;
  if (pending_native_draw_count && (!batch_eligible || !compatible_pipeline ||
      recorded.vs_textures != vs_textures || recorded.ps_textures != ps_textures ||
      !equal_viewport || !equal_scissor || !equal_bias || !equal_blend || !equal_stencil ||
      indexed != pending_native_indexed || (indexed && index_type != pending_native_index_type) ||
      record_offset != pending_native_draw_offset + pending_native_draw_count * sizeof(NativeDrawRecord) ||
      pending_native_draw_count >= std::min(256u, properties.limits.maxDrawIndirectCount)))
    FlushNativeDraws();
  const bool same_state = cache_draw_state && compatible_pipeline;
  if (!same_state) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  else { ++stats.draw_state_commands_saved; ++stats.draw_state_pipeline_binds_saved; }
  VkDescriptorSet fixed_sets[2] = {set_shared_memory, set_constants};
  if (!gpu_draw_constants || !same_state)
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, fixed_sets,
        gpu_draw_constants ? 0 : SpirvShaderTranslator::kConstantBufferCount,
        gpu_draw_constants ? nullptr : dynamic_offsets);
  else ++stats.draw_state_commands_saved;
  if (vs_textures) {
    if (!same_state || recorded.vs_textures != vs_textures)
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            SpirvShaderTranslator::kDescriptorSetTexturesVertex, 1, &vs_textures, 0, nullptr);
    else { ++stats.draw_state_commands_saved; ++stats.draw_state_texture_binds_saved; }
  }
  if (ps_textures) {
    if (!same_state || recorded.ps_textures != ps_textures)
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            SpirvShaderTranslator::kDescriptorSetTexturesPixel, 1, &ps_textures, 0, nullptr);
    else { ++stats.draw_state_commands_saved; ++stats.draw_state_texture_binds_saved; }
  }
  if (!cache_draw_state || !equal_viewport)
    vkCmdSetViewport(cb, 0, 1, &viewport);
  else ++stats.draw_state_commands_saved;
  if (!cache_draw_state || !equal_scissor)
    vkCmdSetScissor(cb, 0, 1, &scissor_rect);
  else ++stats.draw_state_commands_saved;
  if (!cache_draw_state || !equal_bias)
    vkCmdSetDepthBias(cb, depth_bias_offset, 0.0f, depth_bias_slope);
  else ++stats.draw_state_commands_saved;
  if (debug_frame_active && (depth_bias_offset || depth_bias_slope))
    REXLOG_INFO("xn-vk: F{} d{} depth bias offset={} slope={} host_format={}",
                frame_number, draw_number, depth_bias_offset, depth_bias_slope, uint32_t(depth->image.format));
  if (!cache_draw_state || !equal_blend)
    vkCmdSetBlendConstants(cb, blend_constants);
  else ++stats.draw_state_commands_saved;
  if (!cache_draw_state || !equal_stencil)
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_reference);
  else ++stats.draw_state_commands_saved;

  if (auto* profile = WorkProfile()) profile->AddDraw(render_work_token, draw_count);
  if (batch_eligible) {
    if (!pending_native_draw_count) {
      if (indexed) vkCmdBindIndexBuffer(cb, index_buffer, 0, index_type);
      pending_native_index_type = index_type;
      pending_native_draw_offset = record_offset;
      pending_native_indexed = indexed;
      pending_native_first = {draw_count, 1, first_index, 0, record_index};
    }
    ++pending_native_draw_count;
  } else if (indexed) {
    vkCmdBindIndexBuffer(cb, index_buffer, gpu_draw_constants ? 0 : index_offset, index_type);
    if (resident_indices) resident_indices.MarkUsed(frame_number + 1);
    vkCmdDrawIndexed(cb, draw_count, 1, gpu_draw_constants ? first_index : 0, 0, record_index);
  } else vkCmdDraw(cb, draw_count, 1, 0, record_index);

  if (cache_draw_state || multi_draw) {
    recorded.valid = true;
    recorded.pipeline = pipeline;
    recorded.layout = layout;
    recorded.vs_textures = vs_textures;
    recorded.ps_textures = ps_textures;
    recorded.viewport = viewport;
    recorded.scissor = scissor_rect;
    recorded.depth_bias = depth_bias;
    std::memcpy(recorded.blend_constants.data(), blend_constants, sizeof(blend_constants));
    recorded.stencil_reference = stencil_reference;
  }

  if (debug_frame_active && std::getenv("XN_DEBUG_PROBE_DEPTH") && depth) {
    int depth_probe_x = 640, depth_probe_y = 450;
    const char* depth_probe_at = std::getenv("XN_DEBUG_PROBE_DEPTH_AT");
    const bool custom_depth_probe = depth_probe_at &&
        std::sscanf(depth_probe_at, "%d,%d", &depth_probe_x, &depth_probe_y) == 2;
    if ((custom_depth_probe || (depth->edram_base == 0x400 && depth->image.width == 1280 &&
                               depth->samples == VK_SAMPLE_COUNT_2_BIT)) &&
        depth_probe_x >= 0 && depth_probe_y >= 0 &&
        uint32_t(depth_probe_x) < depth->image.width &&
        uint32_t(depth_probe_y) < std::min(depth->image.height, scissor_rect.extent.height)) {
      const VkRect2D point{{depth_probe_x, depth_probe_y}, {1, 1}};
      std::vector<uint8_t> rgba;
      std::vector<float> values;
      if (ReadbackImage(ReadableImage(*depth), rgba, &point, &values) && !values.empty()) {
        static float previous = -1.0f;
        if (custom_depth_probe || values[0] != previous) {
          REXLOG_INFO("xn-vk: DEPTHCHANGE swap={} d{} {:.10g}->{:.10g} vs={:016X} ps={:016X} "
                      "prim={} count={} dc={:#x} mask={:#x} bias={},{} base={:#x} rt={}x{} at={},{}",
                      guest_swap_number, draw_number, previous, values[0],
                      vshader->ucode_data_hash(), pshader ? pshader->ucode_data_hash() : 0ull,
                      uint32_t(prim_type), draw_count, key.depth_control, key.color_write_mask,
                      depth_bias_offset, depth_bias_slope, depth->edram_base,
                      depth->image.width, depth->image.height, depth_probe_x, depth_probe_y);
        }
        previous = values[0];
      }
      BeginFrameIfNeeded();
      debug_frame_active = true;
    }
  }

  static const std::vector<uint64_t> probe_draw_ps = [] {
    std::vector<uint64_t> shaders;
    const char* value = std::getenv("XN_DEBUG_PROBE_DRAW_PS");
    while (value && *value) {
      char* end = nullptr;
      const uint64_t hash = std::strtoull(value, &end, 16);
      if (end == value) break;
      if (hash) shaders.push_back(hash);
      value = *end == ',' ? end + 1 : nullptr;
    }
    return shaders;
  }();
  if (debug_frame_active && std::getenv("XN_DEBUG_PROBE_DRAW") &&
      (!std::getenv("XN_DEBUG_PROBE_WORLD_ONLY") ||
       (color->image.width >= 1280 && color->image.height >= 400)) &&
      (probe_draw_ps.empty() || (pshader &&
       std::find(probe_draw_ps.begin(), probe_draw_ps.end(), pshader->ucode_data_hash()) !=
           probe_draw_ps.end())) &&
      (!std::getenv("XN_DEBUG_PROBE_SMALL_DRAWS") ||
       (p.index_count <= 12 && color->image.width >= 1280 && key.color_write_mask))) {
    // Read the target back after this draw: shows exactly which draw stops
    // contributing. The readback closes the frame; the next draw re-opens the
    // render pass with loadOp=LOAD, so nothing is lost.
    std::vector<uint8_t> px_;
    uint8_t m = 0;
    double sum = 0;
    size_t n = 0;
    VkRect2D probe_area{};
    bool pixel_only = false;
    if (std::getenv("XN_DEBUG_PROBE_PIXEL_ONLY")) {
      if (const char* location = std::getenv("XN_DEBUG_PROBE_PX")) {
        int x = 0, y = 0;
        if (std::sscanf(location, "%d,%d", &x, &y) == 2 && x >= 0 && y >= 0 &&
            uint32_t(x) < color->image.width && uint32_t(y) < color->image.height) {
          probe_area = {{x, y}, {1, 1}};
          pixel_only = true;
        }
      }
      // Ignore passes that cannot cover the selected pixel.
      if (!pixel_only) return;
    }
    if (ReadbackImage(ReadableImage(*color), px_, pixel_only ? &probe_area : nullptr)) {
      if (!pixel_only) {
        if (const char* directory = std::getenv("XN_DEBUG_DUMP_DRAWS")) {
          std::filesystem::create_directories(directory);
          const auto path = std::filesystem::path(directory) / fmt::format(
              "swap{}_draw{}_vs{:016X}_{}x{}.ppm", guest_swap_number, draw_number,
              vshader->ucode_data_hash(), color->image.width, color->image.height);
          if (FILE* file = std::fopen(path.string().c_str(), "wb")) {
            std::vector<uint8_t> rgb(px_.size() / 4 * 3);
            for (size_t i = 0; i < px_.size() / 4; ++i)
              std::memcpy(rgb.data() + i * 3, px_.data() + i * 4, 3);
            std::fprintf(file, "P6\n%u %u\n255\n", color->image.width, color->image.height);
            std::fwrite(rgb.data(), 1, rgb.size(), file);
            std::fclose(file);
          }
        }
      }
      for (size_t i = 0; i + 3 < px_.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
          m = std::max(m, px_[i + c]);
          sum += px_[i + c];
          ++n;
        }
      }
    }
    // Inspect the actual GPU texture (including resolved cubemap faces), not
    // the stale CPU allocation used by the upload dump. One set per swap;
    // the existing shader filter limits this to the material being diagnosed.
    if (const char* directory = std::getenv("XN_DEBUG_DUMP_BOUND_TEXTURES")) {
      static uint64_t dumped_swap = ~uint64_t(0);
      if (pshader && dumped_swap != guest_swap_number) {
        dumped_swap = guest_swap_number;
        std::filesystem::create_directories(directory);
        const auto constants = std::filesystem::path(directory) / fmt::format("swap{}_pixel_constants.bin", guest_swap_number);
        if (FILE* file = std::fopen(constants.string().c_str(), "wb")) {
          std::fwrite(&regs[XE_GPU_REG_SHADER_CONSTANT_256_X], 1, 4096, file);
          std::fwrite(&regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 1, 160, file);
          std::fclose(file);
        }
        uint32_t seen = 0;
        for (const auto& binding : pshader->GetTextureBindingsAfterTranslation()) {
          const uint32_t slot = binding.fetch_constant;
          if (slot >= 32 || (seen & (1u << slot))) continue;
          seen |= 1u << slot;
          const auto fetch = regs.GetTextureFetch(slot);
          TextureEntry* texture = GetTexture(fetch);
          if (!texture || texture->unsupported) continue;
          // Raw RGBA8 captures are enough for encoded reflection targets.
          // Block-compressed images cannot be read with the blit helper.
          if (texture->image.format != VK_FORMAT_R8G8B8A8_UNORM) continue;
          std::vector<uint8_t> rgba;
          if (!ReadbackImage(texture->image, rgba)) continue;
          const auto path = std::filesystem::path(directory) / fmt::format(
              "swap{}_tf{}_addr{:08X}_{}x{}.ppm", guest_swap_number, slot,
              texture->key.base_address, texture->image.width, texture->image.height);
          if (FILE* file = std::fopen((path.string() + ".rgba").c_str(), "wb")) {
            std::fwrite(rgba.data(), 1, rgba.size(), file);
            std::fclose(file);
          }
          if (FILE* file = std::fopen(path.string().c_str(), "wb")) {
            std::fprintf(file, "P6\n%u %u\n255\n", texture->image.width, texture->image.height);
            for (size_t i = 0; i + 3 < rgba.size(); i += 4) std::fwrite(rgba.data() + i, 1, 3, file);
            std::fclose(file);
          }
        }
      }
    }
    // A flat background fill is invisible in a whole-frame mean but obvious at
    // one pixel: XN_DEBUG_PROBE_PX=x,y names a pixel inside the region under
    // investigation and this reports the draw that first writes it.
    std::string px_note;
    if (const char* pxs = std::getenv("XN_DEBUG_PROBE_PX")) {
      int px = 0, py = 0;
      if (std::sscanf(pxs, "%d,%d", &px, &py) == 2 && px >= 0 && py >= 0 &&
          uint32_t(px) < color->image.width && uint32_t(py) < color->image.height) {
        const size_t o = pixel_only ? 0 : (size_t(py) * color->image.width + size_t(px)) * 4;
        if (o + 3 < px_.size()) {
          px_note = fmt::format(" px({},{})=[{} {} {} {}]", px, py, px_[o], px_[o + 1],
                                px_[o + 2], px_[o + 3]);
        }
      }
    }
    // When the probe pixel CHANGES, this draw is the one that produced it -
    // so dump everything that decides its colour. Self-locating: it needs no
    // draw index, which the per-draw readbacks shift anyway.
    static std::array<uint8_t, 4> probe_prev{0, 0, 0, 0};
    if (!px_note.empty()) {
      std::array<uint8_t, 4> now{0, 0, 0, 0};
      {
        int qx = 0, qy = 0;
        std::sscanf(std::getenv("XN_DEBUG_PROBE_PX"), "%d,%d", &qx, &qy);
        const size_t o = pixel_only ? 0 : (size_t(qy) * color->image.width + size_t(qx)) * 4;
        for (int c = 0; c < 4; ++c) now[c] = px_[o + c];
      }
      if (now != probe_prev) {
        REXLOG_INFO(
            "xn-vk: PROBE-CHANGE d{} [{} {} {} {}] -> [{} {} {} {}] prim={} count={} vs={:016X} "
            "ps={:016X} blend={:#x} mask={:#x} depth={:#x} rt={}x{} samples={}",
            draw_number, probe_prev[0], probe_prev[1], probe_prev[2], probe_prev[3], now[0], now[1],
            now[2], now[3], uint32_t(prim_type), draw_count, vshader->ucode_data_hash(),
            pshader ? pshader->ucode_data_hash() : 0ull, key.blend_controls[0], key.color_write_mask,
            key.depth_control, color->image.width, color->image.height, key.samples);
        for (const Shader::VertexBinding& vb : vshader->vertex_bindings()) {
          xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(vb.fetch_constant);
          std::string attrs;
          for (const Shader::VertexBinding::Attribute& a : vb.attributes) {
            const auto& at = a.fetch_instr.attributes;
            attrs += fmt::format(" [fmt={} off={} exp={} signed={} int={} rf={}]",
                                 uint32_t(at.data_format), at.offset, at.exp_adjust,
                                 int(at.is_signed), int(at.is_integer), uint32_t(at.signed_rf_mode));
          }
          std::string words;
          const uint32_t addr = vf.address << 2;
          const uint32_t stride_b = vb.stride_words * 4;
          if (guest_base && stride_b && addr + stride_b * 4 <= guest_size) {
            for (uint32_t v = 0; v < 4; ++v) {
              words += fmt::format(" v{}=[", v);
              for (uint32_t w = 0; w < vb.stride_words && w < 12; ++w) {
                uint32_t raw;
                std::memcpy(&raw, guest_base + addr + v * stride_b + w * 4, 4);
                words += fmt::format("{:08X} ", __builtin_bswap32(raw));
              }
              words += "]";
            }
          }
          REXLOG_INFO("xn-vk:   vf{} addr={:#x} size={} stride_words={} endian={} attrs:{}{}",
                      vb.fetch_constant, addr, uint32_t(vf.size) << 2, vb.stride_words,
                      uint32_t(vf.endian), attrs, words);
        }
        {
          // The vertex shader's constants matter as much as the pixel
          // shader's: if it modulates the vertex colour per channel, a
          // constant we feed wrongly shows up as exactly the per-channel ratio
          // measured here.
          std::string vcs;
          const Shader::ConstantRegisterMap& vm = vshader->constant_register_map();
          for (uint32_t i = 0; i < 4; ++i) {
            uint64_t e = vm.float_bitmap[i];
            while (e) {
              const uint32_t b = rex::tzcnt(e);
              e &= e - 1;
              const uint32_t idx = (i << 6) + b;
              const uint32_t r = XE_GPU_REG_SHADER_CONSTANT_000_X + idx * 4;
              vcs += fmt::format(" c{}=({:.5g},{:.5g},{:.5g},{:.5g})", idx, regs.Get<float>(r),
                                 regs.Get<float>(r + 1), regs.Get<float>(r + 2),
                                 regs.Get<float>(r + 3));
            }
          }
          REXLOG_INFO("xn-vk:   vs float constants read:{}", vcs.empty() ? " (none)" : vcs);
        }
        if (pshader) {
          std::string cs;
          const Shader::ConstantRegisterMap& cm = pshader->constant_register_map();
          for (uint32_t i = 0; i < 4; ++i) {
            uint64_t e = cm.float_bitmap[i];
            while (e) {
              const uint32_t b = rex::tzcnt(e);
              e &= e - 1;
              const uint32_t idx = (i << 6) + b;
              const uint32_t r = XE_GPU_REG_SHADER_CONSTANT_256_X + idx * 4;
              cs += fmt::format(" c{}=({:.5g},{:.5g},{:.5g},{:.5g})", idx, regs.Get<float>(r),
                                regs.Get<float>(r + 1), regs.Get<float>(r + 2),
                                regs.Get<float>(r + 3));
            }
          }
          REXLOG_INFO("xn-vk:   ps float constants read:{}", cs.empty() ? " (none)" : cs);
          std::string tb;
          for (const Shader::TextureBinding& t : pshader->texture_bindings()) {
            tb += fmt::format(" ps_binding{}->tf{}", t.binding_index, t.fetch_constant);
          }
          for (const Shader::TextureBinding& t : vshader->texture_bindings()) {
            tb += fmt::format(" vs_binding{}->tf{}", t.binding_index, t.fetch_constant);
          }
          REXLOG_INFO("xn-vk:   shader texture bindings:{}", tb.empty() ? " (none)" : tb);
        }
        {
          // Every valid texture fetch constant, not only the ones the shader
          // binds: if the guest pointed a different slot at the artwork and we
          // are reading the wrong one, it shows up here.
          std::string all;
          for (uint32_t t = 0; t < 32; ++t) {
            xenos::xe_gpu_texture_fetch_t f = regs.GetTextureFetch(t);
            if (f.type != xenos::FetchConstantType::kTexture || !f.base_address) continue;
            all += fmt::format(" tf{}={:#x}/{}x{}/fmt{}/sw{:#x}/{}", t, f.base_address << 12,
                               f.size_2d.width + 1, f.size_2d.height + 1, uint32_t(f.format),
                               uint32_t(f.swizzle), f.tiled ? "tiled" : "linear");
          }
          REXLOG_INFO("xn-vk:   all texture fetch constants:{}", all.empty() ? " (none)" : all);
        }
      }
      probe_prev = now;
    }
    REXLOG_INFO("xn-vk: F{} after d{} rt#{} {}x{} max={} mean={:.2f}{}{}", frame_number, draw_number,
                color->id, color->image.width, color->image.height, m, n ? sum / double(n) : 0.0,
                last_texture_log, px_note);
    BeginFrameIfNeeded();
    debug_frame_active = true;  // the readback advanced frame_number
  }
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------
void VulkanRenderer::Impl::ClearImpl(const ClearParams& p) {
  InvalidateDrawState();
  ++stats.clears;
  if (!device) return;
  BeginFrameIfNeeded();
  const auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const auto rb_color_info = regs.Get<reg::RB_COLOR_INFO>();
  const auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  RenderTargetImage* color = GetColorTarget(RenderTargetBase(rb_color_info.value), rb_color_info.color_format,
                                            rb_surface_info.surface_pitch);
  if (!color) return;
  RenderTargetImage* depth =
      GetDepthTarget(RenderTargetBase(rb_depth_info.value), color->image.width, color->image.height,
                     color->samples);
  EnsureRendering(color, depth);
  VkClearAttachment atts[2];
  uint32_t n = 0;
  if ((p.flags & ClearParams::kColor) && (p.color_target_mask & 1)) {
    atts[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    atts[n].colorAttachment = 0;
    std::memcpy(atts[n].clearValue.color.float32, p.color, sizeof(p.color));
    ++n;
  }
  if (depth && (p.flags & (ClearParams::kDepth | ClearParams::kStencil))) {
    atts[n].aspectMask = ((p.flags & ClearParams::kDepth) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                         ((p.flags & ClearParams::kStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    atts[n].colorAttachment = 0;
    atts[n].clearValue.depthStencil = {p.depth, p.stencil};
    ++n;
  }
  if (!n) return;
  VkClearRect rect;
  rect.baseArrayLayer = 0;
  rect.layerCount = 1;
  if (p.width && p.height) {
    rect.rect.offset = {int32_t(std::min(p.x, color->image.width)),
                        int32_t(std::min(p.y, color->image.height))};
    rect.rect.extent = {std::min(p.width, color->image.width - uint32_t(rect.rect.offset.x)),
                        std::min(p.height, color->image.height - uint32_t(rect.rect.offset.y))};
  } else {
    rect.rect = {{0, 0}, {color->image.width, color->image.height}};
  }
  if (!rect.rect.extent.width || !rect.rect.extent.height) return;
  vkCmdClearAttachments(cb, n, atts, 1, &rect);
  if (p.flags & (ClearParams::kDepth | ClearParams::kStencil)) ForgetDepthClearTiles(*depth);
}

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------
void VulkanRenderer::Impl::ResolveFromRegisters() {
  ResolveParams p;
  p.from_copy_registers = true;
  ResolveImpl(p);
}

void VulkanRenderer::Impl::ResolveImpl(const ResolveParams& p_in) {
  InvalidateDrawState();
  ++stats.resolves;
  if (!device) return;
  BeginFrameIfNeeded();
  ResolveParams p = p_in;
  // The resolve scales colour by 2^copy_dest_exp_bias before converting to the
  // destination format - see resolve_bias.h for what that is for, and for the
  // precondition under which applying it once here equals the console applying
  // its format clamp at every blend step. Only meaningful for a register-driven
  // resolve; a caller-supplied ResolveParams carries no bias.
  int32_t copy_exp_bias = 0;
  if (p.from_copy_registers) {
    const auto copy_control = regs.Get<reg::RB_COPY_CONTROL>();
    const auto dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
    copy_exp_bias = dest_info.copy_dest_exp_bias;
    const auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
    p.source = copy_control.copy_src_select >= 4 ? ResolveParams::kDepth
                                                 : int32_t(copy_control.copy_src_select);
    p.copy = copy_control.copy_command == xenos::CopyCommand::kRaw ||
             copy_control.copy_command == xenos::CopyCommand::kConvert;
    p.dest_guest_address = regs.Get<uint32_t>(XE_GPU_REG_RB_COPY_DEST_BASE);
    p.dest_pitch = dest_pitch.copy_dest_pitch;
    p.dest_width = dest_pitch.copy_dest_pitch;
    p.dest_height = dest_pitch.copy_dest_height;
    p.dest_format = uint32_t(dest_info.copy_dest_format);
    p.dest_endian = uint32_t(dest_info.copy_dest_endian);
    p.dest_number = uint32_t(dest_info.copy_dest_number);
    p.dest_tiled = true;
    if (p.source == ResolveParams::kDepth) {
      const auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
      p.dest_format = uint32_t(
          rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8
              ? xenos::TextureFormat::k_24_8_FLOAT
              : xenos::TextureFormat::k_24_8);
      copy_exp_bias = 0;
    }
    if (!p.src_width || !p.src_height) {
      draw_util::Scissor scissor;
      draw_util::GetScissor(regs, scissor);
      p.src_x = scissor.offset[0];
      p.src_y = scissor.offset[1];
      p.src_width = scissor.extent[0];
      p.src_height = scissor.extent[1];
      // D3D9's resolve rectangle is three CPU-written float2 vertices in
      // fetch 0. The scissor only clips it; it does not define the copy.
      // Match the Xenia/ReXGlue GetResolveInfo top-left / 8-pixel alignment
      // contract (thirdparty/licenses/ReXGlue-Xenia.txt).
      const auto fetch = regs.GetVertexFetch(0);
      const uint64_t address = uint64_t(fetch.address) * 4;
      if (fetch.type == xenos::FetchConstantType::kVertex && fetch.size == 6 && guest_base &&
          address + 24 <= guest_size) {
        PrepareGuestRead(uint32_t(address),24);
        int32_t lo[2] = {INT32_MAX, INT32_MAX}, hi[2] = {INT32_MIN, INT32_MIN};
        bool valid = true;
        const float half = regs.Get<reg::PA_SU_VTX_CNTL>().pix_center ==
            xenos::PixelCenter::kD3DZero ? .5f : 0.f;
        for (uint32_t i = 0; i < 6; ++i) {
          float value;
          std::memcpy(&value, guest_base + address + i * 4, 4);
          value = xenos::GpuSwap(value, fetch.endian) + half;
          if (!std::isfinite(value) || std::abs(value) > 32768.f) { valid = false; break; }
          const int32_t pixel = int32_t(std::floor((std::nearbyint(double(value) * 256) + 127) / 256));
          lo[i & 1] = std::min(lo[i & 1], pixel);
          hi[i & 1] = std::max(hi[i & 1], pixel);
        }
        if (valid) {
          const auto window = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
          const bool offset = regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable;
          for (uint32_t axis = 0; axis < 2; ++axis) {
            const int32_t delta = !offset ? 0 : axis ? window.window_y_offset : window.window_x_offset;
            const int32_t start = scissor.offset[axis];
            const int32_t end = start + scissor.extent[axis];
            lo[axis] = std::clamp(lo[axis] + delta, start, end) & ~7;
            hi[axis] = (std::clamp(hi[axis] + delta, start, end) + 7) & ~7;
          }
          p.src_x = lo[0]; p.src_y = lo[1];
          p.src_width = hi[0] - lo[0]; p.src_height = hi[1] - lo[1];
        }
      }
      p.dest_x = p.src_x;
      p.dest_y = p.src_y;
    }
    // The rectangle may have no fragments after the tile/window scissor.
    // Forza emits these clipped copies after each real strip resolve. They
    // must not reach the implicit full-target fallback below or clear the
    // source: that overwrites the just-resolved strip with an empty image.
    if (!p.src_width || !p.src_height) return;
    // XN_DEBUG_RESOLVE_RECT=1: the resolve's SOURCE rectangle against the
    // scissor that was actually in force when the level was drawn. If the
    // resolve copies a larger rect than was drawn, the margin holds whatever
    // the target held before - the previous, brighter level - and every
    // downstream level blurs and upsamples that stale margin.
    if (launch_diagnostics.resolve_rect) {
      draw_util::Scissor sc_now;
      draw_util::GetScissor(regs, sc_now);
      REXLOG_INFO(
          "xn-vk: RESOLVERECT F{} src=({},{} {}x{}) scissor=({},{} {}x{}) dest={}x{} pitch={} "
          "dest_addr={:#x} margin=({},{})",
          frame_number, p.src_x, p.src_y, p.src_width, p.src_height, sc_now.offset[0],
          sc_now.offset[1], sc_now.extent[0], sc_now.extent[1], p.dest_width, p.dest_height,
          p.dest_pitch, p.dest_guest_address, int32_t(p.src_width) - int32_t(sc_now.extent[0]),
          int32_t(p.src_height) - int32_t(sc_now.extent[1]));
    }
    // Resolve-with-clear: RB_COLOR_CLEAR holds the value packed in the render
    // target format (k_8_8_8_8: R in the low byte); RB_DEPTH_CLEAR is the
    // packed depth/stencil (D24S8: stencil in the low byte).
    p.clear_after = copy_control.color_clear_enable || copy_control.depth_clear_enable;
    p.clear.flags = 0;
    if (copy_control.color_clear_enable) {
      p.clear.flags |= ClearParams::kColor;
      const uint32_t c = regs[XE_GPU_REG_RB_COLOR_CLEAR];
      const auto format = xenos::GetStorageColorFormat(regs.Get<reg::RB_COLOR_INFO>(
          reg::RB_COLOR_INFO::rt_register_indices[p.source >= 0 && p.source < 4 ? p.source : 0])
          .color_format);
      if (format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
          format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT) {
        // The packed clear is in the source target's format, independently
        // of the resolve destination. Forza's HDR grey 0x08521485 contains
        // three identical 7e3 components; decoding it as bytes makes it pink.
        for (uint32_t i = 0; i < 3; ++i) {
          const uint32_t value = (c >> (i * 10)) & 1023;
          p.clear.color[i] = format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT
              ? xenos::Float7e3To32(value) : float(value) / 1023.0f;
        }
        p.clear.color[3] = float(c >> 30) / 3.0f;
      } else {
        for (uint32_t i = 0; i < 4; ++i)
          p.clear.color[i] = float((c >> (i * 8)) & 255) / 255.0f;
      }
    }
    if (copy_control.depth_clear_enable) {
      p.clear.flags |= ClearParams::kDepth | ClearParams::kStencil;
      const uint32_t d = regs[XE_GPU_REG_RB_DEPTH_CLEAR];
      p.clear.depth = regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
              xenos::DepthRenderTargetFormat::kD24FS8
          ? xenos::Float20e4To32(d >> 8) : xenos::UNorm24To32(d >> 8);
      p.clear.stencil = d & 0xFF;
    }
    p.clear.x = p.src_x;
    p.clear.y = p.src_y;
    p.clear.width = p.src_width;
    p.clear.height = p.src_height;
    if (p.source == ResolveParams::kDepth && !p.copy) {
      // Depth-target clear only.
      ClearImpl(p.clear);
      return;
    }
  }
  const auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  RenderTargetImage* src_target = nullptr;
  if (p.source == ResolveParams::kDepth) {
    const auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    if (active_depth && active_depth->edram_base == RenderTargetBase(rb_depth_info.value) &&
        active_depth->xenos_format == uint32_t(rb_depth_info.depth_format)) {
      src_target = active_depth;
    } else {
      for (auto& [key, target] : depth_targets) {
        if (target.edram_base == RenderTargetBase(rb_depth_info.value) &&
            target.xenos_format == uint32_t(rb_depth_info.depth_format)) {
          if (!src_target || (target.image.width * target.image.height >
                              src_target->image.width * src_target->image.height)) {
            src_target = &target;
          }
        }
      }
    }
    if (!src_target) {
      const auto rt_info = regs.Get<reg::RB_COLOR_INFO>();
      RenderTargetImage* color =
          GetColorTarget(RenderTargetBase(rt_info.value), rt_info.color_format,
                         rb_surface_info.surface_pitch,
                         p.src_y + p.src_height);
      if (color) {
        src_target = GetDepthTarget(RenderTargetBase(rb_depth_info.value),
                                    color->image.width, color->image.height,
                                    color->samples);
      }
    }
  } else {
    const auto rt_info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[uint32_t(p.source) & 3]);
    src_target =
        GetColorTarget(RenderTargetBase(rt_info.value), rt_info.color_format,
                       rb_surface_info.surface_pitch,
                       p.src_y + p.src_height);
  }
  if (!src_target) return;
  if (p.source != ResolveParams::kDepth) {
    // Fixed16 guest targets span [-32,32], while the host SNORM attachment
    // stores them divided by 32 (see AdjustColorExpBiasForFormat). Undo that
    // storage remap before applying the guest resolve bias and destination
    // conversion. Otherwise Forza's shadow moments resolve at 1/32 strength.
    const auto source_format = xenos::ColorRenderTargetFormat(src_target->xenos_format);
    if (source_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        source_format == xenos::ColorRenderTargetFormat::k_16_16_16_16)
      copy_exp_bias += 5;
    // A resolve can switch the EDRAM view without drawing through it first.
    // Forza renders its movie through GAMMA then resolves through UNORM at
    // the same base/pitch. Transfer ownership before reading that view, just
    // as a normal attachment switch does, or the copy sees stale pixels.
    EnsureRendering(src_target, nullptr);
    EndRendering();
  }
  if (!p.src_width || !p.src_height) {
    p.src_x = p.src_y = 0;
    p.src_width = src_target->image.width;
    p.src_height = src_target->image.height;
  }
  if (!p.dest_width || !p.dest_height) {
    p.dest_width = p.src_width;
    p.dest_height = p.src_height;
  }
  // RB_COPY_DEST_PITCH.copy_dest_pitch is the destination's ROW STRIDE, aligned
  // up to 32 texels - it is NOT the texture's width. Sizing the image from it
  // creates a texture wider than the one the guest will fetch, leaves the
  // remainder never written, and makes normalised UVs address the wrong texels.
  // The real width is what is actually resolved in; the source rectangle is the
  // guest's own, rounded up to Xenos' 8x8 resolve granularity. Measured on
  // Geometry Wars 2's bloom chain, where pitch and width diverge on every level:
  //
  //   pitch x height   source rect     real width
  //      32 x 1           8 x 8              8
  //      32 x 6          16 x 8             16
  //      96 x 50         80 x 56            80
  //     256 x 135       240 x 136          240
  //
  // The pitch is still the stride, and is kept as such in the cache key below.
  if (p.src_width && !p.dest_x && p.src_width < p.dest_width &&
      p.dest_width - p.src_width < 32) p.dest_width = p.src_width;
  if (!p.copy) {
    if (p.clear_after) ClearImpl(p.clear);
    return;
  }
  TexFormatInfo fi;
  if (!GetTexFormatInfo(xenos::TextureFormat(p.dest_format), fi) || fi.expand_to_rgba8 ||
      fi.block_w != 1) {
    static int logged = 0;
    if (logged++ < 4) REXLOG_WARN("xn-vk: resolve to format {} not supported", p.dest_format);
    return;
  }

  bool display_resolve = false;
  if (p.dest_tiled && IsDisplayColorFormat(p.dest_format)) {
    // A cached strip can claim a full-height image at an interior address.
    // Its apparent range then overlaps the next frontbuffer. Forza's three
    // display strips were merged into that stale owner, losing the first
    // strip of every other frame. Prefer the actual allocations VdSwap gave
    // us, even for a resolve at their exact base or before a view exists.
    const uint32_t pitch = (p.dest_pitch + 31) & ~31u;
    const uint32_t log_bpp = untile::Log2Ceil(fi.bytes_per_block);
    for (const auto& [base, extent] : display_buffers) {
      if (p.dest_guest_address < base || p.dest_height != extent.height ||
          pitch != ((extent.width + 31) & ~31u)) continue;
      const uint32_t height = (extent.height + 31) & ~31u;
      const uint32_t delta = p.dest_guest_address - base;
      if (uint64_t(delta) >= uint64_t(pitch) * height * fi.bytes_per_block) continue;
      for (uint32_t y = 0; !display_resolve && y < height; y += 32) {
        const uint32_t row = untile::TiledOffset2DRow(y, pitch, log_bpp);
        for (uint32_t x = 0; x < pitch; x += 32) {
          if (untile::TiledOffset2DColumn(x, y, log_bpp, row) != delta) continue;
          if (uint64_t(x) + p.src_x + p.src_width <= extent.width &&
              uint64_t(y) + p.src_y + p.src_height <= extent.height) {
            p.dest_guest_address = base;
            p.dest_x = x + p.src_x;
            p.dest_y = y + p.src_y;
            p.dest_width = extent.width;
            p.dest_height = extent.height;
            display_resolve = true;
          }
          break;
        }
      }
      if (display_resolve) break;
    }
  }
  if (p.dest_tiled && !display_resolve) {
    for (const auto& [rkey, owner] : resolved_by_key) {
      const uint32_t base = rkey.address;
      if (p.dest_guest_address <= base || !AreFormatsEquivalent(owner->key.format, p.dest_format) ||
          owner->key.pitch != p.dest_pitch) continue;
      if (uint64_t(p.dest_guest_address - base) >= uint64_t(owner->guest_size)) continue;
      const bool height_sub = (owner->image.height > p.src_height || owner->key.height > p.src_height);
      const bool width_sub = (owner->image.width > p.src_width || owner->key.width > p.src_width);
      if (!height_sub && !width_sub) continue;
      const uint32_t pitch = (owner->key.pitch + 31) & ~31u;
      const uint32_t height = (owner->image.height + 31) & ~31u;
      const uint32_t delta = p.dest_guest_address - base;
      if (uint64_t(delta) >= uint64_t(pitch) * height * fi.bytes_per_block) continue;
      const uint32_t log_bpp = untile::Log2Ceil(fi.bytes_per_block);
      bool found = false;
      for (uint32_t y = 0; !found && y < height; y += 32) {
        const uint32_t row = untile::TiledOffset2DRow(y, pitch, log_bpp);
        for (uint32_t x = 0; x < pitch; x += 32) {
          if (untile::TiledOffset2DColumn(x, y, log_bpp, row) != delta) continue;
          if (x + p.src_x + p.src_width <= owner->image.width &&
              y + p.src_y + p.src_height <= owner->image.height) {
            p.dest_guest_address = base;
            if (p.src_x >= x) p.src_x -= x;
            if (p.src_y >= y) p.src_y -= y;
            // The destination address names the TILE a strip starts in; the
            // copy rectangle's own offset then places it INSIDE that tile.
            // Dropping that offset left a band unwritten: Burnout Revenge
            // composes its frame from three strips resolving to tile rows
            // 0/224/480 with rects at y 0/16/0 - that is rows 0/240/480, 240
            // apart - so ignoring the 16 put the middle strip at 224..463 and
            // nothing ever wrote 464..479, a 16-row bar across the screen.
            p.dest_x = x + p.src_x;
            p.dest_y = y + p.src_y;
            p.dest_width = owner->image.width;
            p.dest_height = owner->image.height;
            found = true;
          }
          break;
        }
      }
      if (found) {
        if (debug_frame_active || launch_diagnostics.resolve_rect) {
          REXLOG_INFO("xn-vk: merged sub-tile resolve {:#x} -> owner {:#x} at ({},{}) size {}x{} (owner {}x{})",
                      delta + base, base, p.dest_x, p.dest_y, p.src_width, p.src_height,
                      owner->image.width, owner->image.height);
        }
        break;
      }
    }
  }

  // MCLA cycles one address through several sizes/formats every frame. Keep
  // those allocations alive instead of waiting for the whole GPU and creating
  // them again at every change. Queue-ordered transitions protect reuse; the
  // address lookup for sampling/presentation still selects the latest resolve.
  TextureKey resolve_key;
  resolve_key.base_address = p.dest_guest_address;
  resolve_key.format = p.dest_format;
  resolve_key.width = p.dest_width;
  resolve_key.height = p.dest_height;
  resolve_key.pitch = p.dest_pitch ? p.dest_pitch : p.dest_width;
  resolve_key.tiled = p.dest_tiled;
  resolve_key.endian = p.dest_endian;
  auto cached = resolve_images.find(resolve_key);
  TextureEntry* entry = cached == resolve_images.end() ? nullptr : cached->second.get();
  if (entry) ++stats.resolve_images_reused;
  if (!entry) {
    auto e = std::make_unique<TextureEntry>();
    e->key = resolve_key;
    e->resolved = true;
    e->host_swizzle = fi.host_swizzle;
    // Footprint in guest memory is stride x height, not width x height.
    e->guest_size = e->key.pitch * p.dest_height * fi.bytes_per_block;
    if (!CreateImage(p.dest_width, p.dest_height, fi.vk,
                     // COLOR_ATTACHMENT so a biased resolve can render into it
                     // (resolve_bias.h); a plain resolve only ever copies.
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, e->image)) {
      return;
    }
    entry = e.get();
    resolve_images.emplace(resolve_key, std::move(e));
    ++stats.resolve_images_created;
  }
  resolved_by_key[ResolvedKey{p.dest_guest_address, p.dest_format}] = entry;
  entry->resolve_serial = stats.resolves;
  auto ait = resolved_by_address.find(p.dest_guest_address);
  if (ait == resolved_by_address.end() || p.source != ResolveParams::kDepth) {
    resolved_by_address[p.dest_guest_address] = entry;
  }
  // Keep each allocation once, ordered by its most recent resolve. Appending
  // every frame made this lookup grow without limit and made presentation
  // traverse an ever-growing list of duplicate entries.
  auto& address_views = resolved_by_addr[p.dest_guest_address];
  std::erase(address_views, entry);
  address_views.push_back(entry);
  entry->last_frame_used = frame_number;
  // Keep the host image in logical colour order and carry the resolve's R/B
  // swap in its texture view. D3D fetches these resolves with BGRA swizzle;
  // omitting this swap feeds blue glow into red geometry (and vice versa).
  // Presentation consumes the logical image, as the display also expects BGRA.
  entry->host_swizzle = fi.host_swizzle;
  if (p.from_copy_registers && regs.Get<reg::RB_COPY_DEST_INFO>().copy_dest_swap &&
      p.source != ResolveParams::kDepth)
    entry->host_swizzle = GuestToHostSwizzle(0x60A, fi.host_swizzle);

  // XN_DEBUG_LEVEL_CURVE=1: the mean of the small bloom level at every
  // resolve, from a cold start. A curve that climbs and plateaus means the
  // target is never cleared and light accumulates across frames; a flat line
  // from frame 0 excludes accumulation.
  if (launch_diagnostics.level_curve && !src_target->is_depth && src_target->image.width == 320) {
    std::vector<uint8_t> px;
    if (ReadbackImage(ReadableImage(*src_target), px)) {
      double sum = 0;
      uint64_t n = 0;
      for (size_t i = 0; i + 3 < px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
          sum += px[i + c];
          ++n;
        }
      }
      REXLOG_INFO("levelcurve frame {} mean {:.3f}", frame_number, n ? sum / double(n) : 0.0);
    }
    BeginFrameIfNeeded();
  }
  if (debug_frame_active && (std::getenv("XN_DEBUG_PROBE_RESOLVE") ||
                             std::getenv("XN_DEBUG_DUMP_PASSES"))) {
    std::vector<uint8_t> px;
    uint8_t m = 0;
    EndRendering();
    GpuImage& probe_source = ReadableImage(*src_target);
    if (ReadbackImage(probe_source, px)) {
      for (uint8_t v : px) m = std::max(m, v);
      if (const char* directory = std::getenv("XN_DEBUG_DUMP_PASSES")) {
        std::filesystem::create_directories(directory);
        const auto path = fmt::format("{}/swap{}_resolve{}_rt{}_to_{:08X}.ppm",
            directory, guest_swap_number, stats.resolves, src_target->id, p.dest_guest_address);
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
          std::fprintf(file, "P6\n%u %u\n255\n", probe_source.width, probe_source.height);
          for (size_t i = 0; i + 3 < px.size(); i += 4) std::fwrite(px.data() + i, 1, 3, file);
          std::fclose(file);
        }
      }
    }
    REXLOG_INFO("xn-vk: F{} pre-resolve rt#{} {}x{} max={}", frame_number, src_target->id,
                src_target->image.width, src_target->image.height, m);
    BeginFrameIfNeeded();  // the readback closed the frame
    debug_frame_active = true;
  }
  if (debug_frame_active) {
    REXLOG_INFO("xn-vk: F{} RESOLVE src=rt#{} base={:#x} {}x{} fmt={} pitch={} -> dest={:#x} "
                "{}x{} fmt={} src_rect=({},{} {}x{}) copy={} clear_after={} "
                "dest_info={:#x} exp_bias={} surface_msaa={} dest_xy=({}, {}) raw_dest={:#x}",
                frame_number, src_target->id,
                p.source == ResolveParams::kDepth
                    ? RenderTargetBase(regs.Get<reg::RB_DEPTH_INFO>().value)
                    : RenderTargetBase(regs.Get<reg::RB_COLOR_INFO>().value),
                src_target->image.width, src_target->image.height,
                p.source == ResolveParams::kDepth ? uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_format)
                                                  : uint32_t(regs.Get<reg::RB_COLOR_INFO>().color_format),
                uint32_t(rb_surface_info.surface_pitch), p.dest_guest_address, p.dest_width,
                p.dest_height, p.dest_format, p.src_x, p.src_y, p.src_width, p.src_height, p.copy,
                p.clear_after, regs[XE_GPU_REG_RB_COPY_DEST_INFO],
                int32_t(regs.Get<reg::RB_COPY_DEST_INFO>().copy_dest_exp_bias),
                uint32_t(rb_surface_info.msaa_samples), p.dest_x, p.dest_y,
                regs[XE_GPU_REG_RB_COPY_DEST_BASE]);
  }
  EndRendering();
  GpuWorkScope resolve_profile(WorkProfile(), cb, "explicit_resolve", guest_swap_number,
      src_target->id, p.src_width, p.src_height, uint32_t(src_target->samples));
  GpuImage& resolve_src = ReadableImage(*src_target);
  const uint32_t sx = std::min(p.src_x, resolve_src.width);
  const uint32_t sy = std::min(p.src_y, resolve_src.height);
  const uint32_t dx = std::min(p.dest_x, entry->image.width);
  const uint32_t dy = std::min(p.dest_y, entry->image.height);
  const uint32_t w = std::min({p.src_width, resolve_src.width - sx, entry->image.width - dx});
  const uint32_t h = std::min({p.src_height, resolve_src.height - sy, entry->image.height - dy});
  if (w < std::min(p.src_width, entry->image.width - dx) ||
      h < std::min(p.src_height, entry->image.height - dy)) {
    static int logged = 0;
    if (logged++ < 4) {
      REXLOG_WARN("xn-vk: resolve to {:#x} leaves part of the destination unwritten: copying "
                  "{}x{} into {}x{} at ({},{}) - those texels keep their cleared value",
                  p.dest_guest_address, w, h, entry->image.width, entry->image.height, dx, dy);
    }
  }

  const bool is_depth = (p.source == ResolveParams::kDepth);
  static const bool no_exp_bias = std::getenv("XN_VK_NO_RESOLVE_EXP_BIAS") != nullptr;
  bool biased = false;
  if (w && h && is_depth && resolve_bias.Init(device)) {
    Transition(resolve_src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(entry->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkImageView src_view = resolve_src.depth_view ? resolve_src.depth_view : resolve_src.view;
    bool ok = resolve_bias.Apply(cb, frames[frame_index].texture_pool,
                                 src_view, entry->image.view, entry->image.format,
                                 int32_t(sx), int32_t(sy), int32_t(dx), int32_t(dy), w, h,
                                 1.0f);
    if (!ok) {
      static int logged = 0;
      if (logged++ < 4) REXLOG_WARN("xn-vk: depth resolve pass failed into {:#x}", p.dest_guest_address);
    }
    // Preserve stencil and the guest depth encoding for raw colour aliases.
    // The numerical R32_SFLOAT image above remains the depth sampling view.
    // Keep packed copies separately from actual resolves, since the game can
    // reuse this address for a real colour buffer earlier in the same frame.
    TextureKey packed_key = entry->key;
    packed_key.format = uint32_t(xenos::TextureFormat::k_8_8_8_8);
    auto& packed = resolve_texture_views[packed_key];
    if (!packed) {
      auto e = std::make_unique<TextureEntry>();
      e->key = packed_key;
      e->resolved = true;
      e->host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
      if (CreateImage(entry->image.width, entry->image.height, VK_FORMAT_R8G8B8A8_UNORM,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, e->image)) {
        packed = std::move(e);
        ++stats.resolve_images_created;
      }
    }
    if (packed && resolve_src.stencil_view && resolve_depth_pack.Init(device, true)) {
      Transition(packed->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      if (resolve_depth_pack.Apply(cb, frames[frame_index].texture_pool,
          src_view, packed->image.view, packed->image.format,
          int32_t(sx), int32_t(sy), int32_t(dx), int32_t(dy), w, h, 1.0f,
          resolve_src.stencil_view, p.dest_format == uint32_t(xenos::TextureFormat::k_24_8_FLOAT))) {
        packed->resolve_serial = entry->resolve_serial;
        packed->last_frame_used = frame_number;
        entry->packed_depth = packed.get();
      }
      Transition(packed->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    Transition(resolve_src, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  } else if (w && h && ((copy_exp_bias != 0 && !no_exp_bias) ||
             ((p.dest_format == uint32_t(xenos::TextureFormat::k_16) ||
               p.dest_format == uint32_t(xenos::TextureFormat::k_16_16) ||
               p.dest_format == uint32_t(xenos::TextureFormat::k_16_16_16_16)) &&
              p.dest_number >= 1 && p.dest_number <= 3) ||
             p.dest_format == uint32_t(xenos::TextureFormat::k_6_5_5)) && resolve_bias.Init(device)) {
    Transition(resolve_src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(entry->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    biased = resolve_bias.Apply(cb, frames[frame_index].texture_pool,
                                resolve_src.view, entry->image.view, entry->image.format,
                                int32_t(sx), int32_t(sy), int32_t(dx), int32_t(dy), w, h,
                                std::ldexp(1.0f, no_exp_bias ? 0 : copy_exp_bias), VK_NULL_HANDLE, false,
                                p.dest_format == uint32_t(xenos::TextureFormat::k_6_5_5),
                                (p.dest_format == uint32_t(xenos::TextureFormat::k_16) ||
                                 p.dest_format == uint32_t(xenos::TextureFormat::k_16_16) ||
                                 p.dest_format == uint32_t(xenos::TextureFormat::k_16_16_16_16))
                                    ? p.dest_number : 0);
    if (biased) {
      static bool announced = false;
      if (!announced) {
        announced = true;
        REXLOG_INFO("xn-vk: resolve exponent bias active (first: {} -> x{}); a title whose "
                    "resolves are all exp_bias=0 never reaches this",
                    copy_exp_bias, std::ldexp(1.0f, copy_exp_bias));
      }
    }
    static int logged = 0;
    if (!biased && logged++ < 4) {
      REXLOG_WARN("xn-vk: resolve exp_bias {} not applied (pass unavailable); colour will be "
                  "{}x too bright into {:#x}",
                  copy_exp_bias, std::ldexp(1.0f, -copy_exp_bias), p.dest_guest_address);
    }
  }

  if (w && h && !biased && !is_depth) {
    Transition(resolve_src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(entry->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {int32_t(sx), int32_t(sy), 0};
    blit.srcOffsets[1] = {int32_t(sx + w), int32_t(sy + h), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
    blit.dstOffsets[1] = {int32_t(dx + w), int32_t(dy + h), 1};
    vkCmdBlitImage(cb, resolve_src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   entry->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_NEAREST);
  }
  Transition(entry->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  if (w && h) {
    RecordResolvedRegion(*entry, {{int32_t(dx), int32_t(dy)}, {w, h}});
    if (is_depth && entry->packed_depth) {
      entry->packed_depth->resolved_regions = entry->resolved_regions;
      entry->packed_depth->fully_resolved = entry->fully_resolved;
    }
  }

  // Depth images can't be blitted directly to a colour readback. Capture the
  // numerical depth resolve instead when inspecting shadow-map passes.
  if (is_depth && debug_frame_active) {
    if (const char* directory = std::getenv("XN_DEBUG_DUMP_PASSES")) {
      std::vector<uint8_t> pixels;
      if (ReadbackImage(entry->image, pixels)) {
        const auto path = fmt::format("{}/swap{}_depth_{:08X}.ppm",
                                     directory, guest_swap_number, p.dest_guest_address);
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
          std::fprintf(file, "P6\n%u %u\n255\n", entry->image.width, entry->image.height);
          for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
            const uint8_t grey[3] = {pixels[i], pixels[i], pixels[i]};
            std::fwrite(grey, 1, 3, file);
          }
          std::fclose(file);
        }
      }
      BeginFrameIfNeeded();
      debug_frame_active = true;
    }
  }

  // XN_DEBUG_RESOLVED_CURVE=<width>: the mean of every resolved level of that
  // width, at every resolve, from a cold start. A curve that climbs and
  // plateaus means the level is never cleared and light accumulates across
  // frames; a flat line from frame 0 excludes accumulation.
  //
  // Read from the resolve DESTINATION, not the render target, and deliberately
  // so: the destination has had the exponent bias applied and is UNORM, so it
  // fits the 8-bit readback. A 7e3 target's values run to 32, would pin at 255
  // and would report a flat line whatever the truth was. (XN_DEBUG_LEVEL_CURVE
  // above reads the target instead and is the first title's probe, where every
  // target is 8-bit and that distinction does not arise. Left alone.)
  static const uint32_t resolved_curve_width = [] {
    const char* e = std::getenv("XN_DEBUG_RESOLVED_CURVE");
    const int n = e ? std::atoi(e) : 0;
    return n > 0 ? uint32_t(n) : 0u;
  }();
  if (resolved_curve_width && entry->image.width == resolved_curve_width) {
    std::vector<uint8_t> px;
    if (ReadbackImage(entry->image, px)) {
      double sum = 0;
      uint64_t n = 0, hot = 0;
      for (size_t i = 0; i + 3 < px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
          sum += px[i + c];
          if (px[i + c] >= 254) ++hot;
          ++n;
        }
      }
      // Interior against the right/bottom margin. Successive bloom levels share
      // one render target and are distinguished only by scissor, while the
      // resolve's source rectangle is tile-rounded LARGER than that scissor -
      // so the outer band of every level may hold the previous, larger, brighter
      // level rather than anything this pass drew. If that is happening the
      // margin reads brighter than the interior; if the two match, it is not.
      const uint32_t iw = entry->image.width, ih = entry->image.height;
      const uint32_t band = 4;
      double in_sum = 0, mg_sum = 0;
      uint64_t in_n = 0, mg_n = 0;
      for (uint32_t y = 0; y < ih; ++y) {
        for (uint32_t x = 0; x < iw; ++x) {
          const size_t i = (size_t(y) * iw + x) * 4;
          if (i + 3 >= px.size()) continue;
          const bool margin = (x + band >= iw) || (y + band >= ih);
          for (int c = 0; c < 3; ++c) {
            if (margin) { mg_sum += px[i + c]; ++mg_n; } else { in_sum += px[i + c]; ++in_n; }
          }
        }
      }
      // Percentiles matter more than the mean for a bloom source: a floor lifts
      // the median, a gain only moves the top. Luma, over the whole surface.
      std::vector<uint8_t> lum;
      lum.reserve(px.size() / 4);
      for (size_t i = 0; i + 3 < px.size(); i += 4) {
        lum.push_back(uint8_t((54u * px[i] + 183u * px[i + 1] + 19u * px[i + 2]) >> 8));
      }
      std::sort(lum.begin(), lum.end());
      auto pct = [&](double q) {
        return lum.empty() ? 0 : int(lum[size_t(q * double(lum.size() - 1))]);
      };
      REXLOG_INFO("resolvedcurve frame {} dest {:#x} {}x{} p10 {} p50 {} p90 {} p99 {}",
                  frame_number, p.dest_guest_address, entry->image.width, entry->image.height,
                  pct(0.10), pct(0.50), pct(0.90), pct(0.99));
      REXLOG_INFO("resolvedcurve frame {} dest {:#x} {}x{} mean {:.3f} hot {:.2f}% "
                  "interior {:.3f} margin {:.3f}",
                  frame_number, p.dest_guest_address, entry->image.width, entry->image.height,
                  n ? sum / double(n) : 0.0, n ? 100.0 * double(hot) / double(n) : 0.0,
                  in_n ? in_sum / double(in_n) : 0.0, mg_n ? mg_sum / double(mg_n) : 0.0);
    }
    BeginFrameIfNeeded();  // the readback closed the frame
  }

  if (p.clear_after) ClearImpl(p.clear);
}

// ---------------------------------------------------------------------------
// Present
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Display gamma ramp
// ---------------------------------------------------------------------------
// Runs the frame through the guest's DC_LUT before it is shown. See
// gamma_ramp.h for what the curve is and why it matters. Returns the image to
// present; unchanged when the guest has never programmed a ramp.
GpuImage* VulkanRenderer::Impl::ApplyGammaRamp(GpuImage* source) {
  // XN_VK_NO_GAMMA_RAMP=1: present without it, i.e. the behaviour before this
  // existed. The A/B that shows what the ramp is worth.
  static const bool disabled = std::getenv("XN_VK_NO_GAMMA_RAMP") != nullptr;
  if (disabled || !source || !dc_lut_valid || dc_lut_entries == 0) return source;
  if (!gamma_ramp.Init(device)) return source;

  const uint32_t entries = std::min<uint32_t>(dc_lut_entries, 256);
  if (dc_lut_image.image && dc_lut_image.width != entries) {
    frames[frame_index].garbage_images.push_back(dc_lut_image);
    dc_lut_image = {};
    dc_lut_uploaded_writes = 0;
  }
  if (!dc_lut_image.image) {
    if (!CreateImage(entries, 1, VK_FORMAT_R16G16B16A16_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, dc_lut_image)) {
      return source;
    }
    dc_lut_uploaded_writes = 0;
  }
  // The guest rewrites the ramp continuously (the second title ~1,100 times by
  // frame 600), so re-upload only when the write count has moved.
  if (dc_lut_uploaded_writes != dc_lut_writes) {
    HostBuffer staging;
    const VkDeviceSize bytes = VkDeviceSize(entries) * 8;
    if (!CreateHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) return source;
    auto* out = reinterpret_cast<uint16_t*>(staging.mapped);
    for (uint32_t i = 0; i < entries; ++i) {
      out[i * 4 + 0] = dc_lut[i][0];
      out[i * 4 + 1] = dc_lut[i][1];
      out[i * 4 + 2] = dc_lut[i][2];
      out[i * 4 + 3] = 0xFFFF;
    }
    EndRendering();
    GpuWorkScope lut_profile(WorkProfile(), cb, "gamma_lut_upload", guest_swap_number,
        0, entries, 1, 1, bytes);
    Transition(dc_lut_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {entries, 1, 1};
    vkCmdCopyBufferToImage(cb, staging.buffer, dc_lut_image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    frames[frame_index].garbage.push_back(staging);
    dc_lut_uploaded_writes = dc_lut_writes;
  }

  if (gamma_out.image &&
      (gamma_out.width != source->width || gamma_out.height != source->height ||
       gamma_out.format != source->format)) {
    frames[frame_index].garbage_images.push_back(gamma_out);
    gamma_out = {};
  }
  if (!gamma_out.image) {
    if (!CreateImage(source->width, source->height, source->format,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, gamma_out)) {
      return source;
    }
    REXLOG_INFO("xn-vk: display gamma ramp active ({} entries, {} guest writes so far)", entries,
                dc_lut_writes);
  }

  EndRendering();
  GpuWorkScope gamma_profile(WorkProfile(), cb, "gamma_pass", guest_swap_number,
      0, source->width, source->height, 1);
  Transition(*source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  Transition(dc_lut_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  Transition(gamma_out, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  if (!gamma_ramp.Apply(cb, frames[frame_index].texture_pool,
                        source->view, dc_lut_image.view, gamma_out.view, gamma_out.format,
                        gamma_out.width, gamma_out.height, entries)) {
    return source;
  }
  return &gamma_out;
}

void VulkanRenderer::Impl::PresentImpl(const PresentParams& p) {
  if (!device) return;
  if (p.width && p.height) last_host_present=p;
  last_host_present_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  if (p.width && p.height)
    display_buffers[p.frontbuffer_guest_address] = {p.width, p.height};
  // Optional wall-time measurements: CPU sampling misses waits in desktop WSI.
  static FILE* presentation_times = []() -> FILE* {
    const char* path = std::getenv("XN_PRESENT_TIMES");
    if (!path) return nullptr;
    FILE* file = std::fopen(path, "w");
    if (file) {
      std::setvbuf(file, nullptr, _IOLBF, 0);
      std::fprintf(file, "swap,begin_ms,prepare_ms,acquire_ms,blit_badge_ms,submit_present_ms\n");
    }
    return file;
  }();
  using PresentationClock = std::chrono::steady_clock;
  PresentationClock::time_point stamps[6];
  auto stamp = [&](uint32_t i) { if (presentation_times) stamps[i] = PresentationClock::now(); };
  auto record_times = [&] {
    if (!presentation_times) return;
    std::fprintf(presentation_times, "%llu", static_cast<unsigned long long>(guest_swap_number));
    for (uint32_t i = 1; i < 6; ++i)
      std::fprintf(presentation_times, ",%.3f",
          std::chrono::duration<double, std::milli>(stamps[i] - stamps[i - 1]).count());
    std::fputc('\n', presentation_times);
  };
  stamp(0);
  BeginFrameIfNeeded();
  EndRendering();
  stamp(1);
  // Source: the resolved front buffer, else the largest colour target.
  GpuImage* source = nullptr;
  TextureEntry* front_entry = nullptr;
  auto prefer_frontbuffer = [&](const TextureEntry* entry) {
    if (!front_entry) return true;
    const uint64_t area = uint64_t(entry->image.width) * entry->image.height;
    const uint64_t previous_area = uint64_t(front_entry->image.width) * front_entry->image.height;
    // A guest display address can be reused with another format. Keep the
    // largest-view convention, but equal-sized cached views must reflect the
    // latest write to that address rather than the order they were allocated.
    return area > previous_area ||
           (area == previous_area && entry->resolve_serial > front_entry->resolve_serial);
  };
  auto addr_it = resolved_by_addr.find(p.frontbuffer_guest_address);
  if (addr_it != resolved_by_addr.end()) {
    if (std::getenv("XN_DEBUG_PRESENT")) {
      for (const TextureEntry* entry : addr_it->second)
        REXLOG_INFO("xn-vk: frontbuffer candidate addr={:#x} fmt={} {}x{} serial={} display={}",
                    p.frontbuffer_guest_address, entry->key.format, entry->image.width,
                    entry->image.height, entry->resolve_serial, IsDisplayColorFormat(entry->key.format));
    }
    // 1. Look for a standard display color format (k_8_8_8_8 etc.) matching the frontbuffer address.
    for (TextureEntry* entry : addr_it->second) {
      if (IsDisplayColorFormat(entry->key.format)) {
        if (prefer_frontbuffer(entry)) {
          front_entry = entry;
        }
      }
    }
    // 2. Fall back to any non-depth resolve at that address.
    if (!front_entry) {
      for (TextureEntry* entry : addr_it->second) {
        const bool is_depth = (entry->key.format == 22 || entry->key.format == 23);
        if (!is_depth) {
          if (prefer_frontbuffer(entry)) {
            front_entry = entry;
          }
        }
      }
    }
  }
  if (front_entry) {
    source = &front_entry->image;
  } else {
    uint64_t best = 0;
    for (auto& rt : color_targets) {
      const uint64_t area = uint64_t(rt.second.image.width) * rt.second.image.height;
      if (area > best && rt.second.image.layout != VK_IMAGE_LAYOUT_UNDEFINED) {
        best = area;
        source = &ReadableImage(rt.second);
      }
    }
  }
  StoreAllEdramOwners();
  if ((frame_number % 600) == 0) {
    REXLOG_INFO("xn-vk: frame {}: DC_LUT (display gamma ramp) writes so far = {}{}", frame_number,
                dc_lut_writes, dc_lut_valid ? " (table captured)" : "");
    // XN_DEBUG_DUMP_LUT=<path>: all 256 entries as CSV, rewritten each time so
    // the file holds the most recent ramp. Dumped here rather than at shutdown
    // because the renderer's Shutdown() does not run in a headless exit, which
    // is how the first attempt at this produced no file at all.
    if (const char* p = dc_lut_valid ? std::getenv("XN_DEBUG_DUMP_LUT") : nullptr) {
      if (FILE* f = std::fopen(p, "w")) {
        std::fprintf(f, "index,r,g,b\n");
        for (uint32_t i = 0; i < 256; ++i) {
          std::fprintf(f, "%u,%u,%u,%u\n", i, dc_lut[i][0], dc_lut[i][1], dc_lut[i][2]);
        }
        std::fclose(f);
        REXLOG_INFO("xn-vk: DC_LUT ({} writes) dumped to {}", dc_lut_writes, p);
      }
    }
  }
  source = ApplyGammaRamp(source);
  SnapshotPresentedImage(source);
  if (stats.frames < 4 || debug_frame_active || debug_frame_prev ||
      std::getenv("XN_DEBUG_PRESENT")) {
    REXLOG_INFO("xn-vk: present frame {} frontbuffer={:#x} source={} {}x{} layout={} fmt={} serial={} (resolved "
                "targets {}, colour targets {})",
                frame_number, p.frontbuffer_guest_address,
                source ? (front_entry ? "resolved" : "colour-target") : "NONE",
                source ? source->width : 0, source ? source->height : 0,
                source ? int(source->layout) : -1, front_entry ? int(front_entry->key.format) : -1,
                front_entry ? front_entry->resolve_serial : 0, resolved_by_key.size(),
                color_targets.size());
  }
  ++guest_swap_number;
  if ((guest_swap_number % 120) == 0) {
    if (const char* path = std::getenv("XN_VERTEX_PROFILE")) {
      if (FILE* file = std::fopen(path, "w")) {
        std::fprintf(file, "shader,slot,calls,requested_bytes,mirrored_bytes\n");
        for (const auto& [key, shader] : shaders) {
          for (uint32_t slot = 0; slot < shader.vertex_read_profile.size(); ++slot) {
            const auto& profile = shader.vertex_read_profile[slot];
            if (!profile.calls) continue;
            std::fprintf(file, "%016llX,%u,%llu,%llu,%llu\n",
                static_cast<unsigned long long>(shader.shader->ucode_data_hash()), slot,
                static_cast<unsigned long long>(profile.calls),
                static_cast<unsigned long long>(profile.requested_bytes),
                static_cast<unsigned long long>(profile.mirrored_bytes));
          }
        }
        std::fclose(file);
      }
    }
  }
  if (source && !config.frame_dump_dir.empty() && frame_number >= config.frame_dump_start &&
      ((frame_number - config.frame_dump_start) % std::max<uint32_t>(config.frame_dump_every, 1)) ==
          0) {
    // Submits and waits for the frame so far; the swapchain blit below goes
    // into the next command buffer.
    DumpFrame(*source);
    BeginFrameIfNeeded();
  }
  if (!swapchain || !present_limiter.Allow(last_host_present_ns,target_frame_rate)) {
    stamp(2); stamp(3); stamp(4);
    SubmitFrame(false, 0);
    stamp(5); record_times();
    return;
  }
  if (window_pixel_size.load(std::memory_order_relaxed) != swapchain_window_pixel_size)
    swapchain_dirty = true;
  if (swapchain_dirty) {
    vkDeviceWaitIdle(device);
    if (!InitSwapchain()) return;
  }
  FrameContext& f = frames[frame_index];
  uint32_t image_index = 0;
  stamp(2);
  VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f.image_available,
                                     VK_NULL_HANDLE, &image_index);
  stamp(3);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    swapchain_dirty = true;
    SubmitFrame(false, 0);
    return;
  }
  VkImage swap_image = swapchain_images[image_index];
  TransitionRaw(swap_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  if (source) {
    Transition(*source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const auto destination = FitPresentationRect({source->width,source->height},swapchain_extent);
    if (destination.extent.width != swapchain_extent.width ||
        destination.extent.height != swapchain_extent.height) {
      // Fullscreen and freely resized windows keep the game's aspect ratio.
      // Clear the unused margins instead of stretching the HUD and scenery.
      VkClearColorValue black = {{0.f,0.f,0.f,1.f}};
      VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      vkCmdClearColorImage(cb,swap_image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
      TransitionRaw(swap_image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(source->width), int32_t(source->height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {destination.offset.x,destination.offset.y,0};
    blit.dstOffsets[1] = {destination.offset.x+int32_t(destination.extent.width),
                          destination.offset.y+int32_t(destination.extent.height),1};
    vkCmdBlitImage(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   presentation_settings.filter==OutputFilter::Nearest?VK_FILTER_NEAREST:VK_FILTER_LINEAR);
  } else {
    VkClearColorValue black = {{0.f, 0.f, 0.f, 1.f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, swap_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
  }
  const bool host_ui_drawn=DrawHostUi(swap_image,swapchain_extent.width,swapchain_extent.height,
      presentation_settings.filter==OutputFilter::Sharpened?source:nullptr);
  if(host_ui_drawn)
    TransitionRaw(swap_image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  if (source && xn::FpsOverlayEnabled() && swapchain_extent.width >= 200 && swapchain_extent.height >= 50) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f FPS", xn::MeasuredFrameRate());
    const int text_len = static_cast<int>(std::strlen(text));
    const int glyph_scale = 2;
    const int badge_pad_x = 8;
    const int badge_pad_y = 4;
    const int glyph_w = 8 * glyph_scale;
    const int glyph_h = 8 * glyph_scale;
    const int badge_w = text_len * glyph_w + badge_pad_x * 2;
    const int badge_h = glyph_h + badge_pad_y * 2;

    const size_t badge_bytes = static_cast<size_t>(badge_w * badge_h * 4);
    HostBuffer staging;
    if (CreateHostBuffer(badge_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging)) {
      auto pack_color = [this](uint8_t r, uint8_t g, uint8_t b, uint8_t a) -> uint32_t {
        if (swapchain_format == VK_FORMAT_B8G8R8A8_UNORM || swapchain_format == VK_FORMAT_B8G8R8A8_SRGB) {
          return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        } else {
          return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
        }
      };
      const uint32_t bg_color = pack_color(16, 16, 16, 255);
      const uint32_t border_color = pack_color(60, 60, 60, 255);
      const uint32_t text_color = pack_color(0, 255, 128, 255);

      uint32_t* pixels = reinterpret_cast<uint32_t*>(staging.mapped);
      for (int y = 0; y < badge_h; ++y) {
        for (int x = 0; x < badge_w; ++x) {
          if (x == 0 || x == badge_w - 1 || y == 0 || y == badge_h - 1) {
            pixels[y * badge_w + x] = border_color;
          } else {
            pixels[y * badge_w + x] = bg_color;
          }
        }
      }

      for (int i = 0; i < text_len; ++i) {
        const uint8_t* glyph = GetFpsGlyph(text[i]);
        int char_x0 = badge_pad_x + i * glyph_w;
        int char_y0 = badge_pad_y;
        for (int row = 0; row < 8; ++row) {
          uint8_t row_bits = glyph[row];
          for (int col = 0; col < 8; ++col) {
            if ((row_bits >> (7 - col)) & 1) {
              for (int sy = 0; sy < glyph_scale; ++sy) {
                for (int sx = 0; sx < glyph_scale; ++sx) {
                  int px = char_x0 + col * glyph_scale + sx;
                  int py = char_y0 + row * glyph_scale + sy;
                  if (px < badge_w && py < badge_h) {
                    pixels[py * badge_w + px] = text_color;
                  }
                }
              }
            }
          }
        }
      }

      int dst_x = int(swapchain_extent.width) - badge_w - 16;
      int dst_y = 16;
      if (dst_x < 0) dst_x = 0;

      VkBufferImageCopy region = {};
      region.bufferOffset = 0;
      region.bufferRowLength = badge_w;
      region.bufferImageHeight = badge_h;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageOffset = {dst_x, dst_y, 0};
      region.imageExtent = {uint32_t(badge_w), uint32_t(badge_h), 1};

      vkCmdCopyBufferToImage(cb, staging.buffer, swap_image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      f.garbage.push_back(staging);
    }
  }
  TransitionRaw(swap_image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  stamp(4);
  SubmitFrame(true, image_index);
  presented_frames.fetch_add(1,std::memory_order_relaxed);
  stamp(5); record_times();
}

}  // namespace xn::gfx

namespace xn::gfx {
bool VulkanRenderer::SynchronizeTextureToGuest(uint32_t address, uint32_t width,
                                               uint32_t height, uint32_t format) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  Impl& im = *impl_;
  if (!im.device || !im.guest_base) return false;
  const auto found = im.resolved_by_addr.find(address);
  if (found == im.resolved_by_addr.end() || found->second.empty()) return false;
  // Only the latest write owns this address. An older, matching allocation
  // must not overwrite a newer image with different dimensions or format.
  TextureEntry& entry = *found->second.back();
  TexFormatInfo fi;
  if (!AreFormatsEquivalent(entry.key.format, format) ||
      entry.image.width != width || entry.image.height != height ||
      !GetTexFormatInfo(xenos::TextureFormat(entry.key.format), fi) ||
      fi.block_w != 1 || fi.block_h != 1 || fi.expand_to_rgba8 ||
      entry.image.format != fi.vk || entry.key.endian > 3 ||
      entry.resolved_regions.empty()) return false;
  if (entry.guest_sync_serial == entry.resolve_serial) return true;

  // Copy the native storage, retaining packed10-bit values and float precision.
  // Only the view's channel mapping and the guest's endian/layout are applied.
  std::array<uint32_t, 4> bits{};
  bool float_components = false;
  switch (fi.vk) {
    case VK_FORMAT_R8_UNORM: bits = {8,0,0,0}; break;
    case VK_FORMAT_R8G8_UNORM: bits = {8,8,0,0}; break;
    case VK_FORMAT_R8G8B8A8_UNORM: bits = {8,8,8,8}; break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: bits = {10,10,10,2}; break;
    case VK_FORMAT_R16_SFLOAT: float_components = true; bits = {16,0,0,0}; break;
    case VK_FORMAT_R16G16_SFLOAT: float_components = true; bits = {16,16,0,0}; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: float_components = true; bits = {16,16,16,16}; break;
    case VK_FORMAT_R16G16_UNORM: bits = {16,16,0,0}; break;
    case VK_FORMAT_R16G16B16A16_UNORM: bits = {16,16,16,16}; break;
    case VK_FORMAT_R32_SFLOAT: float_components = true; bits = {32,0,0,0}; break;
    case VK_FORMAT_R32G32_SFLOAT: float_components = true; bits = {32,32,0,0}; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: float_components = true; bits = {32,32,32,32}; break;
    default: return false;
  }
  std::array<uint32_t, 4> bit_offsets{}, components{};
  for (uint32_t c = 0; c < 4; ++c) {
    if (c) bit_offsets[c] = bit_offsets[c - 1] + bits[c - 1];
    components[c] = (entry.host_swizzle >> (c * 3)) & 7;
    if (bits[c] && components[c] < 4 && bits[components[c]] != bits[c]) return false;
  }
  const uint32_t pitch = std::max(entry.key.pitch, width);
  const uint32_t log_bpp = std::countr_zero(fi.bytes_per_block);
  const uint32_t endian_xor[] = {0, 1, 3, 2};
  const uint32_t byte_xor = endian_xor[entry.key.endian];
  uint64_t footprint = entry.key.tiled
      ? untile::TiledExtent2D(width, height, (pitch + 31) & ~31u, log_bpp)
      : (uint64_t(height - 1) * pitch + width) * fi.bytes_per_block;
  if (byte_xor) footprint = (footprint + 3) & ~uint64_t(3);
  if (uint64_t(address) + footprint > im.guest_size) return false;
  std::vector<uint8_t> pixels;
  if (!im.ReadbackImage(entry.image, pixels, nullptr, nullptr, fi.bytes_per_block)) return false;
  if (pixels.size() != uint64_t(width) * height * fi.bytes_per_block) return false;

  GuestWriteScope writable(im.guest_base + address, size_t(footprint));
  for (const auto& region : entry.resolved_regions) {
    const uint32_t x0 = uint32_t(std::max(0, region.offset.x));
    const uint32_t y0 = uint32_t(std::max(0, region.offset.y));
    const uint32_t x1 = std::min(width, x0 + region.extent.width);
    const uint32_t y1 = std::min(height, y0 + region.extent.height);
    for (uint32_t y = y0; y < y1; ++y) for (uint32_t x = x0; x < x1; ++x) {
      const uint8_t* pixel = pixels.data() + (uint64_t(y) * width + x) * fi.bytes_per_block;
      std::array<uint8_t, 16> texel{};
      for (uint32_t c = 0; c < 4; ++c) {
        if (!bits[c]) continue;
        const uint64_t mask = (uint64_t(1) << bits[c]) - 1;
        const uint32_t component = components[c];
        uint64_t value = 0;
        if (component < 4) {
          const uint32_t shift = bit_offsets[component] & 7;
          const uint32_t count = (shift + bits[c] + 7) / 8;
          std::memcpy(&value, pixel + bit_offsets[component] / 8, count);
          value = (value >> shift) & mask;
        } else if (component != xenos::XE_GPU_TEXTURE_SWIZZLE_0) {
          value = float_components ? (bits[c] == 16 ? 0x3C00u : 0x3F800000u) : mask;
        }
        const uint32_t shift = bit_offsets[c] & 7;
        const uint32_t count = (shift + bits[c] + 7) / 8;
        uint64_t packed = 0;
        std::memcpy(&packed, texel.data() + bit_offsets[c] / 8, count);
        packed = (packed & ~(mask << shift)) | (value << shift);
        std::memcpy(texel.data() + bit_offsets[c] / 8, &packed, count);
      }
      const uint64_t offset = entry.key.tiled ? untile::TiledOffset2DColumn(x, y, log_bpp,
          untile::TiledOffset2DRow(y, (pitch + 31) & ~31u, log_bpp))
          : (uint64_t(y) * pitch + x) * fi.bytes_per_block;
      for (uint32_t b = 0; b < fi.bytes_per_block; ++b)
        im.guest_base[address + ((offset + b) ^ byte_xor)] = texel[b];
    }
  }
  for (uint64_t page = address / Impl::kMirrorPageSize;
       page < (uint64_t(address) + footprint + Impl::kMirrorPageSize - 1) / Impl::kMirrorPageSize;
       ++page) im.mirrored_pages.reset(page);
  entry.guest_sync_serial = entry.resolve_serial;
  REXLOG_INFO("xn-vk: CPU texture synchronized {:#x} {}x{} format={} serial={}",
              address, width, height, format, entry.resolve_serial);
  return true;
}

bool VulkanRenderer::DebugDrawTriangle(const GpuRegisterFile& gpu_regs) {
  std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
  if (!impl_->device) return false;
  impl_->BeginFrameIfNeeded();
  if (impl_->cache_texture_dirty)
    impl_->MarkTextureFetchChanges(gpu_regs.values + 0x4800);
  if (impl_->cache_dirty_preparation) impl_->preparation_dirty.InvalidateAll();
  std::memcpy(impl_->regs.values, gpu_regs.values, sizeof(impl_->regs.values));
  impl_->ring_registers_current = false;
  return impl_->DebugTriangleIntoCurrentTarget();
}

bool VulkanRenderer::Impl::DebugTriangleIntoCurrentTarget() {
  InvalidateDrawState();
  Impl& im = *this;
  auto& vs = im.debug_vertex_module;
  auto& fs = im.debug_pixel_module;
  auto make = [&](const uint32_t* code, size_t words) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = words * 4;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(im.device, &ci, nullptr, &m);
    return m;
  };
  if (!vs) vs = make(kDebugTriangleVert, sizeof(kDebugTriangleVert) / 4);
  if (!fs) fs = make(kDebugTriangleFrag, sizeof(kDebugTriangleFrag) / 4);
  const auto rb_surface_info = im.regs.Get<reg::RB_SURFACE_INFO>();
  const auto rb_color_info = im.regs.Get<reg::RB_COLOR_INFO>();
  const auto rb_depth_info = im.regs.Get<reg::RB_DEPTH_INFO>();
  RenderTargetImage* color = im.GetColorTarget(im.RenderTargetBase(rb_color_info.value),
                                               rb_color_info.color_format,
                                               rb_surface_info.surface_pitch);
  RenderTargetImage* depth =
      im.GetDepthTarget(im.RenderTargetBase(rb_depth_info.value),
                     color->image.width, color->image.height,
                     color->samples);
  PipelineKey key;
  key.vs = vs;
  key.ps = fs;
  key.color_formats[0] = color->image.format;
  key.depth_format = depth->image.format;
  key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  key.blend_controls[0] = 0x00010001;
  key.color_write_mask = 0xF;
  key.color_attachment_count = 1;
  key.samples = uint32_t(color->samples);
  key.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  VkPipelineLayout layout = im.GetPipelineLayout(0, 0, 0, 0);
  VkPipeline pipeline = im.GetPipeline(key, layout);
  if (!pipeline) return false;
  im.EnsureRendering(color, depth);
  vkCmdBindPipeline(im.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  // XN_DEBUG_BIG_VIEWPORT: 8192x8192 viewport, like the guest's window when
  // PA_CL_VTE_CNTL disables the viewport transform.
  const bool big = std::getenv("XN_DEBUG_BIG_VIEWPORT") != nullptr;
  VkViewport viewport = {0.f, 0.f, big ? 8192.f : float(color->image.width),
                         big ? 8192.f : float(color->image.height), 0.f, 1.f};
  VkRect2D scissor = {{0, 0}, {color->image.width, color->image.height}};
  vkCmdSetViewport(im.cb, 0, 1, &viewport);
  vkCmdSetScissor(im.cb, 0, 1, &scissor);
  const float bc[4] = {0, 0, 0, 0};
  vkCmdSetBlendConstants(im.cb, bc);
  vkCmdSetStencilReference(im.cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vkCmdSetDepthBias(im.cb, 0.0f, 0.0f, 0.0f);
  vkCmdDraw(im.cb, 3, 1, 0, 0);
  return true;
}
}  // namespace xn::gfx
