#include <array>

#include <catch2/catch_test_macros.hpp>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>

TEST_CASE("XMA decoder selection is explicit and safely defaults to new", "[audio][xma]") {
  using rex::audio::ResolveXmaDecoderKind;
  using rex::audio::XmaDecoderKind;

  CHECK(ResolveXmaDecoderKind("old") == XmaDecoderKind::kLegacy);
  CHECK(ResolveXmaDecoderKind("new") == XmaDecoderKind::kNew);
  CHECK(ResolveXmaDecoderKind("") == XmaDecoderKind::kNew);
  CHECK(ResolveXmaDecoderKind("invalid") == XmaDecoderKind::kNew);
}

TEST_CASE("XMA exhausted one-shot cannot replay stale PCM", "[audio][xma]") {
  std::array<uint32_t, sizeof(rex::audio::XMA_CONTEXT_DATA) / sizeof(uint32_t)> storage = {};
  rex::audio::XMA_CONTEXT_DATA data(storage.data());
  data.output_buffer_valid = 1;
  data.output_buffer_read_offset = 9;
  data.output_buffer_write_offset = 3;

  rex::audio::FinalizeXmaOutputBufferState(&data, false, 7, 4, false);

  CHECK(data.output_buffer_write_offset == 9);
  CHECK(data.output_buffer_valid == 0);
}

TEST_CASE("XMA active input publishes the decoder ring write offset", "[audio][xma]") {
  std::array<uint32_t, sizeof(rex::audio::XMA_CONTEXT_DATA) / sizeof(uint32_t)> storage = {};
  rex::audio::XMA_CONTEXT_DATA data(storage.data());
  data.output_buffer_valid = 1;
  data.output_buffer_read_offset = 9;
  data.output_buffer_write_offset = 3;

  rex::audio::FinalizeXmaOutputBufferState(&data, true, 7, 4, true);

  CHECK(data.output_buffer_write_offset == 7);
  CHECK(data.output_buffer_valid == 1);
}

TEST_CASE("XMA invalidates only a confirmed full output ring", "[audio][xma]") {
  std::array<uint32_t, sizeof(rex::audio::XMA_CONTEXT_DATA) / sizeof(uint32_t)> storage = {};
  rex::audio::XMA_CONTEXT_DATA data(storage.data());
  data.output_buffer_valid = 1;

  rex::audio::FinalizeXmaOutputBufferState(&data, true, 5, 0, false);
  CHECK(data.output_buffer_valid == 1);

  rex::audio::FinalizeXmaOutputBufferState(&data, true, 5, 0, true);
  CHECK(data.output_buffer_valid == 0);
}
