/**
 ******************************************************************************
 * ReXGlue                                                                    *
 ******************************************************************************
 * Tests for the native backend's EDRAM phase model.
 *
 * Each of these encodes a rule that was established empirically and is easy to
 * regress: get phase ownership wrong and an imposter atlas accumulates the
 * draws of the one before it; get display selection wrong and the frame is
 * blank or double-composited.
 ******************************************************************************
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "graphics/native/phase_model.h"

using namespace rex::graphics::native;

// ---------------------------------------------------------------------------
// Address keying
// ---------------------------------------------------------------------------

TEST_CASE("resolve keys ignore the low 12 bits", "[phase_model]") {
  CHECK(ResolvedTargetKey(0x16FCF000u) == 0x16FCF000u);
  CHECK(ResolvedTargetKey(0x16FCF123u) == 0x16FCF000u);
}

TEST_CASE("resolve keys drop bits above the EDRAM-addressable range",
          "[phase_model]") {
  // A texture fetch and a resolve destination must agree after masking or the
  // alias silently misses and the draw samples guest memory the backend never
  // wrote - which reads as black.
  CHECK(ResolvedTargetKey(0xF16FCF000u & 0xFFFFFFFFu) == ResolvedTargetKey(0x16FCF000u));
  CHECK(ResolvedTargetKey(0x1F4FE000u) == 0x1F4FE000u);
}

// ---------------------------------------------------------------------------
// Phase ownership
// ---------------------------------------------------------------------------

TEST_CASE("a base's first resolve of the frame owns everything before it",
          "[phase_model]") {
  CHECK(PhaseFirstDraw(false, 0, false, 0) == 0u);
}

TEST_CASE("a re-resolved base owns only draws since its last resolve",
          "[phase_model]") {
  // Base 832 serves four imposter atlases in sequence. Without this, atlas 2
  // would contain atlas 1's draws as well.
  CHECK(PhaseFirstDraw(false, 0, true, 40) == 40u);
}

TEST_CASE("a clear later than the last resolve wins", "[phase_model]") {
  CHECK(PhaseFirstDraw(true, 90, true, 40) == 90u);
}

TEST_CASE("a resolve later than the last clear wins", "[phase_model]") {
  // Resolves do NOT clear EDRAM, so a post-process chain that resolves the same
  // base twice is progressive - but ownership still starts at the later mark.
  CHECK(PhaseFirstDraw(true, 40, true, 119) == 119u);
}

// ---------------------------------------------------------------------------
// Draw membership
// ---------------------------------------------------------------------------

TEST_CASE("an unfiltered range takes every draw", "[phase_model]") {
  CHECK(DrawInRange(/*color_base=*/468, /*depth_only=*/false, kAnyBase));
  CHECK(DrawInRange(/*color_base=*/832, /*depth_only=*/false, kAnyBase));
}

TEST_CASE("a filtered range takes only its own base", "[phase_model]") {
  CHECK(DrawInRange(468, false, 468));
  CHECK_FALSE(DrawInRange(832, false, 468));
}

TEST_CASE("depth-only draws bypass the base filter", "[phase_model]") {
  // Their RB_COLOR_INFO is stale, so filtering on it would drop the depth
  // pre-pass out of the very phase whose depth it establishes - which is what
  // left 3D geometry failing its depth test against a cleared buffer.
  CHECK(DrawInRange(/*color_base=*/0, /*depth_only=*/true, 468));
  CHECK(DrawInRange(/*color_base=*/832, /*depth_only=*/true, 468));
}

// ---------------------------------------------------------------------------
// Display selection
// ---------------------------------------------------------------------------

TEST_CASE("a frame with no resolves replays everything", "[phase_model]") {
  // The Geometry Wars baseline - no render-to-texture at all.
  const auto ranges = SelectDisplayRanges({}, /*fb_key=*/0x1F4FE000u,
                                          /*deferred_count=*/120, /*phase_first_draw=*/0);
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0] == DisplayRange{0, 120, kAnyBase});
}

TEST_CASE("only frontbuffer phases reach the screen", "[phase_model]") {
  const uint32_t fb = 0x1F4FE000u;
  std::vector<PhaseSpan> phases{
      {/*src_base=*/468, /*first=*/0, /*end=*/122, /*dest=*/0x16FCF000u},  // offscreen scene
      {/*src_base=*/0, /*first=*/122, /*end=*/134, /*dest=*/fb},           // composite
  };
  const auto ranges = SelectDisplayRanges(phases, fb, /*deferred_count=*/134,
                                          /*phase_first_draw=*/134);
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0] == DisplayRange{122, 134, 0});
}

TEST_CASE("trailing draws after the last resolve still display",
          "[phase_model]") {
  const uint32_t fb = 0x1F4FE000u;
  std::vector<PhaseSpan> phases{{0, 0, 10, fb}};
  const auto ranges = SelectDisplayRanges(phases, fb, /*deferred_count=*/14,
                                          /*phase_first_draw=*/10);
  REQUIRE(ranges.size() == 2);
  CHECK(ranges[0] == DisplayRange{0, 10, 0});
  CHECK(ranges[1] == DisplayRange{10, 14, kAnyBase});
}

TEST_CASE("no frontbuffer match falls back to replaying everything",
          "[phase_model]") {
  // Note this DISCARDS the trailing range too - the fallback is whole-frame.
  std::vector<PhaseSpan> phases{{468, 0, 122, 0x16FCF000u}};
  const auto ranges = SelectDisplayRanges(phases, /*fb_key=*/0x1F4FE000u,
                                          /*deferred_count=*/130, /*phase_first_draw=*/122);
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0] == DisplayRange{0, 130, kAnyBase});
}

TEST_CASE("an empty frontbuffer phase does not count as a match",
          "[phase_model]") {
  const uint32_t fb = 0x1F4FE000u;
  std::vector<PhaseSpan> phases{{0, 5, 5, fb}};
  const auto ranges = SelectDisplayRanges(phases, fb, /*deferred_count=*/5,
                                          /*phase_first_draw=*/5);
  REQUIRE(ranges.size() == 1);
  CHECK(ranges[0] == DisplayRange{0, 5, kAnyBase});
}

// ---------------------------------------------------------------------------
// Destination collisions
// ---------------------------------------------------------------------------

TEST_CASE("distinct destinations never collide", "[phase_model]") {
  std::vector<PhaseSpan> phases{
      {468, 0, 122, 0x16FCF000u},
      {468, 122, 123, 0x16D8F000u},
  };
  CHECK(FindOverwrittenPhases(phases).empty());
}

TEST_CASE("two phases resolving to one address in a frame collide",
          "[phase_model]") {
  // Measured in Hydro Thunder: a 512x576 phase at base 936 and a 1024x576 phase
  // at base 468 both resolve to 0x1690F000 in the same frame. Resolved images
  // are keyed by address alone, so they share one image - the second overwrites
  // the first, and a differing size reallocates it outright.
  std::vector<PhaseSpan> phases{
      {/*src_base=*/936, 0, 3, 0x1690F000u},
      {/*src_base=*/468, 3, 4, 0x1690F000u},
  };
  const auto overwritten = FindOverwrittenPhases(phases);
  REQUIRE(overwritten.size() == 1);
  CHECK(overwritten[0] == 0u);  // the base-936 phase loses its content
}

TEST_CASE("an empty phase is not reported as overwritten", "[phase_model]") {
  std::vector<PhaseSpan> phases{
      {936, 3, 3, 0x1690F000u},
      {468, 3, 4, 0x1690F000u},
  };
  CHECK(FindOverwrittenPhases(phases).empty());
}
