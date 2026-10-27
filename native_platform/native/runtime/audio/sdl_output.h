// xenon-native: host audio sink. Takes the 48 kHz stereo float frames the
// render driver folds down from the title's 5.1 output and hands them to an
// SDL3 audio stream; reports how much is queued so the pump can pace itself.
#pragma once
#include <atomic>
#include <cstdint>

struct SDL_AudioStream;

namespace xn::audio {
void SetMasterVolume(float volume);
float MasterVolume();

constexpr uint32_t kOutputSampleRate = 48000;
constexpr uint32_t kOutputChannels = 2;
constexpr uint32_t kFrameSamples = 256;  // one render-driver frame (5.333 ms)

class SdlOutput {
 public:
  ~SdlOutput();
  // Opens the default playback device. Returns false (and stays a sink that
  // drops audio) when no device is available; the pump keeps running so the
  // title's mixer still advances.
  bool Open();
  void Close();
  bool open() const { return stream_ != nullptr; }
  // interleaved stereo, frames * 2 floats
  void Submit(const float* interleaved, uint32_t frames);
  // Render-driver frames still waiting in the device queue.
  uint32_t QueuedFrames() const;
  uint64_t frames_submitted() const { return submitted_.load(std::memory_order_relaxed); }
  // Frames the device has consumed (submitted minus still queued).
  uint64_t FramesDrained() const;

 private:
  SDL_AudioStream* stream_ = nullptr;
  bool sdl_audio_initialized_ = false;
  float applied_gain_ = -1;
  std::atomic<uint64_t> submitted_{0};
  std::atomic<bool> put_failed_{false};
};

}  // namespace xn::audio
