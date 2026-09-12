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

#include <fmt/format.h>

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
  uint32_t signal_target;
  uint32_t target_object;
  uint32_t caller_lr;
  std::chrono::steady_clock::time_point started;
  std::string thread_name;
};

struct SignalRecord {
  uint64_t count = 0;
  std::chrono::steady_clock::time_point last;
};

std::mutex g_waits_lock;
std::map<uint64_t, WaitRecord> g_waits;  // keyed by host thread id
std::map<uint32_t, SignalRecord> g_signals;  // guest handle -> signal history
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
      uint64_t signals = 0;
      double since_signal = -1.0;
      {
        std::lock_guard<std::mutex> lock(g_waits_lock);
        // A handle wait can be satisfied by a KeSetEvent on the object itself,
        // so check both keys before reporting "never signalled".
        for (uint32_t key : {record.target, record.target_object}) {
          if (!key) {
            continue;
          }
          auto it = g_signals.find(key);
          if (it != g_signals.end()) {
            signals += it->second.count;
            const double age = std::chrono::duration<double>(now - it->second.last).count();
            if (since_signal < 0.0 || age < since_signal) {
              since_signal = age;
            }
          }
        }
      }
      // The interesting case is a signal that arrived DURING the wait: the
      // object was signalled and the waiter did not wake, which is a runtime
      // bug rather than a guest handshake nobody completes.
      const bool signalled_during_wait = signals && since_signal >= 0.0 && since_signal < seconds;
      REXLOG_WARN(
          "[GUESTWAIT]   {:<24} {} on {:#010x}{} from lr {:#010x} for {:.1f} s "
          "(signalled {} times{}{})",
          record.thread_name.empty() ? "?" : record.thread_name, record.api, record.target,
          record.signal_target ? fmt::format(", signalling {:#010x}", record.signal_target)
                               : std::string(),
          record.caller_lr, seconds, signals,
          signals ? fmt::format(", last {:.1f} s ago", since_signal) : std::string(),
          signalled_during_wait ? " <- SIGNALLED WHILE WAITING" : "");
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

GuestWaitScope::GuestWaitScope(const char* api, uint32_t target, uint32_t signal_target,
                               uint32_t target_object) {
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
  // The guest return address says which recompiled function is doing the
  // waiting, which is the difference between an address and a place in the
  // code to read.
  uint32_t caller_lr = 0;
  if (thread && thread->thread_state() && thread->thread_state()->context()) {
    caller_lr = static_cast<uint32_t>(thread->thread_state()->context()->lr);
  }
  char host_name[32] = {};
  if (pthread_getname_np(pthread_self(), host_name, sizeof(host_name)) == 0 && host_name[0]) {
    name = host_name;
  }
  std::lock_guard<std::mutex> lock(g_waits_lock);
  // Keep the outermost wait for this thread.
  if (g_waits.find(thread_id) != g_waits.end()) {
    return;
  }
  g_waits.emplace(thread_id, WaitRecord{api, target, signal_target, target_object, caller_lr,
                                        std::chrono::steady_clock::now(), std::move(name)});
  recorded_ = true;
}

void RecordGuestObjectCreation(const char* kind, uint32_t handle, uint32_t guest_object,
                               const char* detail) {
  if (REXCVAR_GET(guest_wait_report_seconds) == 0) {
    return;
  }
  auto* thread = rex::system::XThread::GetCurrentThread();
  uint32_t caller_lr = 0;
  if (thread && thread->thread_state() && thread->thread_state()->context()) {
    caller_lr = static_cast<uint32_t>(thread->thread_state()->context()->lr);
  }
  REXLOG_WARN("[GUESTWAIT] created {} handle {:#010x} object {:#010x} {} from lr {:#010x}", kind,
              handle, guest_object, detail, caller_lr);
}

void RecordGuestSignal(uint32_t handle) {
  if (REXCVAR_GET(guest_wait_report_seconds) == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_waits_lock);
  auto& record = g_signals[handle];
  ++record.count;
  record.last = std::chrono::steady_clock::now();
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
