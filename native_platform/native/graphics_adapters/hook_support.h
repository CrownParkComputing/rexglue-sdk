#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace xn::gfx::adapters {

inline bool NativeGraphicsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("XN_NATIVE_GFX");
    return value && (!std::strcmp(value, "1") || !std::strcmp(value, "constants"));
  }();
  return enabled;
}

inline bool NativeGraphicsStatsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("XN_NATIVE_GFX_STATS");
    return value && value[0] == '1';
  }();
  return enabled;
}

struct NativeGraphicsHookStats {
  std::atomic<uint64_t> calls{0}, native{0}, fallback{0}, bytes{0};
};

inline void RecordNativeGraphicsHook(NativeGraphicsHookStats& stats, bool attempted,
                                     bool consumed, uint32_t float4_count) {
  // Keep atomic counters out of performance runs unless explicitly requested.
  if (!NativeGraphicsStatsEnabled()) return;
  stats.calls.fetch_add(1, std::memory_order_relaxed);
  if (consumed) {
    stats.native.fetch_add(1, std::memory_order_relaxed);
    stats.bytes.fetch_add(uint64_t(float4_count) * 16, std::memory_order_relaxed);
  } else if (attempted) stats.fallback.fetch_add(1, std::memory_order_relaxed);
}

inline void ReportNativeGraphicsHook(uint32_t address, const NativeGraphicsHookStats& stats) {
  if (!NativeGraphicsStatsEnabled()) return;
  std::fprintf(stderr, "native-gfx: %08X calls=%llu native=%llu fallback=%llu bytes=%llu\n",
      address, static_cast<unsigned long long>(stats.calls.load()),
      static_cast<unsigned long long>(stats.native.load()),
      static_cast<unsigned long long>(stats.fallback.load()),
      static_cast<unsigned long long>(stats.bytes.load()));
}

// Emitted once per target by the source generator. Call after stopping graphics
// work when the process uses _Exit rather than running C++ static destructors.
void ReportNativeGraphicsHooks();

}  // namespace xn::gfx::adapters
