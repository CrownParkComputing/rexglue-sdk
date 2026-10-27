#include "runtime/gfx/vulkan/draw_preparation_dirty.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {
using namespace rex::graphics;
using Dirty = xn::gfx::DrawPreparationDirtyState;

void Check(bool result, int line) {
  if (!result) throw std::runtime_error("draw preparation dirty probe line " +
                                        std::to_string(line));
}
#define CHECK(condition) Check(bool(condition), __LINE__)

void Reset(Dirty& dirty) {
  dirty.pending = 0;
  for (auto& stage : dirty.float_dirty) stage.fill(0);
}

void TestRegisterDependencies() {
  constexpr std::array<std::array<uint32_t, 2>, 24> cases = {{
      {{XE_GPU_REG_SQ_PROGRAM_CNTL, Dirty::kShader | Dirty::kSystem | Dirty::kPipeline}},
      {{XE_GPU_REG_SQ_CONTEXT_MISC, Dirty::kShader | Dirty::kSystem | Dirty::kPipeline}},
      {{XE_GPU_REG_SQ_INTERPOLATOR_CNTL, Dirty::kShader | Dirty::kSystem | Dirty::kPipeline}},
      {{XE_GPU_REG_RB_MODECONTROL, Dirty::kShader | Dirty::kViewport | Dirty::kSystem | Dirty::kPipeline}},
      {{XE_GPU_REG_RB_SURFACE_INFO, Dirty::kShader | Dirty::kViewport | Dirty::kSystem | Dirty::kPipeline}},
      {{XE_GPU_REG_RB_COLOR3_INFO, Dirty::kShader | Dirty::kSystem | Dirty::kPipeline}},
      {{XE_GPU_REG_RB_COLOR_MASK, Dirty::kShader | Dirty::kSystem | Dirty::kPipeline}},
      {{XE_GPU_REG_RB_COLORCONTROL, Dirty::kShader | Dirty::kSystem | Dirty::kPipeline}},
      {{XE_GPU_REG_PA_CL_CLIP_CNTL, Dirty::kShader | Dirty::kViewport | Dirty::kSystem | Dirty::kPipeline}},
      {{XE_GPU_REG_PA_CL_VTE_CNTL, Dirty::kViewport | Dirty::kSystem}},
      {{XE_GPU_REG_PA_CL_VPORT_ZOFFSET, Dirty::kViewport | Dirty::kSystem}},
      {{XE_GPU_REG_PA_SC_WINDOW_OFFSET, Dirty::kViewport | Dirty::kSystem}},
      {{XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL, Dirty::kViewport}},
      {{XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, Dirty::kViewport}},
      {{XE_GPU_REG_PA_SU_VTX_CNTL, Dirty::kViewport | Dirty::kSystem}},
      {{XE_GPU_REG_RB_DEPTHCONTROL, Dirty::kViewport | Dirty::kPipeline}},
      {{XE_GPU_REG_RB_BLENDCONTROL2, Dirty::kPipeline}},
      {{XE_GPU_REG_RB_STENCILREFMASK, Dirty::kPipeline}},
      {{XE_GPU_REG_PA_SU_SC_MODE_CNTL, Dirty::kViewport | Dirty::kSystem | Dirty::kPipeline}},
      {{XE_GPU_REG_PA_SU_POINT_SIZE, Dirty::kSystem}},
      {{XE_GPU_REG_PA_CL_UCP_5_W, Dirty::kSystem}},
      {{XE_GPU_REG_VGT_OUTPUT_PATH_CNTL, Dirty::kSystem | Dirty::kPipeline}},
      {{XE_GPU_REG_VGT_MAX_VTX_INDX, Dirty::kSystemDynamic}},
      {{XE_GPU_REG_RB_ALPHA_REF, Dirty::kSystem}},
  }};
  for (const auto& item : cases) CHECK(Dirty::RegisterMask(item[0]) == item[1]);
  CHECK(Dirty::RegisterMask(XE_GPU_REG_VGT_DRAW_INITIATOR) == 0);
  CHECK(Dirty::RegisterMask(XE_GPU_REG_VGT_HOS_CNTL) == 0);
  CHECK(Dirty::RegisterMask(XE_GPU_REG_RB_COPY_CONTROL) == 0);
  CHECK(Dirty::RegisterMask(0x47FF) == 0);  // Floats use their own bitsets.
  CHECK(Dirty::RegisterMask(0x4800) == (Dirty::kFetch | Dirty::kSystemDynamic));
  CHECK(Dirty::RegisterMask(0x48BF) == (Dirty::kFetch | Dirty::kSystemDynamic));
  CHECK(Dirty::RegisterMask(0x48C0) == 0);
  CHECK(Dirty::RegisterMask(0x4900) == Dirty::kBoolLoop);
  CHECK(Dirty::RegisterMask(0x4927) == Dirty::kBoolLoop);
  CHECK(Dirty::RegisterMask(0x4928) == 0);
}

void TestWritesAndRanges() {
  Dirty dirty;
  CHECK(dirty.pending == Dirty::kAll);
  for (const auto& stage : dirty.float_dirty)
    for (uint64_t bits : stage) CHECK(bits == UINT64_MAX);
  Reset(dirty);
  dirty.MarkWrite(XE_GPU_REG_PA_CL_VTE_CNTL, 7, 7);
  dirty.MarkWrite(0x4000, 7, 7);
  CHECK(dirty.pending == 0 && dirty.float_dirty[0][0] == 0);
  dirty.MarkWrite(XE_GPU_REG_PA_CL_VTE_CNTL, 7, 8);
  CHECK(dirty.pending == (Dirty::kViewport | Dirty::kSystem));
  Reset(dirty);
  dirty.MarkWrite(XE_GPU_REG_VGT_INDX_OFFSET, 0, 3);
  CHECK(dirty.pending == Dirty::kSystemDynamic);
  Reset(dirty);
  dirty.MarkRange(XE_GPU_REG_VGT_MIN_VTX_INDX, 2);
  CHECK(dirty.pending == Dirty::kSystemDynamic);
  Reset(dirty);
  dirty.MarkRange(XE_GPU_REG_SQ_PROGRAM_CNTL, 3);
  CHECK(dirty.pending == (Dirty::kShader | Dirty::kSystem | Dirty::kPipeline));
  Reset(dirty);
  dirty.MarkRange(XE_GPU_REG_RB_COPY_CONTROL, 1);
  CHECK(dirty.pending == 0);
  dirty.MarkRange(0x2000, 0x800);  // Wide context ranges scan the dependency list.
  CHECK(dirty.pending ==
        (Dirty::kShader | Dirty::kViewport | Dirty::kSystem | Dirty::kPipeline |
         Dirty::kSystemDynamic));
  Reset(dirty);
  dirty.MarkRange(0x2000, 0);
  CHECK(dirty.pending == 0);
  dirty.MarkRange(UINT32_MAX - 1, 100);
  CHECK(dirty.pending == 0);
  dirty.MarkRange(0x47FF, UINT32_MAX);
  CHECK(dirty.float_dirty[1][3] == (UINT64_C(1) << 63));
  CHECK(dirty.pending == (Dirty::kFetch | Dirty::kSystemDynamic | Dirty::kBoolLoop));
}

void TestFloatBoundaries() {
  Dirty dirty;
  Reset(dirty);
  for (uint32_t constant : {31u, 32u, 63u, 64u, 255u})
    dirty.MarkWrite(0x4000 + constant * 4 + 3, 0, 1);
  CHECK(dirty.pending == 0);
  CHECK(dirty.float_dirty[0][0] == ((UINT64_C(1) << 31) | (UINT64_C(1) << 32) |
                                     (UINT64_C(1) << 63)));
  CHECK(dirty.float_dirty[0][1] == 1);
  CHECK(dirty.float_dirty[0][3] == (UINT64_C(1) << 63));
  CHECK(dirty.float_dirty[1][0] == 0);
  Reset(dirty);
  dirty.MarkRange(0x407F, 2);  // Last dword of c31 and first dword of c32.
  CHECK(dirty.float_dirty[0][0] == ((UINT64_C(1) << 31) | (UINT64_C(1) << 32)));
  Reset(dirty);
  dirty.MarkRange(0x43FF, 2);  // VS c255 / PS c0 bank boundary.
  CHECK(dirty.float_dirty[0][3] == (UINT64_C(1) << 63));
  CHECK(dirty.float_dirty[1][0] == 1);
  Reset(dirty);
  dirty.MarkRange(0x4000, 0x800);
  for (const auto& stage : dirty.float_dirty)
    for (uint64_t bits : stage) CHECK(bits == UINT64_MAX);
}

void TestFetchIsIndependent() {
  Dirty dirty;
  Reset(dirty);
  dirty.MarkWrite(0x4800, 0, 1);
  CHECK(dirty.pending == (Dirty::kFetch | Dirty::kSystemDynamic));
  dirty.pending &= ~Dirty::kFetch;  // Descriptor preparation consumes its own state.
  CHECK(dirty.pending == Dirty::kSystemDynamic);
  dirty.MarkRange(0x48BF, 2);
  CHECK(dirty.pending == (Dirty::kFetch | Dirty::kSystemDynamic));
  dirty.InvalidateAll();
  CHECK(dirty.pending == Dirty::kAll);
  for (const auto& stage : dirty.float_dirty)
    for (uint64_t bits : stage) CHECK(bits == UINT64_MAX);
}
}  // namespace

int main() {
  TestRegisterDependencies();
  TestWritesAndRanges();
  TestFloatBoundaries();
  TestFetchIsIndependent();
}
