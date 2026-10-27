/**
 * @file        system/tee.h
 * @brief       Per-frame digest channel for comparing two runs of a title
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Snapshots answer "is this one call right"; a tee answers "does this run
// still do what that run did". A hook in tee mode (see the port's
// TeePre_/TeePost_ hook points) feeds the bytes it cares about - a draw's
// arguments, a state block, a resolved buffer - into a Channel under a short
// key. At the end of a frame the channel publishes one JSON line per key with
// the running XXH3 of everything digested under it that frame:
//
//   {"frame":412,"key":"d3d/draw","hash":"3f9c1a0e77d2b4c1","n":1873}
//
// Two runs (guest vs tee, old runtime vs new, flag on vs off) driven by the
// same key script then diff by frame and key with tools/tee_diff.py, which
// reports the first frame and key that diverge. Digests are order-sensitive:
// the same bytes digested in a different order hash differently, which is
// intended (draw order is part of what is being checked).
//
// Enabled by REX_TEE_LOG=<path>; without it Digest() is a load and a branch.
// Line format frozen in docs/native/FORMATS.md ("Tee digest").

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rex::tee {

// True when REX_TEE_LOG names an output file. Cached after the first call.
bool Enabled();

// Frame counter shared by every channel: the presenting hook calls
// EndFrame() once per present, which publishes every channel and advances
// the frame. Channels may also be published explicitly.
uint64_t CurrentFrame();
void EndFrame();

class Channel {
 public:
  // `name` prefixes every key ("d3d" + "draw" -> "d3d/draw"). Must outlive
  // the channel; a string literal does.
  explicit Channel(const char* name);
  ~Channel();
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  // Fold `len` bytes into the digest of `key` for `frame`. A frame number
  // lower than the channel's current frame is folded into the current one
  // (late arrivals from another thread); a higher one publishes the current
  // frame first. Thread-safe.
  void Digest(uint64_t frame, std::string_view key, const void* bytes, size_t len);
  void Digest(std::string_view key, const void* bytes, size_t len) {
    Digest(CurrentFrame(), key, bytes, len);
  }
  template <typename T>
  void DigestValue(uint64_t frame, std::string_view key, const T& value) {
    Digest(frame, key, &value, sizeof(T));
  }

  // Write one line per key digested in the current frame (keys sorted) and
  // reset for the next frame. No-op when nothing was digested.
  void Publish();

  const char* name() const { return name_; }

 private:
  struct Impl;
  const char* name_;
  Impl* impl_;
};

// Flush the log file. Called from EndFrame and at exit.
void Flush();

}  // namespace rex::tee
