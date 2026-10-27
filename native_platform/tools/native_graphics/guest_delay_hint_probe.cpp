// Asset-free CPU-only policy check. Injected callbacks make this deterministic:
// it verifies the default handoff and the experimental 4096-call cadence,
// without timing a game, scheduler, or frame rate.

#include "../../native/runtime/guest/guest_delay_hint.hpp"

#include <cstdint>
#include <cstdio>

namespace {
struct Counts {
  uint32_t relax = 0;
  uint32_t yield = 0;
};

bool check(bool enabled, uint32_t calls, uint32_t expected_relax,
           uint32_t expected_yield) {
  Counts counts;
  uint32_t cadence = 0;
  for (uint32_t i = 0; i < calls; ++i) {
    rex::runtime::apply_guest_delay_hint_policy(
        enabled, cadence, [&] { ++counts.relax; }, [&] { ++counts.yield; });
  }
  if (counts.relax != expected_relax || counts.yield != expected_yield) {
    std::fprintf(stderr, "policy mismatch: enabled=%d calls=%u relax=%u yield=%u\n",
                 enabled, calls, counts.relax, counts.yield);
    return false;
  }
  return true;
}
}  // namespace

int main() {
  const bool ok = check(false, 4097, 0, 4097) &&
                  check(true, 4095, 4095, 0) &&
                  check(true, 4096, 4096, 1) &&
                  check(true, 8192, 8192, 2);
  if (!ok) return 1;
  std::puts("guest delay hint policy checks passed");
  return 0;
}
