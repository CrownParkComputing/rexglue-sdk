/**
 * @file        include/rex/audio/native_mix.h
 * @brief       Mix decoded XMA natively, bypassing the guest's own mixer.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

namespace rex::audio {

// True when --audio_native_xma is on. Read on the decode path, so cheap.
bool NativeMixEnabled();

// Called as each XMA context produces decoded PCM. `samples` is interleaved
// 16-bit at `rate` Hz with `channels` channels - the context's own format,
// before the guest sees any of it.
void NativeMixPush(uint32_t context_id, const int16_t* samples, size_t frames,
                   uint32_t rate, uint32_t channels);

// Fills one 256-sample, 6-channel guest frame (channel-sequential, big-endian
// floats, exactly as the guest would have submitted) from whatever the
// contexts have produced. Returns false when nothing is playing, in which case
// the caller should submit the guest's own frame unchanged.
bool NativeMixFill(float* frame_out, size_t channel_samples, size_t channels);

}  // namespace rex::audio
