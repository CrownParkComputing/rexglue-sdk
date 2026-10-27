#pragma once

// Host approximation for a guest delay/backoff hint. This is not a memory
// barrier, notification, or synchronization primitive. Loads and stores around
// it remain the guest code's responsibility.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <utility>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif

namespace rex::runtime {

// Opt-in is intentionally process-wide and read only once. The safe default
// retains the prior scheduler handoff for every guest hint.
inline bool guest_delay_hint_experiment_enabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv("XN_GUEST_DELAY_HINT");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

inline void host_processor_relax() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  _mm_pause();
#elif (defined(__i386__) || defined(__x86_64__)) && (defined(__GNUC__) || defined(__clang__))
  __asm__ __volatile__("pause");
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
  // AArch64 YIELD is a processor hint. It does not transfer execution to
  // another OS thread and is not a memory fence.
  __asm__ __volatile__("yield");
#else
  // Keep a scheduler handoff on targets without a known processor hint.
  std::this_thread::yield();
#endif
  // Compiler ordering only; the guest delay hint is not a hardware fence.
  std::atomic_signal_fence(std::memory_order_seq_cst);
}

// Factored policy with injected operations so the small CPU-only probe can
// verify handoff cadence without relying on scheduler timing.
template <typename Relax, typename Yield>
inline void apply_guest_delay_hint_policy(bool enabled, uint32_t& relax_count,
                                          Relax&& relax, Yield&& yield) {
  if (!enabled) {
    std::forward<Yield>(yield)();
    return;
  }
  std::forward<Relax>(relax)();
  if ((++relax_count & 0xFFFu) == 0) {
    std::forward<Yield>(yield)();
  }
}

// Experimental policy: amortize scheduler handoffs while retaining an
// occasional handoff for other runnable threads. The counter is per host
// thread, so concurrent guest waits do not contend on shared state. The cadence
// is a tuning parameter, not an architectural equivalence claim.
inline void guest_delay_hint() noexcept {
#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__) || defined(__aarch64__)
  thread_local uint32_t relax_count = 0;
  apply_guest_delay_hint_policy(guest_delay_hint_experiment_enabled(), relax_count,
                                [] { host_processor_relax(); },
                                [] { std::this_thread::yield(); });
#else
  // Unknown targets retain a scheduler handoff on every guest hint.
  (void)guest_delay_hint_experiment_enabled();
  std::this_thread::yield();
#endif
}

}  // namespace rex::runtime
