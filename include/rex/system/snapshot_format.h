/**
 * @file        system/snapshot_format.h
 * @brief       On-disk layout of a .snap recording (shared by the recorder and snap_replay)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// File layout (all integers little-endian host order, no alignment padding
// beyond what the structs declare):
//
//   FileHeader
//   tag           header.tag_len bytes, no terminator, padded to 4 bytes
//   RangeRecord   x header.range_count
//   RegFile       entry registers
//   bytes         entry copy of every range, in table order, range.stored each
//   RegFile       exit registers            (only when flags & kHasExit)
//   bytes         exit copy of every range  (only when flags & kHasExit)
//
// Frozen in docs/native/FORMATS.md.

#pragma once

#include <cstdint>

namespace rex::snapshot {

constexpr char kMagic[8] = {'R', 'E', 'X', 'S', 'N', 'A', 'P', '\0'};
constexpr uint32_t kVersion = 1;

enum HeaderFlags : uint32_t {
  kHasExit = 1u << 0,  // exit registers and bytes follow the entry bytes
};

enum RangeFlags : uint32_t {
  kTruncated = 1u << 0,  // stored < size because of REX_SNAP_MAX_BYTES
};

struct FileHeader {
  char magic[8];
  uint32_t version;
  uint32_t header_size;   // sizeof(FileHeader)
  uint32_t addr;          // guest address of the recorded function
  uint32_t range_count;
  uint64_t timestamp_ns;  // unix time at entry
  uint32_t tag_len;
  uint32_t flags;         // HeaderFlags
};
static_assert(sizeof(FileHeader) == 40);

struct RangeRecord {
  uint32_t addr;    // guest virtual address as declared
  uint32_t size;    // bytes as declared
  uint32_t stored;  // bytes actually present in each copy
  uint32_t flags;   // RangeFlags
};
static_assert(sizeof(RangeRecord) == 16);

}  // namespace rex::snapshot
