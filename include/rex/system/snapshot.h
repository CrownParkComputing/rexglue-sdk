/**
 * @file        system/snapshot.h
 * @brief       Entry/exit register and memory snapshots of hooked guest functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// A native replacement for a recompiled function is right when, given the
// same registers and the same memory, it leaves the same registers and the
// same memory behind. This records exactly that for a hooked function: the
// full PPCContext and a set of declared guest ranges at entry, and again at
// exit, into one `.snap` file per call. tests/native/snap_replay materialises
// a recording, runs a replacement over it and diffs the exits.
//
// Enabled by REX_SNAP_DIR=<directory>; without it Record() is a null check.
// REX_SNAP_MAX caps the number of files per guest address (default 32),
// REX_SNAP_MAX_BYTES caps one range (default 1 MiB; larger ranges are
// truncated and flagged in the file). Both entry and exit copies of every
// range are stored, so "what did the call write" is answered by the reader
// (entry bytes vs exit bytes) with no dependence on the runtime's write-watch
// state - see docs/native/FORMATS.md for the limitation that implies.
//
// Format frozen in docs/native/FORMATS.md (".snap").

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <rex/ppc/context.h>

namespace rex::snapshot {

// A guest virtual range to store at entry and exit. Zero size (or zero
// address) ranges are skipped, which lets a hook declare ranges from
// registers that may be null.
struct Range {
  uint32_t addr;
  uint32_t size;
};

// Register file as stored in a .snap. Plain, fixed layout, host endian.
struct alignas(16) RegFile {
  uint64_t r[32];
  uint64_t lr;
  uint64_t ctr;
  uint8_t xer_so, xer_ov, xer_ca, xer_pad;
  uint8_t cr[8];   // 4-bit raw per field: lt<<3|gt<<2|eq<<1|so
  uint32_t fpscr;  // host csr word as PPCContext keeps it
  uint32_t msr;
  uint8_t vscr_sat;
  uint8_t pad_[11];  // explicit: f[] must start at offset 304
  uint64_t f[32];
  uint8_t v[128][16];
};
static_assert(sizeof(RegFile) == 2608 && offsetof(RegFile, f) == 304 && offsetof(RegFile, v) == 560,
              "RegFile layout is part of the .snap format");

void Capture(const PPCContext& ctx, RegFile& out);
void Restore(const RegFile& in, PPCContext& ctx);

// One in-flight recording; opaque to callers.
struct Call;

// Entry: opens `<REX_SNAP_DIR>/<ADDR8>_<tag>_<seq>.snap`, writes the header,
// entry registers and entry copy of `ranges`. Returns nullptr when snapshots
// are off, the per-address cap is reached or the file cannot be created.
// `tag` is a short label (the hook name); it is sanitised for the file name.
// Not a hot path: one file open and a few writes per call.
Call* Record(const char* tag, uint32_t addr, const PPCContext& ctx, const uint8_t* base,
             std::span<const Range> ranges);

// Exit: appends exit registers and the exit copy of the same ranges, then
// closes the file. Accepts nullptr (does nothing).
void Record(Call* call, const PPCContext& ctx, const uint8_t* base);

// True when REX_SNAP_DIR is set and usable.
bool Enabled();

// Number of files written so far for `addr` (0 when never recorded).
uint32_t CountFor(uint32_t addr);

// Parses the hex address after "sub_" in a symbol name; 0 when absent.
uint32_t ParseAddress(const char* name);

}  // namespace rex::snapshot

// Wrap a generated function so every call (up to REX_SNAP_MAX) is recorded.
// Defines the strong `sub_X` that displaces the weak generated alias and
// forwards to `__imp__sub_X`. Ranges are brace lists evaluated at entry with
// `ctx` and `base` in scope, so they may come from argument registers:
//
//   REX_SNAP_HOOK(sub_8269ADD8, {ctx.r4.u32, 64}, {ctx.r5.u32, ctx.r6.u32 * 4})
//   REX_SNAP_HOOK(sub_82695BE0)                       // registers only
//
// Requires __VA_OPT__ (C++20).
#define REX_SNAP_HOOK(sub, ...)                                                               \
  REX_EXTERN(__imp__##sub);                                                                   \
  extern "C" REX_FUNC(sub) {                                                                  \
    const ::rex::snapshot::Range rex_snap_ranges_[] = {__VA_ARGS__ __VA_OPT__(, ) {0u, 0u}}; \
    ::rex::snapshot::Call* rex_snap_call_ = ::rex::snapshot::Record(                         \
        #sub, ::rex::snapshot::ParseAddress(#sub), ctx, base, rex_snap_ranges_);              \
    __imp__##sub(ctx, base);                                                                  \
    ::rex::snapshot::Record(rex_snap_call_, ctx, base);                                       \
  }
