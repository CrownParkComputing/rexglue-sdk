#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/util/resolve_readback.h>

using rex::graphics::util::IsSplitSecondCompositorResolve;

TEST_CASE("Split Second readback selects complete compositor mip surfaces", "[graphics][readback]") {
  for (uint32_t pitch = 32; pitch <= 1024; pitch *= 2) {
    CHECK(IsSplitSecondCompositorResolve(pitch, pitch * 2, 6, pitch * pitch * 8));
  }
}

TEST_CASE("Split Second readback excludes frame targets and partial shadows", "[graphics][readback]") {
  CHECK_FALSE(IsSplitSecondCompositorResolve(1024, 2048, 6, 4194304));
  CHECK_FALSE(IsSplitSecondCompositorResolve(1024, 3072, 6, 4194304));
  CHECK_FALSE(IsSplitSecondCompositorResolve(1024, 1024, 6, 4194304));
  CHECK_FALSE(IsSplitSecondCompositorResolve(1280, 672, 6, 3440640));
  CHECK_FALSE(IsSplitSecondCompositorResolve(32, 16, 36, 4096));
  CHECK_FALSE(IsSplitSecondCompositorResolve(512, 1024, 32, 2097152));
  CHECK_FALSE(IsSplitSecondCompositorResolve(48, 96, 6, 18432));
  CHECK_FALSE(IsSplitSecondCompositorResolve(0, 0, 6, 0));
  CHECK_FALSE(IsSplitSecondCompositorResolve(2048, 4096, 6, 33554432));
}
