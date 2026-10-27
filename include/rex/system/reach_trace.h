/**
 * @file        system/reach_trace.h
 * @brief       Per-function reach counters for recompiled guest code
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Which generated guest functions does a title actually run, and how often?
// The answer drives the native migration: a function that is never reached
// needs no native replacement, one reached a million times a frame is where
// the CPU time is. The generated prologue compiled with -DREX_GUEST_REACH
// owns one Slot per function (a zero-initialised static, no guard variable)
// and calls Hit() on every entry: one relaxed atomic increment on the hot
// path, plus a single Register() call the first time the function runs.
//
// Set REX_TRACE_REACH=<path> to receive the counters as CSV (addr,count).
// The file is rewritten periodically (REX_TRACE_REACH_INTERVAL_MS, default
// 5000, 0 disables), at normal exit and on SIGTERM/SIGINT, so a timed run
// or a title that hard-exits still leaves a file behind.
//
// Format frozen in docs/native/FORMATS.md.

#pragma once

#include <atomic>
#include <cstdint>

namespace rex::reach {

struct Slot {
  std::atomic<uint64_t> count{0};  // incremented on every call
  uint32_t addr = 0;               // guest address parsed from the name
  const char* name = nullptr;      // "__imp__sub_XXXXXXXX" or "sub_XXXXXXXX"
  Slot* next = nullptr;            // intrusive registry list
};

// First-call registration. `name` must outlive the process (__func__ does).
// Parses the guest address from the "sub_XXXXXXXX" part of the name.
void Register(Slot* slot, const char* name);

// Hot path: one relaxed fetch_add; the branch is taken exactly once per slot.
inline void Hit(Slot& slot, const char* name) {
  if (__builtin_expect(slot.count.fetch_add(1, std::memory_order_relaxed) == 0, 0)) {
    Register(&slot, name);
  }
}

// True when REX_TRACE_REACH names an output file.
bool Enabled();

// Write the counters to REX_TRACE_REACH now. Async-signal-safe: uses only
// open/write/rename and a stack buffer. Safe to call when tracing is off
// (does nothing). Returns the number of slots written.
size_t Dump();

// Same, to an explicit path.
size_t DumpTo(const char* path);

}  // namespace rex::reach
