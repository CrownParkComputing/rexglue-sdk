// xenon-native: SDL3 audio sink (see sdl_output.h).
#include "audio/sdl_output.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <algorithm>
#include <cmath>

namespace xn::audio {
namespace { std::atomic<float> master_volume{1.f}; }
void SetMasterVolume(float value) {
  if (std::isfinite(value)) master_volume.store(std::clamp(value,0.f,1.f),std::memory_order_relaxed);
}
float MasterVolume() { return master_volume.load(std::memory_order_relaxed); }

SdlOutput::~SdlOutput() { Close(); }

bool SdlOutput::Open() {
  if (stream_) return true;
  if (!SDL_WasInit(SDL_INIT_AUDIO)) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      std::fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
      return false;
    }
    sdl_audio_initialized_ = true;
  }
  SDL_AudioSpec spec{};
  spec.format = SDL_AUDIO_F32;
  spec.channels = int(kOutputChannels);
  spec.freq = int(kOutputSampleRate);
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream_) {
    std::fprintf(stderr, "[audio] no playback device: %s\n", SDL_GetError());
    return false;
  }
  SDL_ResumeAudioStreamDevice(stream_);
  // Say exactly what opened and in what format: "no sound at all" is most
  // often the device, and this is the line that rules it in or out.
  const SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(stream_);
  SDL_AudioSpec got{};
  int frames = 0;
  SDL_GetAudioDeviceFormat(dev, &got, &frames);
  const char* name = SDL_GetAudioDeviceName(dev);
  std::fprintf(stderr,
               "[audio] SDL device %u '%s' via %s: %d ch %d Hz format %#x, buffer %d frames; "
               "stream wants %u ch %u Hz f32\n",
               unsigned(dev), name ? name : "?",
               SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", got.channels,
               got.freq, unsigned(got.format), frames, kOutputChannels, kOutputSampleRate);
  if (SDL_AudioStreamDevicePaused(stream_))
    std::fprintf(stderr, "[audio] WARNING: the device is paused after resume\n");
  return true;
}

void SdlOutput::Close() {
  applied_gain_=-1;
  if (stream_) {
    SDL_DestroyAudioStream(stream_);  // also closes the device it opened
    stream_ = nullptr;
  }
  if (sdl_audio_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_audio_initialized_ = false;
  }
}

void SdlOutput::Submit(const float* interleaved, uint32_t frames) {
  submitted_.fetch_add(1, std::memory_order_relaxed);
  if (!stream_) return;
  const float gain=MasterVolume();
  if (gain!=applied_gain_ && SDL_SetAudioStreamGain(stream_,gain)) applied_gain_=gain;
  if (!SDL_PutAudioStreamData(stream_, interleaved, int(frames * kOutputChannels * sizeof(float)))) {
    if (!put_failed_.exchange(true, std::memory_order_relaxed))
      std::fprintf(stderr, "[audio] SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
  }
}

// How many frames the device has actually consumed: what we handed the stream
// minus what is still queued. If this stays at zero while Submit keeps being
// called, the guest is mixing and we are folding but nothing is being played.
uint64_t SdlOutput::FramesDrained() const {
  const uint64_t put = submitted_.load(std::memory_order_relaxed);
  return put > QueuedFrames() ? put - QueuedFrames() : 0;
}

uint32_t SdlOutput::QueuedFrames() const {
  if (!stream_) return 0;
  const int bytes = SDL_GetAudioStreamQueued(stream_);
  return bytes <= 0 ? 0 : uint32_t(bytes) / (kFrameSamples * kOutputChannels * sizeof(float));
}

}  // namespace xn::audio
