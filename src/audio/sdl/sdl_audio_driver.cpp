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
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <cstring>
#include <vector>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/music_player.h>
#include <rex/audio/native_mix.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/types.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_STRING(
    audio_dump_out_wav, "", "Audio",
    "Write what is handed to SDL - after the fold or passthrough, byte-swapped, scaled and "
    "clamped - to this WAV. --audio_dump_wav captures the guest's submission instead; if that one "
    "measures clean and this one does not, the fault is in the output stage and nowhere else.");
REXCVAR_DEFINE_INT32(
    audio_channels, 0, "Audio",
    "Channels to open the output device with: 0 (the default) follows whatever the device "
    "reports, 2 does our own BS.775 stereo fold, 6 forces 5.1 passthrough.\n"
    "UNTESTED HYPOTHESIS, kept as a flag rather than a default until it is: an HDMI endpoint "
    "wired to stereo speakers still advertises 5.1, so following the device passes the guest's "
    "5.1 through for the host to fold - LFE at unity and full band - which is not a mix anyone "
    "authored. If that is what makes music sound hollow and metallic, 2 should fix it by folding "
    "here, centre and surrounds at -3 dB and LFE dropped.");
REXCVAR_DEFINE_STRING(
    audio_dump_wav, "", "Audio",
    "Write everything the guest submits to this WAV file (32-bit float, 6 channels, 48 kHz, "
    "de-swapped to host order). What reaches the driver is the guest's own decoded audio, so a "
    "dump that already sounds wrong puts the fault in the title or the XMA decoder rather than in "
    "delivery. The header is finished on shutdown; a killed process leaves sizes of 0 which most "
    "players still handle.");

REXCVAR_DEFINE_STRING(
    audio_app_name, "rexglue", "Audio",
    "Application name the audio stream carries (what PipeWire/PulseAudio show and remember "
    "volume and mute against). Every port shares the default; a headless verification run "
    "passes its own so that muting it does not mute the next live run of any title.");

namespace rex::audio::sdl {

namespace {
// Diagnostics, off unless REX_AUDIO_STATS=1 (REX_AUDIO_REPEAT_STATS is kept
// working as the old name). "Slow and robotic" output has two very different
// causes and these separate them: the guest handing us the same block again,
// or the callback finding nothing queued and playing silence between good
// frames. The second is a gap every time the guest thread is scheduled late,
// and at the callback rate it sounds exactly like a buzz.
bool AudioStatsEnabled() {
  static const int enabled = [] {
    const char* value = getenv("REX_AUDIO_STATS");
    if (!value) {
      value = getenv("REX_AUDIO_REPEAT_STATS");
    }
    return (value && *value == '1') ? 1 : 0;
  }();
  return enabled != 0;
}

std::atomic<uint64_t> g_callbacks{0};
std::atomic<uint64_t> g_underruns{0};
std::atomic<uint64_t> g_depth_sum{0};
std::atomic<uint64_t> g_depth_min{~uint64_t(0)};
}  // namespace

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING,
                             REXCVAR_GET(audio_app_name).c_str());

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  const int32_t forced_channels = REXCVAR_GET(audio_channels);
  const uint8_t open_channels =
      (forced_channels == 2 || forced_channels == 6)
          ? static_cast<uint8_t>(forced_channels)
          : static_cast<uint8_t>(frame_channels_);
  desired_spec.channels = open_channels;
  sdl_device_channels_ = open_channels;
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

  // A 1-channel device gets the stereo fold too, then SDL collapses to mono.
  // Handing it a 6ch stream instead would use SDL's own downmix.
  if (forced_channels != 2 && forced_channels != 6 && obtained_spec.channels <= 2) {
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

  // The endpoint layout decides which mix the callback runs, and it is the
  // first thing worth knowing when a report says the balance is wrong on one
  // speaker setup and right on another.
  const char* device_name = SDL_GetAudioDeviceName(sdl_device);
  REXAPU_INFO("audio endpoint '{}': {} ch, {} Hz, format 0x{:04X}; submitting {} ch{}",
              device_name ? device_name : "?", obtained_spec.channels, obtained_spec.freq,
              static_cast<uint32_t>(obtained_spec.format), static_cast<int>(sdl_device_channels_),
              (forced_channels == 2 || forced_channels == 6) ? " (forced by --audio_channels)" : "");

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  return true;
}

namespace {
// Minimal WAVE_FORMAT_IEEE_FLOAT header; sizes are patched on close.
void WriteWavHeader(std::FILE* file, uint16_t channels, uint32_t rate) {
  const uint16_t bits = 32;
  const uint32_t byte_rate = rate * channels * (bits / 8);
  const uint16_t block_align = static_cast<uint16_t>(channels * (bits / 8));
  const uint16_t format = 3;  // IEEE float
  const uint32_t fmt_size = 16;
  const uint32_t zero = 0;
  std::fwrite("RIFF", 1, 4, file);
  std::fwrite(&zero, 4, 1, file);  // patched: RIFF size
  std::fwrite("WAVEfmt ", 1, 8, file);
  std::fwrite(&fmt_size, 4, 1, file);
  std::fwrite(&format, 2, 1, file);
  std::fwrite(&channels, 2, 1, file);
  std::fwrite(&rate, 4, 1, file);
  std::fwrite(&byte_rate, 4, 1, file);
  std::fwrite(&block_align, 2, 1, file);
  std::fwrite(&bits, 2, 1, file);
  std::fwrite("data", 1, 4, file);
  std::fwrite(&zero, 4, 1, file);  // patched: data size
}

// Rewrites the two size fields and returns to the end, so the file on disk is
// valid at all times. A capture run is normally ended with a kill or a timeout,
// which means Shutdown() never runs - patching only there leaves every dump
// with zero sizes, readable by ffmpeg only because it guesses, and rejected or
// misread by anything stricter.
void PatchWavSizes(std::FILE* file) {
  const long end = std::ftell(file);
  if (end < 44) {
    return;
  }
  const uint32_t data_size = static_cast<uint32_t>(end - 44);
  const uint32_t riff_size = static_cast<uint32_t>(end - 8);
  std::fseek(file, 4, SEEK_SET);
  std::fwrite(&riff_size, 4, 1, file);
  std::fseek(file, 40, SEEK_SET);
  std::fwrite(&data_size, 4, 1, file);
  std::fseek(file, 0, SEEK_END);
}

std::mutex g_dump_lock;
std::FILE* g_dump_file = nullptr;
bool g_dump_tried = false;

std::mutex g_out_dump_lock;
std::FILE* g_out_dump_file = nullptr;
bool g_out_dump_tried = false;
}  // namespace

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);

  // --audio_native_xma: replace what the guest mixed with what we mixed from
  // the XMA streams we decoded ourselves. Used for a title whose recompiled
  // mixer does not run at real time; falls through to the guest's own frame
  // whenever nothing is playing through XMA, so a title that mixes correctly
  // is unaffected even with the flag on.
  static std::vector<float> native_frame;
  if (NativeMixEnabled()) {
    native_frame.resize(frame_samples_);
    if (NativeMixFill(native_frame.data(), channel_samples_, frame_channels_)) {
      input_frame = native_frame.data();
    }
  }
  // Title-playlist music (XMP): decoded from the title's own WMA files and
  // added to the front pair here, on top of whatever the guest mixed. See
  // music_player.cpp. Nothing happens unless a playlist is playing.
  static std::vector<float> music_frame;
  if (MusicIsPlaying()) {
    music_frame.resize(frame_samples_);
    std::memcpy(music_frame.data(), input_frame, frame_samples_ * sizeof(float));
    if (MusicMixInto(music_frame.data(), channel_samples_, frame_channels_)) {
      input_frame = music_frame.data();
    }
  }

  if (!REXCVAR_GET(audio_dump_wav).empty()) {
    std::lock_guard<std::mutex> guard(g_dump_lock);
    if (!g_dump_tried) {
      g_dump_tried = true;
      const std::string path = REXCVAR_GET(audio_dump_wav);
      g_dump_file = std::fopen(path.c_str(), "wb");
      if (g_dump_file) {
        WriteWavHeader(g_dump_file, frame_channels_, frame_frequency_);
        REXAPU_INFO("dumping submitted audio to {}", path);
      } else {
        REXAPU_ERROR("could not open {} for the audio dump", path);
      }
    }
    if (g_dump_file) {
      // The guest's frame is channel-SEQUENTIAL big-endian - 256 samples of
      // channel 0, then 256 of channel 1, and so on - while WAV is
      // interleaved. Writing it through unchanged produces a file that sounds
      // like noise no matter how good the audio was, which would send anyone
      // listening to it chasing the wrong fault entirely.
      std::array<float, frame_channels_> interleaved{};
      for (size_t sample = 0; sample < channel_samples_; ++sample) {
        for (size_t channel = 0; channel < frame_channels_; ++channel) {
          interleaved[channel] =
              rex::byte_swap(input_frame[channel * channel_samples_ + sample]);
        }
        std::fwrite(interleaved.data(), sizeof(float), interleaved.size(), g_dump_file);
      }
      static uint32_t in_frames = 0;
      if (++in_frames % 188 == 0) {  // about once a second
        PatchWavSizes(g_dump_file);
      }
    }
  }

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

  // Diagnostic: is the guest handing us the same frame more than once? A
  // repeated block is what "slow and robotic" audio sounds like, and it matters
  // whether the repeat is already in the guest's buffer or appears later in
  // this driver's queue. Off unless REX_AUDIO_REPEAT_STATS=1.
  {
    if (AudioStatsEnabled()) {
      static uint64_t history[8] = {};
      static size_t history_count = 0;
      static uint64_t submitted = 0, repeats = 0, repeat_at_4 = 0;
      uint64_t hash = 1469598103934665603ull;
      const auto* bytes = reinterpret_cast<const unsigned char*>(input_frame);
      for (size_t i = 0; i < frame_samples_ * sizeof(float); ++i) {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
      }
      // A repeated SILENT block and a repeated LOUD one sound nothing alike:
      // the first is a gap, the second is the buzz that gets reported as
      // "robotic". The repeat count alone cannot tell them apart.
      static double peak_sum = 0.0;
      static uint64_t silent_frames = 0;
      float peak = 0.0f;
      for (size_t i = 0; i < frame_samples_; ++i) {
        // Guest samples are big-endian floats; only the magnitude matters here.
        const float sample = rex::byte_swap(input_frame[i]);
        const float magnitude = sample < 0.0f ? -sample : sample;
        if (magnitude > peak) {
          peak = magnitude;
        }
      }
      peak_sum += double(peak);
      if (peak < 1e-6f) {
        ++silent_frames;
      }
      static uint32_t ptr_history[8] = {};
      for (size_t back = 0; back < 8 && back < history_count; ++back) {
        if (history[(history_count - 1 - back) % 8] == hash) {
          ++repeats;
          if (back == 3) {
            ++repeat_at_4;
            // Same guest buffer re-submitted, or a different buffer holding
            // identical audio? The first is a submission/acknowledgement
            // problem in our XAudio path; the second means the guest mixed the
            // same thing twice and the fault is inside the title.
            static uint32_t logged = 0;
            if (logged < 8) {
              ++logged;
              REXAPU_INFO("audio repeat 4 frames back: now {:08X}, then {:08X} - {}", frame_ptr,
                          ptr_history[(history_count - 4) % 8],
                          frame_ptr == ptr_history[(history_count - 4) % 8]
                              ? "SAME buffer re-submitted"
                              : "different buffer, identical audio");
            }
          }
          break;
        }
      }
      ptr_history[history_count % 8] = frame_ptr;
      history[history_count % 8] = hash;
      ++history_count;
      if (++submitted % 188 == 0) {  // ~once a second at 256 samples / 48 kHz
        const uint64_t callbacks = g_callbacks.exchange(0);
        const uint64_t underruns = g_underruns.exchange(0);
        const uint64_t depth_sum = g_depth_sum.exchange(0);
        const uint64_t depth_min = g_depth_min.exchange(~uint64_t(0));
        REXAPU_INFO("audio repeats: {} of {} submitted frames matched one of the previous 8 "
                    "({} of them exactly 4 frames back)",
                    repeats, submitted, repeat_at_4);
        REXAPU_INFO("audio level: peak avg {:.4f} over the last second, {} of {} frames silent",
                    peak_sum / 188.0, silent_frames, submitted);
        peak_sum = 0.0;
        silent_frames = 0;
        REXAPU_INFO("audio queue: {} callbacks, {} played silence ({:.1f}%), depth avg {:.2f} "
                    "min {}",
                    callbacks, underruns,
                    callbacks ? 100.0 * double(underruns) / double(callbacks) : 0.0,
                    callbacks ? double(depth_sum) / double(callbacks) : 0.0,
                    depth_min == ~uint64_t(0) ? 0 : depth_min);
      }
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
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}

void SDLAudioDriver::Shutdown() {
  {
    std::lock_guard<std::mutex> guard(g_dump_lock);
    if (g_dump_file) {
      PatchWavSizes(g_dump_file);
      std::fclose(g_dump_file);
      g_dump_file = nullptr;
    }
  }
  {
    std::lock_guard<std::mutex> guard(g_out_dump_lock);
    if (g_out_dump_file) {
      PatchWavSizes(g_out_dump_file);
      std::fclose(g_out_dump_file);
      g_out_dump_file = nullptr;
    }
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
  // Snapshot once. A change mid-callback would split the frame across two mixes.
  const StereoFold fold = GetStereoFold();
  const SurroundMix mix = GetSurroundMix();
  const float gain = GetOutputGain();
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (AudioStatsEnabled()) {
      const uint64_t depth = driver->frames_queued_.size();
      g_callbacks.fetch_add(1, std::memory_order_relaxed);
      g_depth_sum.fetch_add(depth, std::memory_order_relaxed);
      uint64_t previous = g_depth_min.load(std::memory_order_relaxed);
      while (depth < previous &&
             !g_depth_min.compare_exchange_weak(previous, depth, std::memory_order_relaxed)) {
      }
      if (!depth) {
        g_underruns.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (driver->frames_queued_.empty()) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_, fold,
                                                            gain);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_, mix,
                                                            gain);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      if (!REXCVAR_GET(audio_dump_out_wav).empty()) {
        std::lock_guard<std::mutex> dump_guard(g_out_dump_lock);
        if (!g_out_dump_tried) {
          g_out_dump_tried = true;
          const std::string path = REXCVAR_GET(audio_dump_out_wav);
          g_out_dump_file = std::fopen(path.c_str(), "wb");
          if (g_out_dump_file) {
            WriteWavHeader(g_out_dump_file, driver->sdl_device_channels_, frame_frequency_);
            REXAPU_INFO("dumping device-bound audio to {} ({} ch)", path,
                        int(driver->sdl_device_channels_));
          }
        }
        if (g_out_dump_file) {
          // Already interleaved, host-endian and clamped - exactly the bytes
          // SDL gets, so it needs no rearranging on the way out.
          std::fwrite(data, sizeof(float), static_cast<size_t>(sample_count), g_out_dump_file);
          static uint32_t out_frames = 0;
          if (++out_frames % 188 == 0) {
            PatchWavSizes(g_out_dump_file);
          }
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
