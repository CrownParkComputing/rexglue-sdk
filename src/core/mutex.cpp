/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstdlib>
#include <string>
#include <dlfcn.h>
#include <cxxabi.h>
#include <map>
#include <vector>
#include <chrono>
#include <rex/logging.h>
#include <fmt/format.h>
#include <rex/thread/mutex.h>

namespace rex::thread {

std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex global_mutex;
  return global_mutex;
}

namespace detail {

void GlobalLockWatch::Report(const void* blocker, int64_t microseconds) {
  // Aggregate per blocking call site, and print on a coarse interval: a
  // contended kernel-wide lock produces thousands of these a second.
  struct Site {
    uint64_t waits = 0;
    uint64_t total_microseconds = 0;
  };
  static std::mutex lock;
  static std::map<const void*, Site> sites;
  static std::chrono::steady_clock::time_point next_report;
  std::lock_guard<std::mutex> guard(lock);
  Site& site = sites[blocker];
  ++site.waits;
  site.total_microseconds += uint64_t(microseconds);
  const auto now = std::chrono::steady_clock::now();
  if (now < next_report) {
    return;
  }
  next_report = now + std::chrono::seconds(5);
  std::vector<std::pair<const void*, Site>> ranked(sites.begin(), sites.end());
  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    return a.second.total_microseconds > b.second.total_microseconds;
  });
  REXLOG_WARN("[LOCKWAIT] holders that blocked others most:");
  for (size_t i = 0; i < ranked.size() && i < 8; ++i) {
    // dladdr turns the return address into the enclosing symbol, which is what
    // a reader needs; without it this is a hex address in a shared library at a
    // randomised base.
    std::string where = fmt::format("{}", fmt::ptr(ranked[i].first));
    Dl_info info;
    if (ranked[i].first && dladdr(ranked[i].first, &info) && info.dli_sname) {
      int status = 0;
      char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
      where = fmt::format("{}+0x{:x}", status == 0 && demangled ? demangled : info.dli_sname,
                          uintptr_t(ranked[i].first) - uintptr_t(info.dli_saddr));
      std::free(demangled);
    }
    REXLOG_WARN("[LOCKWAIT]   {} waits, {} ms total - {}", ranked[i].second.waits,
                ranked[i].second.total_microseconds / 1000, where);
  }
  sites.clear();
}

}  // namespace detail

}  // namespace rex::thread
