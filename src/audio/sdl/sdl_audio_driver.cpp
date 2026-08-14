/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_BOOL(audio_stereo_front_only, false, "Audio",
                    "Diagnostic stereo output using only front-left and front-right");
REXCVAR_DEFINE_STRING(audio_diagnostic_dump, "", "Audio",
                      "Write at most 20 seconds of pre-SDL stereo float PCM for diagnostics");

namespace rex::audio::sdl {

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Prevent SDL from interfering with timer resolution (causes FPS drops)
  SDL_SetHintWithPriority(SDL_HINT_TIMER_RESOLUTION, "0", SDL_HINT_OVERRIDE);

  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  const std::string diagnostic_path = REXCVAR_GET(audio_diagnostic_dump);
  if (!diagnostic_path.empty()) {
    diagnostic_dump_.open(diagnostic_path, std::ios::binary | std::ios::trunc);
    if (!diagnostic_dump_) {
      REXAPU_ERROR("Unable to open pre-SDL PCM diagnostic dump: {}", diagnostic_path);
    } else {
      REXAPU_INFO("Capturing up to 20 seconds of pre-SDL stereo float PCM: {}",
                  diagnostic_path);
    }
  }

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  if (obtained_spec.channels == 2) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                            SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      return false;
    }
  }

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  statistics_.ObserveSequentialBigEndian(input_frame, frame_channels_, channel_samples_);
  const auto& statistics = statistics_.snapshot();
  if (!signal_evidence_logged_ && statistics.frames_with_signal > 0) {
    signal_evidence_logged_ = true;
    REXAPU_INFO("audio frames submitted: {} ({:.1f} s), {} with signal, peak {:.6f}",
                statistics.submitted_frames, statistics.seconds(), statistics.frames_with_signal,
                statistics.peak());
    REXAPU_INFO("per channel peak: 0={:.6f} 1={:.6f} 2={:.6f} 3={:.6f} 4={:.6f} 5={:.6f}",
                statistics.channel_peaks[0], statistics.channel_peaks[1],
                statistics.channel_peaks[2], statistics.channel_peaks[3],
                statistics.channel_peaks[4], statistics.channel_peaks[5]);
  }
  if (statistics.submitted_frames >= next_integrity_log_frame_) {
    REXAPU_INFO(
        "audio integrity at {:.1f} s: peak {:.6f}, {} non-finite, {} clipped, {} large "
        "sample jumps (max {:.6f}), {} identical signal frames (max run {})",
        statistics.seconds(), statistics.peak(), statistics.nonfinite_samples,
        statistics.clipped_samples, statistics.large_sample_jumps,
        statistics.maximum_sample_jump, statistics.identical_signal_frames,
        statistics.maximum_identical_signal_run);
    REXAPU_INFO(
        "audio channel peaks at {:.1f} s: FL={:.6f} FR={:.6f} FC={:.6f} "
        "LFE={:.6f} BL={:.6f} BR={:.6f}",
        statistics.seconds(), statistics.channel_peaks[0],
        statistics.channel_peaks[1], statistics.channel_peaks[2],
        statistics.channel_peaks[3], statistics.channel_peaks[4],
        statistics.channel_peaks[5]);
    next_integrity_log_frame_ += 938;
  }
  constexpr uint64_t kMaximumDiagnosticFrames =
      (20ull * frame_frequency_) / channel_samples_;
  if (diagnostic_dump_ && diagnostic_dumped_frames_ < kMaximumDiagnosticFrames) {
    conversion::sequential_6_BE_to_interleaved_2_LE(
        diagnostic_stereo_.data(), input_frame, channel_samples_);
    diagnostic_dump_.write(
        reinterpret_cast<const char*>(diagnostic_stereo_.data()),
        static_cast<std::streamsize>(diagnostic_stereo_.size() * sizeof(float)));
    ++diagnostic_dumped_frames_;
    if (diagnostic_dumped_frames_ == kMaximumDiagnosticFrames) {
      diagnostic_dump_.flush();
      REXAPU_INFO("Completed 20-second pre-SDL PCM diagnostic dump");
    }
  }

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    queue_statistics_.ObserveSubmitted(frames_queued_.size());
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}

void SDLAudioDriver::Shutdown() {
  const auto& statistics = statistics_.snapshot();
  REXAPU_INFO(
      "audio final: {} frames ({:.1f} s), {} signal, peak {:.6f}, {} identical signal "
      "frames, maximum identical run {}, {} non-finite, {} clipped, {} large sample "
      "jumps (max {:.6f})",
      statistics.submitted_frames, statistics.seconds(), statistics.frames_with_signal,
      statistics.peak(), statistics.identical_signal_frames,
      statistics.maximum_identical_signal_run, statistics.nonfinite_samples,
      statistics.clipped_samples, statistics.large_sample_jumps,
      statistics.maximum_sample_jump);
  if (diagnostic_dump_.is_open()) {
    diagnostic_dump_.close();
    REXAPU_INFO("pre-SDL PCM diagnostic final: {} frames ({:.1f} s)",
                diagnostic_dumped_frames_,
                double(diagnostic_dumped_frames_ * channel_samples_) /
                    double(frame_frequency_));
  }
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  const auto& queue_statistics = queue_statistics_.snapshot();
  REXAPU_INFO(
      "audio queue final: {} submitted, {} consumed, {} startup underflows, {} playback "
      "underflows, depth min={} max={}",
      queue_statistics.submitted_frames, queue_statistics.consumed_frames,
      queue_statistics.startup_underflows, queue_statistics.playback_underflows,
      queue_statistics.minimum_playback_depth, queue_statistics.maximum_depth);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      const bool first_playback_underflow = driver->queue_statistics_.ObserveUnderflow();
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      if (first_playback_underflow) {
        REXAPU_WARN("SDL audio queue underflow after playback started; inserting silence");
      }
      additional_amount -= len;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      driver->queue_statistics_.ObserveConsumed(driver->frames_queued_.size());
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            if (REXCVAR_GET(audio_stereo_front_only)) {
              conversion::sequential_6_BE_front_to_interleaved_2_LE(
                  data, buffer, channel_samples_);
            } else {
              conversion::sequential_6_BE_to_interleaved_2_LE(
                  data, buffer, channel_samples_);
            }
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
      additional_amount -= len;
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
