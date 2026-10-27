#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include <rex/graphics/registers.h>

namespace xn::gfx {

// Tracks changes to the register inputs of DrawImpl's derived preparation.
// Resource identity, shader identity, primitive type and the half-pixel-offset
// setting are separate cache keys; none can be inferred from register writes.
class DrawPreparationDirtyState {
 public:
  enum : uint32_t {
    kShader = 1u,
    kViewport = 2u,
    kSystem = 4u,
    kPipeline = 8u,
    kBoolLoop = 16u,
    kFetch = 32u,
    kSystemDynamic = 64u,
    kAll = kShader | kViewport | kSystem | kPipeline | kBoolLoop | kFetch |
        kSystemDynamic,
  };

  uint32_t pending = kAll;
  // [shader stage][64-bit word], one bit for each float4 constant.
  std::array<std::array<uint64_t, 4>, 2> float_dirty = {{
      {{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}},
      {{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}},
  }};

  void InvalidateAll() {
    pending = kAll;
    for (auto& stage : float_dirty) stage.fill(UINT64_MAX);
  }

  static constexpr uint32_t RegisterMask(uint32_t index) {
    if (index >= 0x4800 && index < 0x48C0) return kFetch | kSystemDynamic;
    if (index >= 0x4900 && index < 0x4928) return kBoolLoop;
    if (index >= rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X && index < rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X + 24)
      return kSystem;
    if (index >= 0x2000 && index < 0x2800)
      return kContextMaskByIndex[index - 0x2000];
    return 0;
  }

  void MarkWrite(uint32_t index, uint32_t old_value, uint32_t value) {
    if (old_value == value) return;
    pending |= RegisterMask(index);
    if (index >= 0x4000 && index < 0x4800) {
      const uint32_t stage = (index - 0x4000) / 0x400;
      const uint32_t constant = ((index - 0x4000) % 0x400) / 4;
      float_dirty[stage][constant / 64] |= UINT64_C(1) << (constant % 64);
    }
  }

  // Conservative for batched writes: no old values are available here.
  void MarkRange(uint32_t first, uint32_t count) {
    if (!count) return;
    const uint64_t end = std::min<uint64_t>(uint64_t(first) + count, 0x5003);
    if (uint64_t(first) >= end) return;
    if (Intersects(first, end, 0x2000, 0x2800)) {
      const uint32_t begin = std::max(first, 0x2000u);
      const uint32_t limit = uint32_t(std::min<uint64_t>(end, 0x2800));
      if (limit - begin <= kContextDependencies.size()) {
        for (uint32_t index = begin; index < limit; ++index)
          pending |= kContextMaskByIndex[index - 0x2000];
      } else {
        for (const auto& dependency : kContextDependencies)
          if (dependency.index >= begin && dependency.index < limit)
            pending |= dependency.mask;
      }
    }
    if (Intersects(first, end, rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X,
                   rex::graphics::XE_GPU_REG_PA_CL_UCP_0_X + 24)) pending |= kSystem;
    if (Intersects(first, end, 0x4800, 0x48C0)) pending |= kFetch | kSystemDynamic;
    if (Intersects(first, end, 0x4900, 0x4928)) pending |= kBoolLoop;
    MarkFloatRange(0, first, end);
    MarkFloatRange(1, first, end);
  }

 private:
  struct Dependency {
    uint32_t index;
    uint32_t mask;
  };
  // Only context words read by shader selection/translation, viewport and
  // scissor helpers, system constants, or pipeline-key construction belong
  // here. VGT_DRAW_INITIATOR is synthesized in DrawImpl; its primitive type
  // is an explicit cache key. VGT_HOS_CNTL is not read by these helpers.
  inline static constexpr auto kContextDependencies = std::to_array<Dependency>({
      {rex::graphics::XE_GPU_REG_RB_SURFACE_INFO, kShader | kViewport | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_RB_COLOR_INFO, kShader | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_RB_COLOR1_INFO, kShader | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_RB_COLOR2_INFO, kShader | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_RB_COLOR3_INFO, kShader | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_RB_DEPTH_INFO, kViewport | kPipeline},
      {rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL, kViewport},
      {rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR, kViewport},
      {rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET, kViewport | kSystem},
      {rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL, kViewport},
      {rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, kViewport},
      {rex::graphics::XE_GPU_REG_VGT_MAX_VTX_INDX, kSystemDynamic},
      {rex::graphics::XE_GPU_REG_VGT_MIN_VTX_INDX, kSystemDynamic},
      {rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET, kSystemDynamic},
      {rex::graphics::XE_GPU_REG_RB_COLOR_MASK, kShader | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_RB_STENCILREFMASK, kPipeline},
      {rex::graphics::XE_GPU_REG_RB_ALPHA_REF, kSystem},
      {rex::graphics::XE_GPU_REG_PA_CL_VPORT_XSCALE, kViewport | kSystem},
      {rex::graphics::XE_GPU_REG_PA_CL_VPORT_XOFFSET, kViewport | kSystem},
      {rex::graphics::XE_GPU_REG_PA_CL_VPORT_YSCALE, kViewport | kSystem},
      {rex::graphics::XE_GPU_REG_PA_CL_VPORT_YOFFSET, kViewport | kSystem},
      {rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE, kViewport | kSystem},
      {rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET, kViewport | kSystem},
      {rex::graphics::XE_GPU_REG_SQ_PROGRAM_CNTL, kShader | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_SQ_CONTEXT_MISC, kShader | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_SQ_INTERPOLATOR_CNTL, kShader | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL, kViewport | kPipeline},
      {rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0, kPipeline},
      {rex::graphics::XE_GPU_REG_RB_BLENDCONTROL1, kPipeline},
      {rex::graphics::XE_GPU_REG_RB_BLENDCONTROL2, kPipeline},
      {rex::graphics::XE_GPU_REG_RB_BLENDCONTROL3, kPipeline},
      {rex::graphics::XE_GPU_REG_RB_COLORCONTROL, kShader | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL, kShader | kViewport | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_PA_SU_SC_MODE_CNTL, kViewport | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL, kViewport | kSystem},
      {rex::graphics::XE_GPU_REG_RB_MODECONTROL, kShader | kViewport | kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_PA_SU_POINT_SIZE, kSystem},
      {rex::graphics::XE_GPU_REG_PA_SU_POINT_MINMAX, kSystem},
      {rex::graphics::XE_GPU_REG_VGT_OUTPUT_PATH_CNTL, kSystem | kPipeline},
      {rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL, kViewport | kSystem},
  });
  inline static constexpr auto kContextMaskByIndex = [] {
    std::array<uint8_t, 0x800> masks{};
    for (const auto& dependency : kContextDependencies)
      masks[dependency.index - 0x2000] = uint8_t(dependency.mask);
    return masks;
  }();

  static constexpr bool Intersects(uint32_t first, uint64_t end, uint32_t begin,
                                   uint32_t limit) {
    return first < limit && end > begin;
  }

  void MarkFloatRange(uint32_t stage, uint32_t first, uint64_t end) {
    const uint32_t base = 0x4000 + stage * 0x400;
    if (!Intersects(first, end, base, base + 0x400)) return;
    const uint32_t begin_constant = (std::max(first, base) - base) / 4;
    const uint32_t end_constant =
        (uint32_t(std::min<uint64_t>(end, base + 0x400)) - 1 - base) / 4;
    for (uint32_t word = begin_constant / 64; word <= end_constant / 64; ++word) {
      const uint32_t low = word == begin_constant / 64 ? begin_constant % 64 : 0;
      const uint32_t high = word == end_constant / 64 ? end_constant % 64 : 63;
      float_dirty[stage][word] |= (UINT64_MAX << low) & (UINT64_MAX >> (63 - high));
    }
  }
};

}  // namespace xn::gfx
