#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <rex/graphics/util/memexport_copy.h>

using rex::graphics::draw_util::GetMemExportCopyOffset;

TEST_CASE("Memexport copy ranges retain pool and draw offsets", "[graphics][memexport]") {
  auto range = GetMemExportCopyOffset(100, 384, 1000.0f, 8257536);
  REQUIRE(range);
  CHECK(range->first == 1100 * 64);
  CHECK(range->second == 384 * 64);
  // A source starting later in its pool can have a negative destination delta.
  range = GetMemExportCopyOffset(1000, 2, -999.0f, 24);
  REQUIRE(range);
  CHECK(range->first == 64);
  CHECK(range->second == 128);
}

TEST_CASE("Memexport copy bounds reject unsupported arithmetic", "[graphics][memexport]") {
  CHECK_FALSE(GetMemExportCopyOffset(0, 0, 0, 8));
  CHECK_FALSE(GetMemExportCopyOffset(0, 1, -1, 8));
  CHECK_FALSE(GetMemExportCopyOffset(0, 1, 0.5f, 8));
  CHECK_FALSE(GetMemExportCopyOffset(0, 1, std::numeric_limits<float>::infinity(), 8));
  CHECK_FALSE(GetMemExportCopyOffset(0, 1, std::numeric_limits<float>::quiet_NaN(), 8));
  CHECK_FALSE(GetMemExportCopyOffset(0, 2, 0, 15));
  CHECK_FALSE(GetMemExportCopyOffset(UINT32_MAX, 2, 0, UINT32_MAX));
  CHECK_FALSE(GetMemExportCopyOffset(0, (1u << 20) + 1, 0, UINT32_MAX));
  CHECK_FALSE(GetMemExportCopyOffset(0, 1, float(1u << 25), UINT32_MAX));
  auto range = GetMemExportCopyOffset(0, 2, 0, 16);
  REQUIRE(range);
  CHECK(range->second == 128);
}
