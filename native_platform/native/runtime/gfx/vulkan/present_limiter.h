#pragma once
#include <cstdint>
namespace xn::gfx {
// A display cap never changes the guest vblank clock. The small tolerance
// absorbs scheduler jitter without turning a 60 Hz producer into 30 Hz.
struct PresentLimiter {
  uint64_t next_ns = 0;
  uint32_t previous_target = 0;
  bool Allow(uint64_t now, uint32_t target) {
    if (!target) return true;
    const uint64_t period = 1000000000ull / target;
    if (target != previous_target) { next_ns = 0; previous_target = target; }
    if (next_ns && now + 1000000 < next_ns) return false;
    if (!next_ns || now > next_ns + period) next_ns = now;
    next_ns += period;
    return true;
  }
};
}
