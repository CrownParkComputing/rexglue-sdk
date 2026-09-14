/**
 * @file        src/audio/native_mix.cpp
 * @brief       Mix decoded XMA natively, bypassing the guest's own mixer.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Why this exists.
//
// Hydro Thunder's audio repeats every 1024 samples, and the cause is not the
// decode: measured, the title produces correct, fresh audio at about 93 ring
// refills a second where its own output chain drains 188. Its recompiled mixer
// runs at half real time, so every block is heard twice. Identical with the
// original XMA banks and with PCM ones, so it is not the codec; the guest
// clock is right (ratio 0.9999), the pump is 1:1, the output stage is
// bit-exact, and a from-scratch re-port reproduces it exactly.
//
// We already hold everything that title plays: 34 XMA contexts, 66,118 of
// 66,121 kicks decoded, at the correct rate, before the guest's mixer touches
// any of it. So this takes that decoded audio and mixes it here instead.
//
// What this can and cannot do, stated plainly: the per-voice volume, pan and
// 3D positioning live in the guest's mixer, not in the XMA contexts, so this
// path reproduces the streams at unity gain spread across the front pair. For
// music and stereo ambience - which is what a broken mixer is most audible on -
// that is right. For positional effects it is flat. It is a bypass for titles
// whose recompiled mixer does not work, not a replacement for one that does,
// and it is off unless asked for.

#include <rex/audio/native_mix.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/types.h>

REXCVAR_DEFINE_BOOL(
    audio_native_xma, false, "Audio",
    "Mix decoded XMA here and submit that instead of the guest's own mix.\n"
    "For a title whose recompiled mixer does not keep up - Hydro Thunder produces correct audio at "
    "half real time, so every block is heard twice - this bypasses it entirely: the streams we "
    "already decode are resampled and summed here. Per-voice volume, pan and 3D live in the guest "
    "mixer and are not reproduced, so effects are flat while music and ambience are correct.");
REXCVAR_DEFINE_UINT32(audio_native_xma_queue_ms, 120, "Audio",
                      "How much decoded audio each XMA context may bank before the oldest is "
                      "dropped. Too small underruns; too large adds latency.");

namespace rex::audio {

namespace {

constexpr uint32_t kOutputRate = 48000;

// One decoded stream, kept in its own sample rate until the mix pulls from it.
struct Source {
  std::deque<float> left;
  std::deque<float> right;
  uint32_t rate = kOutputRate;
  uint32_t channels = 2;
  double position = 0.0;   // fractional read cursor, in source samples
  uint64_t last_push = 0;  // mix ticks since this source last received audio
};

std::mutex g_lock;
std::map<uint32_t, Source> g_sources;
uint64_t g_ticks = 0;

}  // namespace

bool NativeMixEnabled() {
  static const bool enabled = REXCVAR_GET(audio_native_xma);
  return enabled;
}

void NativeMixPush(uint32_t context_id, const int16_t* samples, size_t frames, uint32_t rate,
                   uint32_t channels) {
  if (!NativeMixEnabled() || !samples || !frames || !channels) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_lock);
  auto& source = g_sources[context_id];
  if (source.rate != rate || source.channels != channels) {
    // A context is reused for different streams; starting clean beats mixing
    // the tail of one rate into the head of another.
    source.left.clear();
    source.right.clear();
    source.position = 0.0;
    source.rate = rate ? rate : kOutputRate;
    source.channels = channels;
  }
  for (size_t i = 0; i < frames; ++i) {
    const float l = float(samples[i * channels]) / 32768.0f;
    const float r = channels > 1 ? float(samples[i * channels + 1]) / 32768.0f : l;
    source.left.push_back(l);
    source.right.push_back(r);
  }
  source.last_push = g_ticks;

  const size_t cap = size_t(source.rate) * REXCVAR_GET(audio_native_xma_queue_ms) / 1000;
  while (source.left.size() > cap) {
    source.left.pop_front();
    source.right.pop_front();
    if (source.position > 0.0) {
      source.position -= 1.0;
    }
  }
}

bool NativeMixFill(float* frame_out, size_t channel_samples, size_t channels) {
  if (!NativeMixEnabled() || !frame_out || !channel_samples || channels < 2) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_lock);
  ++g_ticks;

  std::vector<float> left(channel_samples, 0.0f);
  std::vector<float> right(channel_samples, 0.0f);
  bool any = false;

  for (auto it = g_sources.begin(); it != g_sources.end();) {
    Source& s = it->second;
    // A context that has produced nothing for a while is finished; dropping it
    // stops a stopped sound being held at its last position forever.
    if (g_ticks - s.last_push > 400) {
      it = g_sources.erase(it);
      continue;
    }
    const double step = double(s.rate) / double(kOutputRate);
    for (size_t i = 0; i < channel_samples; ++i) {
      const size_t index = size_t(s.position);
      if (index + 1 >= s.left.size()) {
        break;  // underrun: leave the rest of this frame silent for this source
      }
      // Linear interpolation is enough for a bypass path; the alternative is
      // audible aliasing on the 27 kHz and 32 kHz streams these titles use.
      const double frac = s.position - double(index);
      left[i] += float(s.left[index] * (1.0 - frac) + s.left[index + 1] * frac);
      right[i] += float(s.right[index] * (1.0 - frac) + s.right[index + 1] * frac);
      s.position += step;
      any = true;
    }
    // Retire what has been consumed, keeping one sample for interpolation.
    const size_t consumed = s.position > 1.0 ? size_t(s.position) - 1 : 0;
    if (consumed) {
      s.left.erase(s.left.begin(), s.left.begin() + std::min(consumed, s.left.size()));
      s.right.erase(s.right.begin(), s.right.begin() + std::min(consumed, s.right.size()));
      s.position -= double(consumed);
    }
    ++it;
  }

  if (!any) {
    return false;
  }

  // The guest's frame layout: channel-sequential, big-endian floats. Front pair
  // carries the mix; the rest stay silent rather than being faked.
  std::memset(frame_out, 0, channel_samples * channels * sizeof(float));
  for (size_t i = 0; i < channel_samples; ++i) {
    const float l = std::clamp(left[i], -1.0f, 1.0f);
    const float r = std::clamp(right[i], -1.0f, 1.0f);
    frame_out[0 * channel_samples + i] = rex::byte_swap(l);
    frame_out[1 * channel_samples + i] = rex::byte_swap(r);
  }
  return true;
}

}  // namespace rex::audio
