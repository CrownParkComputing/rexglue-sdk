#pragma once
#include <array>
#include <cstdint>

namespace xn::gfx {

// Bit-exact CPU profiling key; contains no Vulkan handles/types or pointers.
// Index byte offset is deliberately absent: MDI represents it via firstIndex.
struct DrawBatchKey {
  uint64_t pipeline = 0, layout = 0, vs_textures = 0, ps_textures = 0, index_buffer = 0;
  uint32_t index_type = 0, indexed = 0;
  uint64_t render_epoch = 0;
  std::array<uint32_t, 6> viewport_bits{};
  std::array<uint32_t, 4> scissor{};
  std::array<uint32_t, 3> depth_bias{};
  std::array<uint32_t, 4> blend{};
  uint32_t stencil_ref = 0;
  std::array<uint32_t, 5> constant_offsets{};
  bool operator==(const DrawBatchKey&) const = default;
};

struct DrawBatchBreakReasons {
  // Non-exclusive: one transition may increment several changed categories.
  uint64_t epoch = 0, pipeline_layout = 0, textures = 0, dynamic = 0, index = 0;
  uint64_t uniform_offsets = 0;  // strict runs only
  // Count only boundaries which terminate a nonempty run.
  uint64_t ineligible = 0, explicit_flush = 0;
  bool operator==(const DrawBatchBreakReasons&) const = default;
};
struct DrawBatchRunStats {
  uint64_t runs = 0, max_run = 0, potential_draw_calls_saved = 0;
  // Bins: 1, 2, 3-4, 5-8, 9-16, 17-32, 33+ draws.
  std::array<uint64_t, 7> histogram{};
  DrawBatchBreakReasons breaks{};
  bool operator==(const DrawBatchRunStats&) const = default;
};
struct DrawBatchProfileStats {
  uint64_t total_draws = 0, eligible_draws = 0;
  DrawBatchRunStats weak{}, strict{};
  bool operator==(const DrawBatchProfileStats&) const = default;
};

class DrawBatchProfile {
 public:
  void Observe(const DrawBatchKey& key, bool eligible = true) {
    ++stats_.total_draws;
    if (!eligible) {
      if (weak_run_) ++stats_.weak.breaks.ineligible;
      if (strict_run_) ++stats_.strict.breaks.ineligible;
      FlushRuns();
      return;
    }
    ++stats_.eligible_draws;
    if (!weak_run_) {
      weak_run_ = strict_run_ = 1;
      previous_ = key;
      return;
    }
    DrawBatchBreakReasons changed{};
    changed.epoch = previous_.render_epoch != key.render_epoch;
    changed.pipeline_layout = previous_.pipeline != key.pipeline || previous_.layout != key.layout;
    changed.textures = previous_.vs_textures != key.vs_textures || previous_.ps_textures != key.ps_textures;
    changed.dynamic = previous_.viewport_bits != key.viewport_bits || previous_.scissor != key.scissor ||
        previous_.depth_bias != key.depth_bias || previous_.blend != key.blend || previous_.stencil_ref != key.stencil_ref;
    changed.index = previous_.index_buffer != key.index_buffer || previous_.index_type != key.index_type ||
        previous_.indexed != key.indexed;
    const bool weak_break = changed.epoch || changed.pipeline_layout || changed.textures || changed.dynamic || changed.index;
    const bool offsets = previous_.constant_offsets != key.constant_offsets;
    if (weak_break) {
      Record(weak_run_, stats_.weak);
      Add(stats_.weak.breaks, changed);
      weak_run_ = 1;
    } else ++weak_run_;
    if (weak_break || offsets) {
      Record(strict_run_, stats_.strict);
      changed.uniform_offsets = offsets;
      Add(stats_.strict.breaks, changed);
      strict_run_ = 1;
    } else ++strict_run_;
    previous_ = key;  // one fixed key copy, shared by both independent runs
  }

  // Resource boundaries and swap/end boundaries must terminate both runs.
  void ExplicitFlush() {
    if (weak_run_) ++stats_.weak.breaks.explicit_flush;
    if (strict_run_) ++stats_.strict.breaks.explicit_flush;
    FlushRuns();
  }
  void Reset() { *this = {}; }
  // Includes active runs in a finished statistics copy. Repeated queries do
  // not split runs or double count them, and add no synthetic break reason.
  [[nodiscard]] DrawBatchProfileStats GetStats() const {
    auto result = stats_;
    Record(weak_run_, result.weak);
    Record(strict_run_, result.strict);
    return result;
  }

 private:
  static void Record(uint64_t length, DrawBatchRunStats& stats) {
    if (!length) return;
    ++stats.runs;
    if (length > stats.max_run) stats.max_run = length;
    stats.potential_draw_calls_saved += length - 1;
    const unsigned bin = length == 1 ? 0 : length == 2 ? 1 : length <= 4 ? 2 :
                         length <= 8 ? 3 : length <= 16 ? 4 : length <= 32 ? 5 : 6;
    ++stats.histogram[bin];
  }
  static void Add(DrawBatchBreakReasons& to, const DrawBatchBreakReasons& from) {
    to.epoch += from.epoch;
    to.pipeline_layout += from.pipeline_layout;
    to.textures += from.textures;
    to.dynamic += from.dynamic;
    to.index += from.index;
    to.uniform_offsets += from.uniform_offsets;
  }
  void FlushRuns() {
    Record(weak_run_, stats_.weak);
    Record(strict_run_, stats_.strict);
    weak_run_ = strict_run_ = 0;
  }
  DrawBatchKey previous_{};
  DrawBatchProfileStats stats_{};
  uint64_t weak_run_ = 0, strict_run_ = 0;
};

}  // namespace xn::gfx
