// xenon-native: the XAudio render driver client, as the title sees it through
// XAudioRegisterRenderDriverClient / XAudioSubmitRenderDriverFrame.
//
// On the console the audio hardware pulls one 256-sample frame every
// 5.333 ms (187.5 Hz) and calls the registered client back to mix it; the
// client answers with XAudioSubmitRenderDriverFrame(driver, samples) from
// inside that callback. Here a pump thread with guest-callable state (r13,
// stack) does the calling, spaced evenly on a steady clock, gated by how much
// the host device still has queued. The title's mixer paces itself on the
// callback count, so the calls must not bunch up: one per slot, never more.
//
// Thread creation and guest calls are injected so this file does not depend
// on the kernel; runtime/audio/imports_audio.cpp wires the real ones.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "audio/sdl_output.h"

namespace xn::audio {

constexpr uint32_t kRenderChannels = 6;             // FL FR FC LFE BL BR
constexpr uint32_t kRenderFrameBytes = kFrameSamples * kRenderChannels * 4;
constexpr uint32_t kRenderDriverHandleBase = 0x41550000;  // what the SDK hands out
constexpr uint32_t kMaxClients = 4;
constexpr std::chrono::nanoseconds kFramePeriod{5333333};  // 256 / 48000 s
constexpr uint32_t kMaxQueuedFrames = 8;            // ~43 ms of device queue

struct RenderDriverHost {
  // Starts a thread that can run guest code and runs body on it.
  std::function<void(const char* name, std::function<void()> body)> spawn_guest_thread;
  // Calls guest function fn(arg) on the current (guest-capable) thread.
  std::function<void(uint32_t fn, uint32_t arg)> call_guest;
  // Host pointer for a guest address (for the submitted frame).
  std::function<const uint8_t*(uint32_t guest)> translate;
  // Optional tap on the folded stereo frame, before the device (capture,
  // tests). Called on the pump thread; keep it cheap.
  std::function<void(const float* interleaved, uint32_t frames)> on_output;
  // Runs on the guest-capable pump before the title mixer; system music can
  // submit a captured PCM packet to the title's own voice here.
  std::function<void()> before_render;
  std::function<void(float* stereo, uint32_t frames)> mix_output;
  bool headless = false;  // no device: still pump, drop audio
};

class RenderDriver {
 public:
  void Setup(RenderDriverHost host);
  void Shutdown();

  // XAudioRegisterRenderDriverClient: returns the driver handle (0x4155xxxx)
  // or 0 when the client table is full.
  // Retain callback storage until any in-flight invocation has returned.
  uint32_t RegisterClient(uint32_t callback, uint32_t callback_arg,
                          std::shared_ptr<void> callback_storage = {});
  void UnregisterClient(uint32_t handle);
  // XAudioSubmitRenderDriverFrame: samples is the guest address of 256 x 6
  // big-endian floats, channel-sequential (all 256 of FL, then FR, ...).
  void SubmitFrame(uint32_t handle, uint32_t samples_guest);

  uint64_t callbacks_dispatched() const { return dispatched_.load(std::memory_order_relaxed); }
  uint64_t slots_skipped() const { return skipped_.load(std::memory_order_relaxed); }
  uint64_t slots_late() const { return late_.load(std::memory_order_relaxed); }
  // Longest gap between two callbacks since the last call, in microseconds.
  uint32_t TakeMaxGapUs() { return max_gap_us_.exchange(0, std::memory_order_relaxed); }
  // Peak absolute sample seen in the folded output since the last call, and
  // how many of those frames were pure silence.
  void TakeSinkLevel(float* out_peak, uint32_t* out_silent_frames);
  uint64_t frames_submitted() const { return output_.frames_submitted(); }
  uint32_t queued_frames() const { return output_.QueuedFrames(); }
  uint64_t frames_drained() const { return output_.FramesDrained(); }
  bool device_open() const { return output_.open(); }

 private:
  struct Client {
    uint32_t callback = 0;
    uint32_t callback_arg = 0;
    bool in_use = false;
    std::shared_ptr<void> callback_storage;
  };
  void PumpMain();
  void Fold(const uint8_t* frame_be, float* stereo_out);

  RenderDriverHost host_;
  SdlOutput output_;
  std::mutex lock_;
  Client clients_[kMaxClients];
  std::atomic<bool> running_{false};
  std::atomic<bool> pump_started_{false};
  std::atomic<bool> pump_exited_{false};
  std::atomic<uint64_t> dispatched_{0};
  std::atomic<uint64_t> skipped_{0}, late_{0};
  std::atomic<uint32_t> max_gap_us_{0};
  std::atomic<uint32_t> sink_peak_bits_{0}, sink_silent_{0};
  std::atomic<uint32_t> client_count_{0};
};

}  // namespace xn::audio
