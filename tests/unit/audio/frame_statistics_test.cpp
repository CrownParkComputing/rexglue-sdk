#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include <rex/audio/frame_statistics.h>

namespace {

float GuestFloat(float value) {
  return rex::byte_swap(value);
}

}  // namespace

TEST_CASE("audio frame statistics distinguish silence from signal", "[audio]") {
  rex::audio::AudioFrameStatistics statistics;
  std::array<float, 6 * 256> frame = {};

  statistics.ObserveSequentialBigEndian(frame.data(), 6, 256);
  CHECK(statistics.snapshot().submitted_frames == 1);
  CHECK(statistics.snapshot().frames_with_signal == 0);
  CHECK(statistics.snapshot().peak() == 0.0f);

  frame[0 * 256 + 7] = GuestFloat(-0.25f);
  frame[4 * 256 + 9] = GuestFloat(0.75f);
  statistics.ObserveSequentialBigEndian(frame.data(), 6, 256);
  CHECK(statistics.snapshot().submitted_frames == 2);
  CHECK(statistics.snapshot().frames_with_signal == 1);
  CHECK(statistics.snapshot().channel_peaks[0] == 0.25f);
  CHECK(statistics.snapshot().channel_peaks[4] == 0.75f);
  CHECK(statistics.snapshot().peak() == 0.75f);
}

TEST_CASE("audio frame statistics report Xbox frame duration", "[audio]") {
  rex::audio::AudioFrameStatistics statistics;
  std::array<float, 6 * 256> frame = {};
  for (size_t i = 0; i < 1875; ++i) {
    statistics.ObserveSequentialBigEndian(frame.data(), 6, 256);
  }
  CHECK(statistics.snapshot().seconds() == 10.0);
}

TEST_CASE("audio frame statistics detect replayed signal but ignore silence", "[audio]") {
  rex::audio::AudioFrameStatistics statistics;
  std::array<float, 6 * 256> frame = {};

  statistics.ObserveSequentialBigEndian(frame.data(), 6, 256);
  statistics.ObserveSequentialBigEndian(frame.data(), 6, 256);
  CHECK(statistics.snapshot().identical_signal_frames == 0);
  CHECK(statistics.snapshot().maximum_identical_signal_run == 0);

  frame[17] = GuestFloat(0.5f);
  statistics.ObserveSequentialBigEndian(frame.data(), 6, 256);
  statistics.ObserveSequentialBigEndian(frame.data(), 6, 256);
  statistics.ObserveSequentialBigEndian(frame.data(), 6, 256);
  CHECK(statistics.snapshot().identical_signal_frames == 2);
  CHECK(statistics.snapshot().maximum_identical_signal_run == 3);

  frame[18] = GuestFloat(-0.25f);
  statistics.ObserveSequentialBigEndian(frame.data(), 6, 256);
  CHECK(statistics.snapshot().identical_signal_frames == 2);
  CHECK(statistics.snapshot().maximum_identical_signal_run == 3);
}

TEST_CASE("audio frame statistics expose invalid clipped and discontinuous PCM", "[audio]") {
  rex::audio::AudioFrameStatistics statistics;
  std::array<float, 6 * 4> frame = {};

  frame[0] = GuestFloat(0.25f);
  frame[1] = GuestFloat(1.25f);
  frame[2] = GuestFloat(-0.25f);
  frame[3] = GuestFloat(std::numeric_limits<float>::infinity());
  statistics.ObserveSequentialBigEndian(frame.data(), 6, 4);

  const auto& snapshot = statistics.snapshot();
  CHECK(snapshot.nonfinite_samples == 1);
  CHECK(snapshot.clipped_samples == 1);
  CHECK(snapshot.large_sample_jumps == 1);
  CHECK(snapshot.maximum_sample_jump == 1.5f);
}

TEST_CASE("audio queue statistics separate startup and playback underflows",
          "[audio][queue]") {
  rex::audio::AudioQueueStatistics statistics;

  CHECK_FALSE(statistics.ObserveUnderflow());
  statistics.ObserveSubmitted(1);
  statistics.ObserveSubmitted(2);
  statistics.ObserveConsumed(1);
  statistics.ObserveSubmitted(2);
  statistics.ObserveConsumed(1);
  statistics.ObserveConsumed(0);
  CHECK(statistics.ObserveUnderflow());
  CHECK_FALSE(statistics.ObserveUnderflow());

  const auto& snapshot = statistics.snapshot();
  CHECK(snapshot.submitted_frames == 3);
  CHECK(snapshot.consumed_frames == 3);
  CHECK(snapshot.startup_underflows == 1);
  CHECK(snapshot.playback_underflows == 2);
  CHECK(snapshot.minimum_playback_depth == 0);
  CHECK(snapshot.maximum_depth == 2);
}
