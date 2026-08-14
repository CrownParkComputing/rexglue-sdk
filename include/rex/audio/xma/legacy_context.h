/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified Tom Clay, 2026 - Adapted as a selectable ReXGlue legacy decoder.
 */

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <tuple>

#include <rex/audio/xma/context.h>

namespace rex::audio {

class XmaLegacyContext final : public XmaContextInterface {
 public:
  static constexpr uint32_t kBytesPerPacket = 2048;
  static constexpr uint32_t kBytesPerPacketHeader = 4;
  static constexpr uint32_t kBytesPerPacketData = kBytesPerPacket - kBytesPerPacketHeader;
  static constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
  static constexpr uint32_t kBitsPerHeader = 32;
  static constexpr uint32_t kBytesPerSample = 2;
  static constexpr uint32_t kSamplesPerFrame = 512;
  static constexpr uint32_t kSamplesPerSubframe = 128;
  static constexpr uint32_t kBytesPerFrameChannel = kSamplesPerFrame * kBytesPerSample;
  static constexpr uint32_t kBytesPerSubframeChannel = kSamplesPerSubframe * kBytesPerSample;

  XmaLegacyContext();
  ~XmaLegacyContext() override;

  int Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) override;
  bool Work() override;
  void Enable() override;
  bool Block(bool poll) override {
    std::unique_lock<std::mutex> lock(lock_, std::try_to_lock);
    if (!lock.owns_lock()) {
      if (poll) {
        return false;
      }
      lock.lock();
    }
    return true;
  }
  void Clear() override;
  void Disable() override;
  void Release() override;

  uint32_t guest_ptr() override { return guest_ptr_; }
  bool is_allocated() override { return is_allocated_.load(std::memory_order_acquire); }
  void set_is_allocated(bool value) override {
    is_allocated_.store(value, std::memory_order_release);
  }
  void SignalWorkDone() override {
    if (work_completion_event_) {
      work_completion_event_->Set();
    }
  }

 private:
  bool is_enabled() const { return is_enabled_.load(std::memory_order_acquire); }
  void set_is_enabled(bool value) { is_enabled_.store(value, std::memory_order_release); }
  memory::Memory* memory() const { return memory_; }
  uint32_t id() const { return id_; }

  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  static void NextPacket(XMA_CONTEXT_DATA* data);
  static bool TrySetupNextLoop(XMA_CONTEXT_DATA* data, bool ignore_input_buffer_offset);
  static int GetSampleRate(int id);
  static size_t GetNextFrame(uint8_t* block, size_t size, size_t bit_offset);
  static int GetFramePacketNumber(uint8_t* block, size_t size, size_t bit_offset);
  static std::tuple<int, int> GetFrameNumber(uint8_t* block, size_t size, size_t bit_offset);
  static std::tuple<int, bool> GetPacketFrameCount(uint8_t* packet);
  bool ValidFrameOffset(uint8_t* block, size_t size_bytes, size_t frame_offset_bits);
  void Decode(XMA_CONTEXT_DATA* data);
  int PrepareDecoder(uint8_t* packet, int sample_rate, bool is_two_channel);
  uint32_t GetPacketFirstFrameOffset(const XMA_CONTEXT_DATA* data);

  memory::Memory* memory_ = nullptr;
  std::unique_ptr<rex::thread::Event> work_completion_event_;
  uint32_t id_ = 0;
  uint32_t guest_ptr_ = 0;
  std::mutex lock_;
  std::atomic<bool> is_allocated_ = false;
  std::atomic<bool> is_enabled_ = false;

  AVPacket* av_packet_ = nullptr;
  AVCodec* av_codec_ = nullptr;
  AVCodecContext* av_context_ = nullptr;
  AVFrame* av_frame_ = nullptr;

  uint32_t packets_skip_ = 0;
  bool is_stream_done_ = false;
  uint32_t split_frame_len_ = 0;
  uint32_t split_frame_len_partial_ = 0;
  uint8_t split_frame_padding_start_ = 0;
  std::array<uint8_t, 1 + 4096> xma_frame_{};
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw_frame_{};
};

}  // namespace rex::audio
