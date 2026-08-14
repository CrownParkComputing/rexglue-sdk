#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/audio/conversion.h>

namespace {

float GuestFloat(float value) {
  return rex::byte_swap(value);
}

}  // namespace

TEST_CASE("six-channel big-endian PCM is interleaved without channel swaps", "[audio]") {
  constexpr size_t kSamples = 4;
  std::array<float, 6 * kSamples> input = {};
  std::array<float, 6 * kSamples> output = {};
  for (size_t channel = 0; channel < 6; ++channel) {
    for (size_t sample = 0; sample < kSamples; ++sample) {
      input[channel * kSamples + sample] =
          GuestFloat(static_cast<float>(channel * 10 + sample));
    }
  }

  rex::audio::conversion::sequential_6_BE_to_interleaved_6_LE(
      output.data(), input.data(), kSamples);

  for (size_t sample = 0; sample < kSamples; ++sample) {
    for (size_t channel = 0; channel < 6; ++channel) {
      CHECK(output[sample * 6 + channel] == static_cast<float>(channel * 10 + sample));
    }
  }
}

TEST_CASE("Xbox 5.1 PCM downmixes to stereo with the Xenia channel matrix", "[audio]") {
  constexpr size_t kSamples = 4;
  std::array<float, 6 * kSamples> input = {};
  std::array<float, 2 * kSamples> output = {};
  for (size_t sample = 0; sample < kSamples; ++sample) {
    input[0 * kSamples + sample] = GuestFloat(0.10f + float(sample) * 0.01f);  // FL
    input[1 * kSamples + sample] = GuestFloat(0.20f + float(sample) * 0.01f);  // FR
    input[2 * kSamples + sample] = GuestFloat(0.30f + float(sample) * 0.01f);  // FC
    input[3 * kSamples + sample] = GuestFloat(0.90f);                          // LFE ignored
    input[4 * kSamples + sample] = GuestFloat(0.40f + float(sample) * 0.01f);  // BL
    input[5 * kSamples + sample] = GuestFloat(0.50f + float(sample) * 0.01f);  // BR
  }

  rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(
      output.data(), input.data(), kSamples);

  for (size_t sample = 0; sample < kSamples; ++sample) {
    const float center = 0.30f + float(sample) * 0.01f;
    const float expected_left =
        (0.10f + float(sample) * 0.01f + 0.40f + float(sample) * 0.01f + center * 0.5f) /
        2.5f;
    const float expected_right =
        (0.20f + float(sample) * 0.01f + 0.50f + float(sample) * 0.01f + center * 0.5f) /
        2.5f;
    CHECK(output[sample * 2] == Catch::Approx(expected_left));
    CHECK(output[sample * 2 + 1] == Catch::Approx(expected_right));
  }
}

TEST_CASE("front-only stereo diagnostics exclude center LFE and rear channels", "[audio]") {
  constexpr size_t kSamples = 4;
  std::array<float, 6 * kSamples> input = {};
  std::array<float, 2 * kSamples> output = {};
  for (size_t sample = 0; sample < kSamples; ++sample) {
    input[0 * kSamples + sample] = GuestFloat(0.10f + float(sample) * 0.01f);
    input[1 * kSamples + sample] = GuestFloat(0.20f + float(sample) * 0.01f);
    input[2 * kSamples + sample] = GuestFloat(0.90f);
    input[3 * kSamples + sample] = GuestFloat(0.80f);
    input[4 * kSamples + sample] = GuestFloat(0.70f);
    input[5 * kSamples + sample] = GuestFloat(0.60f);
  }

  rex::audio::conversion::sequential_6_BE_front_to_interleaved_2_LE(
      output.data(), input.data(), kSamples);

  for (size_t sample = 0; sample < kSamples; ++sample) {
    CHECK(output[sample * 2] == Catch::Approx(0.10f + float(sample) * 0.01f));
    CHECK(output[sample * 2 + 1] == Catch::Approx(0.20f + float(sample) * 0.01f));
  }
}
