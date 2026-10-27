#include "gfx/vulkan/draw_batch_profile.h"
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace xn::gfx;
static_assert(std::is_trivially_copyable_v<DrawBatchKey>);
static_assert(std::is_trivially_copyable_v<DrawBatchProfile>);
static_assert(std::tuple_size_v<decltype(DrawBatchKey::constant_offsets)> == 5);
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void TestOffsetsAndSnapshots() {
  DrawBatchProfile profile;
  DrawBatchKey key{};
  for (unsigned i = 0; i < 3; ++i) profile.Observe(key);
  key.constant_offsets[0] = 16;
  for (unsigned i = 0; i < 2; ++i) profile.Observe(key);
  key.constant_offsets[4] = 32;
  for (unsigned i = 0; i < 2; ++i) profile.Observe(key);
  const auto stats = profile.GetStats();
  Check(stats.total_draws == 7 && stats.eligible_draws == 7, "draw totals");
  Check(stats.weak.runs == 1 && stats.weak.max_run == 7 && stats.weak.potential_draw_calls_saved == 6,
        "weak must ignore constant offsets");
  Check(stats.strict.runs == 3 && stats.strict.max_run == 3 && stats.strict.potential_draw_calls_saved == 4 &&
        stats.strict.histogram[1] == 2 && stats.strict.histogram[2] == 1, "strict offset runs");
  Check(stats.weak.breaks.uniform_offsets == 0 && stats.strict.breaks.uniform_offsets == 2,
        "strict-only offset break accounting");
  Check(profile.GetStats() == stats, "snapshot double counted active runs");
  profile.Observe(key);
  Check(profile.GetStats().weak.max_run == 8 && profile.GetStats().strict.max_run == 3,
        "snapshot split active run");
  profile.ExplicitFlush();
  const auto flushed = profile.GetStats();
  profile.ExplicitFlush();
  Check(profile.GetStats() == flushed, "empty explicit flush changed counts");
}
void TestReasonsAndBits() {
  DrawBatchProfile profile;
  DrawBatchKey key{};
  profile.Observe(key);
  auto next = [&] { profile.Observe(key); };
  ++key.pipeline; next(); ++key.layout; next();
  ++key.vs_textures; next(); ++key.ps_textures; next();
  key.viewport_bits[4] = 0x80000000; next();  // -0 differs from +0 bit-exactly.
  ++key.scissor[2]; next(); ++key.depth_bias[1]; next();
  ++key.blend[3]; next(); ++key.stencil_ref; next();
  ++key.index_buffer; next(); ++key.index_type; next(); key.indexed = 1; next();
  ++key.render_epoch; next(); ++key.constant_offsets[4]; next();
  const auto stats = profile.GetStats();
  const auto& b = stats.strict.breaks;
  Check(b.pipeline_layout == 2 && b.textures == 2 && b.dynamic == 5 && b.index == 3 &&
        b.epoch == 1 && b.uniform_offsets == 1, "individual typed-key break reasons");
  Check(stats.strict.runs == stats.total_draws && stats.weak.runs + 1 == stats.total_draws,
        "strict/weak independence");
  DrawBatchKey copy = key;
  Check(copy == key, "typed equality");
  ++copy.constant_offsets[1]; Check(!(copy == key), "typed equality ignored offsets");
  copy = key; copy.viewport_bits[0] = 0x7fc00001;
  key.viewport_bits[0] = 0x7fc00002;
  Check(!(copy == key), "NaN payload bits were compared as floats");
  profile.Reset();
  profile.Observe(key);
  ++key.render_epoch; ++key.pipeline; ++key.ps_textures; ++key.stencil_ref;
  ++key.index_type; ++key.constant_offsets[0]; profile.Observe(key);
  const auto multi = profile.GetStats().strict.breaks;
  Check(multi.epoch == 1 && multi.pipeline_layout == 1 && multi.textures == 1 &&
        multi.dynamic == 1 && multi.index == 1 && multi.uniform_offsets == 1,
        "simultaneous reasons must be non-exclusive");
}
void TestBoundariesEligibilityAndReset() {
  DrawBatchProfile profile;
  DrawBatchKey key{};
  profile.Observe(key); profile.Observe(key);
  profile.Observe(key, false); profile.Observe(key, false);
  profile.Observe(key); profile.ExplicitFlush();
  profile.Observe(key); ++key.render_epoch; profile.Observe(key);
  const auto stats = profile.GetStats();
  Check(stats.total_draws == 7 && stats.eligible_draws == 5, "ineligible draws not counted correctly");
  Check(stats.weak.runs == 4 && stats.weak.histogram[0] == 3 && stats.weak.histogram[1] == 1 &&
        stats.weak.potential_draw_calls_saved == 1, "boundary run accounting");
  Check(stats.weak.breaks.ineligible == 1 && stats.weak.breaks.explicit_flush == 1 &&
        stats.weak.breaks.epoch == 1 && stats.weak == stats.strict, "resource/epoch boundaries");
  profile.Reset();
  Check(profile.GetStats() == DrawBatchProfileStats{}, "Reset left profile history");
  profile.Observe(key, false);
  Check(profile.GetStats().total_draws == 1 && profile.GetStats().eligible_draws == 0 &&
        profile.GetStats().weak.runs == 0, "ineligible-only profile created a run");
}
void TestHistogram() {
  DrawBatchProfile profile;
  DrawBatchKey key{};
  for (const unsigned length : {1, 2, 3, 4, 5, 8, 9, 16, 17, 32, 33, 65}) {
    for (unsigned i = 0; i < length; ++i) profile.Observe(key);
    profile.ExplicitFlush();
  }
  const auto stats = profile.GetStats();
  Check(stats.total_draws == 195 && stats.eligible_draws == 195 && stats.weak.runs == 12 &&
        stats.weak.max_run == 65 && stats.weak.potential_draw_calls_saved == 183, "histogram totals");
  Check(stats.weak.histogram == std::array<uint64_t, 7>{1, 1, 2, 2, 2, 2, 2} &&
        stats.weak == stats.strict && stats.weak.breaks.explicit_flush == 12, "histogram edge bins");
}
}  // namespace

int main() {
  try {
    TestOffsetsAndSnapshots(); TestReasonsAndBits(); TestBoundariesEligibilityAndReset(); TestHistogram();
    std::cout << "draw batch profile probe: all CPU checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "draw batch profile probe: " << error.what() << '\n';
    return 1;
  }
}
