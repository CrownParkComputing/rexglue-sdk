/**
 * @file        src/kernel/xboxkrnl/guest_wait_watchdog.cpp
 * @brief       Implementation of the in-flight guest wait watchdog.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/kernel/xboxkrnl/guest_wait_watchdog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <pthread.h>

#include <rex/thread.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_UINT32(
    guest_wait_report_seconds, 0, "Kernel",
    "Report every guest kernel wait still outstanding after this many seconds, then again on that "
    "interval. 0 disables. A stalled title shows which thread is parked on which object.");

namespace rex::kernel::xboxkrnl {

namespace {

struct WaitRecord {
  const char* api;
  uint32_t target;
  std::chrono::steady_clock::time_point started;
  std::string thread_name;
};

std::mutex g_waits_lock;
std::map<uint64_t, WaitRecord> g_waits;  // keyed by host thread id
std::atomic<bool> g_watchdog_started{false};

void WatchdogMain() {
  const auto interval = std::chrono::seconds(REXCVAR_GET(guest_wait_report_seconds));
  auto next_report = std::chrono::steady_clock::now() + interval;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto now = std::chrono::steady_clock::now();
    if (now < next_report) {
      continue;
    }
    next_report = now + interval;

    std::vector<std::pair<double, WaitRecord>> outstanding;
    {
      std::lock_guard<std::mutex> lock(g_waits_lock);
      for (const auto& [thread_id, record] : g_waits) {
        const double seconds = std::chrono::duration<double>(now - record.started).count();
        if (seconds >= double(interval.count())) {
          outstanding.emplace_back(seconds, record);
        }
      }
    }
    if (outstanding.empty()) {
      continue;
    }
    // Longest first: whatever has been stuck the longest is the thing to look
    // at, and the threads merely parked for work sort to the bottom.
    std::sort(outstanding.begin(), outstanding.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    REXLOG_WARN("[GUESTWAIT] {} guest wait(s) outstanding:", outstanding.size());
    for (const auto& [seconds, record] : outstanding) {
      REXLOG_WARN("[GUESTWAIT]   {:<24} {} on {:#010x} for {:.1f} s",
                  record.thread_name.empty() ? "?" : record.thread_name, record.api, record.target,
                  seconds);
    }
  }
}

void EnsureWatchdog() {
  if (g_watchdog_started.load(std::memory_order_acquire)) {
    return;
  }
  bool expected = false;
  if (!g_watchdog_started.compare_exchange_strong(expected, true)) {
    return;
  }
  // Detached on purpose: it outlives nothing, holds no state anyone waits on,
  // and a diagnostic must not add a shutdown ordering problem of its own.
  std::thread(WatchdogMain).detach();
}

}  // namespace

GuestWaitScope::GuestWaitScope(const char* api, uint32_t target) {
  if (REXCVAR_GET(guest_wait_report_seconds) == 0) {
    return;
  }
  EnsureWatchdog();

  const uint64_t thread_id = rex::thread::current_thread_id();
  // The guest object's own name is usually empty; the host thread name is what
  // the rest of the log and every profiler calls this thread, so prefer it and
  // fall back to the guest name.
  auto* thread = rex::system::XThread::GetCurrentThread();
  std::string name = thread ? thread->name() : std::string();
  char host_name[32] = {};
  if (pthread_getname_np(pthread_self(), host_name, sizeof(host_name)) == 0 && host_name[0]) {
    name = host_name;
  }
  std::lock_guard<std::mutex> lock(g_waits_lock);
  // Keep the outermost wait for this thread.
  if (g_waits.find(thread_id) != g_waits.end()) {
    return;
  }
  g_waits.emplace(thread_id,
                  WaitRecord{api, target, std::chrono::steady_clock::now(), std::move(name)});
  recorded_ = true;
}

GuestWaitScope::~GuestWaitScope() {
  if (!recorded_) {
    return;
  }
  const uint64_t thread_id = rex::thread::current_thread_id();
  std::lock_guard<std::mutex> lock(g_waits_lock);
  g_waits.erase(thread_id);
}

}  // namespace rex::kernel::xboxkrnl
