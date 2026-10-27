// xenon-native: guest clock. REX_QUERY_TIMEBASE() in the codegen contract calls
// rex::chrono::Clock::QueryGuestTickCount().
#pragma once
#include <cstdint>
namespace rex::chrono {
struct Clock {
  static constexpr uint64_t kGuestTickFrequency = 50000000;  // Xenon timebase, 50 MHz
  static uint64_t QueryGuestTickCount();      // timebase ticks since boot
  static uint64_t QueryGuestSystemTime();     // FILETIME (100ns since 1601)
  static uint32_t QueryGuestUptimeMillis();   // ms since boot
  static uint64_t QueryHostNanos();           // steady clock, ns
};
}  // namespace rex::chrono
