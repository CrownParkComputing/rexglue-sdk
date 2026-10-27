#include <rex/chrono/clock.h>

#include <chrono>

namespace rex::chrono {
namespace {
const auto g_boot = std::chrono::steady_clock::now();
}
uint64_t Clock::QueryHostNanos() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - g_boot)
                      .count());
}
uint64_t Clock::QueryGuestTickCount() {
  // 50 MHz timebase: one tick per 20 ns.
  return QueryHostNanos() / 20;
}
uint64_t Clock::QueryGuestSystemTime() {
  // FILETIME: 100 ns intervals since 1601-01-01.
  auto now = std::chrono::system_clock::now().time_since_epoch();
  uint64_t unix_100ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100);
  return unix_100ns + 116444736000000000ull;
}
uint32_t Clock::QueryGuestUptimeMillis() { return uint32_t(QueryHostNanos() / 1000000); }
}  // namespace rex::chrono
