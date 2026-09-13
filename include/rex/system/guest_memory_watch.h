/**
 * @file        include/rex/system/guest_memory_watch.h
 * @brief       Report changes to a guest memory range while a title runs.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace rex::system {

// Starts the --guest_watch sampler if any range was configured. Safe to call
// more than once; does nothing when the cvar is empty.
//
// A bring-up that stalls usually stalls because one structure never finished
// being filled in. Finding out WHEN a field changed, and what it changed to,
// is otherwise a gdb session or a rebuild with printfs in generated code -
// and generated code is exactly where a fix must never live.
void StartGuestMemoryWatch(rex::memory::Memory* memory);

// Called from the access-violation handler for a fault inside a watched range.
// Names the guest code doing the write, lets the write through, and returns
// true; returns false for every fault that is not a watch hit, which is the
// case whenever --guest_watch is unset.
//
// Diffing the range says WHAT changed; only this says WHO changed it, which is
// the half that identifies the function to read.
bool ReportGuestWatchFault(void* host_address, uint32_t guest_address, bool is_write,
                           uint32_t guest_lr);

// True only while a watch is armed, so the fault path can skip the lookup
// entirely in the normal case.
bool GuestMemoryWatchActive();

}  // namespace rex::system
