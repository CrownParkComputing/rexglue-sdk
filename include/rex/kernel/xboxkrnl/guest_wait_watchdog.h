/**
 * @file        include/rex/kernel/xboxkrnl/guest_wait_watchdog.h
 * @brief       Reports what the guest is waiting on while it is still waiting.
 *
 *              A title that has stopped making progress is almost always parked
 *              in a kernel wait, and the log says nothing: a wait that never
 *              returns can never be reported by timing it after the fact. This
 *              records waits as they START and a watchdog prints every one that
 *              is still outstanding, so a stalled bring-up says
 *
 *                [GUESTWAIT] Main XThread: NtSignalAndWaitForSingleObjectEx on
 *                            0x00000114 for 42.0 s (lr 82546924)
 *
 *              instead of nothing at all. Off unless
 *              guest_wait_report_seconds is set.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

namespace rex::kernel::xboxkrnl {

// Records one in-flight guest wait for the lifetime of the scope. Nested waits
// on one thread (a wait entered from an APC, say) keep the outermost, which is
// the one that describes what the thread is actually stuck on.
class GuestWaitScope {
 public:
  // signal_target is the object the call signals first, for the
  // signal-and-wait handshake; 0 when the API only waits.
  // target is the guest handle (or object pointer for the Ke* APIs).
  // target_object is the waited object's guest address when the handle form is
  // used, so a KeSetEvent on that address still matches this wait.
  GuestWaitScope(const char* api, uint32_t target, uint32_t signal_target = 0,
                 uint32_t target_object = 0);
  ~GuestWaitScope();

  GuestWaitScope(const GuestWaitScope&) = delete;
  GuestWaitScope& operator=(const GuestWaitScope&) = delete;

 private:
  bool recorded_ = false;
};

// Counts a guest signal of a kernel object, so the watchdog can say whether the
// thing a thread waits on is ever signalled at all. Cheap and ignored unless
// the watchdog is enabled.
void RecordGuestSignal(uint32_t handle);

// Notes a kernel object as the guest creates it, with the guest function that
// asked for it. When the watchdog then reports a wait on that object, the
// creator is the first place to look for whoever was meant to signal it.
void RecordGuestObjectCreation(const char* kind, uint32_t handle, uint32_t guest_object,
                               const char* detail);

}  // namespace rex::kernel::xboxkrnl
