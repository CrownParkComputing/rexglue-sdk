#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <rex/types.h>

namespace rex::audio {

struct AudioFrameStatisticsSnapshot {
  uint64_t submitted_frames = 0;
  uint64_t frames_with_signal = 0;
  uint64_t identical_signal_frames = 0;
  uint64_t maximum_identical_signal_run = 0;
  uint64_t nonfinite_samples = 0;
  uint64_t clipped_samples = 0;
  uint64_t large_sample_jumps = 0;
  float maximum_sample_jump = 0.0f;
  std::array<float, 6> channel_peaks = {};

  float peak() const { return *std::max_element(channel_peaks.begin(), channel_peaks.end()); }

  double seconds(uint32_t samples_per_channel = 256, uint32_t sample_rate = 48000) const {
    return sample_rate ? double(submitted_frames * samples_per_channel) / double(sample_rate) : 0.0;
  }
};

struct AudioQueueStatisticsSnapshot {
  uint64_t submitted_frames = 0;
  uint64_t consumed_frames = 0;
  uint64_t startup_underflows = 0;
  uint64_t playback_underflows = 0;
  size_t minimum_playback_depth = 0;
  size_t maximum_depth = 0;
};

class AudioQueueStatistics {
 public:
  void ObserveSubmitted(size_t queue_depth) {
    ++snapshot_.submitted_frames;
    snapshot_.maximum_depth = std::max(snapshot_.maximum_depth, queue_depth);
  }

  void ObserveConsumed(size_t queue_depth) {
    ++snapshot_.consumed_frames;
    snapshot_.minimum_playback_depth =
        has_playback_depth_ ? std::min(snapshot_.minimum_playback_depth, queue_depth)
                            : queue_depth;
    has_playback_depth_ = true;
  }

  bool ObserveUnderflow() {
    if (!snapshot_.submitted_frames) {
      ++snapshot_.startup_underflows;
      return false;
    }
    ++snapshot_.playback_underflows;
    return snapshot_.playback_underflows == 1;
  }

  const AudioQueueStatisticsSnapshot& snapshot() const { return snapshot_; }

 private:
  AudioQueueStatisticsSnapshot snapshot_;
  bool has_playback_depth_ = false;
};

class AudioFrameStatistics {
 public:
  void ObserveSequentialBigEndian(const float* samples, size_t channels,
                                  size_t samples_per_channel) {
    ++snapshot_.submitted_frames;
    bool has_signal = false;
    uint64_t frame_hash = 1469598103934665603ull;
    const size_t measured_channels = std::min(channels, snapshot_.channel_peaks.size());
    for (size_t channel = 0; channel < measured_channels; ++channel) {
      float frame_peak = 0.0f;
      for (size_t sample = 0; sample < samples_per_channel; ++sample) {
        const size_t index = channel * samples_per_channel + sample;
        const uint32_t bits = rex::byte_swap(std::bit_cast<uint32_t>(samples[index]));
        frame_hash ^= bits;
        frame_hash *= 1099511628211ull;
        const float value = rex::byte_swap(samples[index]);
        if (!std::isfinite(value)) {
          ++snapshot_.nonfinite_samples;
          has_previous_sample_[channel] = false;
          continue;
        }
        const float magnitude = std::abs(value);
        frame_peak = std::max(frame_peak, magnitude);
        if (magnitude > 1.0f) {
          ++snapshot_.clipped_samples;
        }
        if (has_previous_sample_[channel]) {
          const float jump = std::abs(value - previous_sample_[channel]);
          snapshot_.maximum_sample_jump = std::max(snapshot_.maximum_sample_jump, jump);
          if (jump > 1.0f) {
            ++snapshot_.large_sample_jumps;
          }
        }
        previous_sample_[channel] = value;
        has_previous_sample_[channel] = true;
      }
      snapshot_.channel_peaks[channel] = std::max(snapshot_.channel_peaks[channel], frame_peak);
      has_signal = has_signal || frame_peak > 0.000001f;
    }
    if (has_signal) {
      ++snapshot_.frames_with_signal;
      if (has_previous_signal_hash_ && frame_hash == previous_signal_hash_) {
        ++snapshot_.identical_signal_frames;
        ++current_identical_signal_run_;
      } else {
        current_identical_signal_run_ = 1;
      }
      snapshot_.maximum_identical_signal_run =
          std::max(snapshot_.maximum_identical_signal_run, current_identical_signal_run_);
      previous_signal_hash_ = frame_hash;
      has_previous_signal_hash_ = true;
    } else {
      current_identical_signal_run_ = 0;
      has_previous_signal_hash_ = false;
    }
  }

  const AudioFrameStatisticsSnapshot& snapshot() const { return snapshot_; }

 private:
  AudioFrameStatisticsSnapshot snapshot_;
  uint64_t previous_signal_hash_ = 0;
  uint64_t current_identical_signal_run_ = 0;
  bool has_previous_signal_hash_ = false;
  std::array<float, 6> previous_sample_ = {};
  std::array<bool, 6> has_previous_sample_ = {};
};

}  // namespace rex::audio
