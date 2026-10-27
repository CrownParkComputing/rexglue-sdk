// xenon-native: XAudio render driver pump (see render_driver.h).
#include "audio/render_driver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace xn::audio {

void RenderDriver::Setup(RenderDriverHost host) {
  host_ = std::move(host);
  if (!host_.headless) output_.Open();
  running_ = true;
}

void RenderDriver::Shutdown() {
  running_ = false;
  if (pump_started_) {
    while (!pump_exited_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  output_.Close();
}

uint32_t RenderDriver::RegisterClient(uint32_t callback, uint32_t callback_arg,
                                      std::shared_ptr<void> callback_storage) {
  std::lock_guard<std::mutex> g(lock_);
  for (uint32_t i = 0; i < kMaxClients; i++) {
    if (clients_[i].in_use) continue;
    clients_[i] = {callback, callback_arg, true, std::move(callback_storage)};
    client_count_++;
    if (!pump_started_.exchange(true)) {
      host_.spawn_guest_thread("audio pump", [this] { PumpMain(); });
    }
    return kRenderDriverHandleBase | i;
  }
  return 0;
}

void RenderDriver::UnregisterClient(uint32_t handle) {
  std::lock_guard<std::mutex> g(lock_);
  const uint32_t i = handle & 0xFFFF;
  if ((handle & 0xFFFF0000u) != kRenderDriverHandleBase || i >= kMaxClients || !clients_[i].in_use) return;
  clients_[i] = {};
  client_count_--;
}

// 5.1 -> stereo: centre and surrounds at -3 dB, LFE at -6 dB, then a fixed
// -3 dB so a full-scale front channel alone does not clip after the fold.
void RenderDriver::Fold(const uint8_t* be, float* out) {
  const uint32_t* words = reinterpret_cast<const uint32_t*>(be);
  auto ch = [&](uint32_t c, uint32_t i) {
    const uint32_t w = __builtin_bswap32(words[c * kFrameSamples + i]);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
  };
  constexpr float k3 = 0.70710678f, k6 = 0.5f;
  for (uint32_t i = 0; i < kFrameSamples; i++) {
    const float fl = ch(0, i), fr = ch(1, i), fc = ch(2, i), lfe = ch(3, i), bl = ch(4, i), br = ch(5, i);
    out[i * 2 + 0] = std::clamp((fl + k3 * fc + k6 * lfe + k3 * bl) * k3, -1.0f, 1.0f);
    out[i * 2 + 1] = std::clamp((fr + k3 * fc + k6 * lfe + k3 * br) * k3, -1.0f, 1.0f);
  }
}

void RenderDriver::TakeSinkLevel(float* out_peak, uint32_t* out_silent_frames) {
  const uint32_t bits = sink_peak_bits_.exchange(0, std::memory_order_relaxed);
  float f;
  std::memcpy(&f, &bits, 4);
  *out_peak = bits ? f : 0.0f;
  *out_silent_frames = sink_silent_.exchange(0, std::memory_order_relaxed);
}

void RenderDriver::SubmitFrame(uint32_t handle, uint32_t samples_guest) {
  if ((handle & 0xFFFF0000u) != kRenderDriverHandleBase) return;
  const uint8_t* frame = host_.translate ? host_.translate(samples_guest) : nullptr;
  if (!frame) return;
  float stereo[kFrameSamples * kOutputChannels];
  Fold(frame, stereo);
  if (host_.mix_output) host_.mix_output(stereo, kFrameSamples);
  // Level tracking, so a log can say whether what left here was silence.
  float peak = 0.0f;
  uint32_t silent = 0;
  for (uint32_t i = 0; i < kFrameSamples; i++) {
    const float l = stereo[i * 2], r = stereo[i * 2 + 1];
    peak = std::max({peak, std::abs(l), std::abs(r)});
    if (l == 0.0f && r == 0.0f) silent++;
  }
  uint32_t bits;
  std::memcpy(&bits, &peak, 4);
  uint32_t prev = sink_peak_bits_.load(std::memory_order_relaxed);
  float prev_f;
  std::memcpy(&prev_f, &prev, 4);
  while (peak > (prev ? prev_f : 0.0f) &&
         !sink_peak_bits_.compare_exchange_weak(prev, bits, std::memory_order_relaxed)) {
    std::memcpy(&prev_f, &prev, 4);
  }
  sink_silent_.fetch_add(silent, std::memory_order_relaxed);
  if (host_.on_output) host_.on_output(stereo, kFrameSamples);
  output_.Submit(stereo, kFrameSamples);
}

void RenderDriver::PumpMain() {
  using clock = std::chrono::steady_clock;
  auto slot = clock::now();
  while (running_) {
    // Even spacing: one slot per frame period on an absolute schedule. If
    // we fall far behind (debugger, page-in) restart the schedule rather
    // than firing a burst of catch-up callbacks.
    slot += kFramePeriod;
    const auto now = clock::now();
    if (now > slot + kFramePeriod * 4) { slot = now; late_.fetch_add(1, std::memory_order_relaxed); }
    else if (now < slot) std::this_thread::sleep_until(slot);

    // Back-pressure: the device drains at exactly 48 kHz; if it already
    // holds kMaxQueuedFrames we skip this slot (the console driver would
    // likewise not ask for a frame it cannot take).
    if (output_.open() && output_.QueuedFrames() >= kMaxQueuedFrames) {
      skipped_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    Client client;
    {
      std::lock_guard<std::mutex> g(lock_);
      for (auto& c : clients_) {
        if (c.in_use) { client = c; break; }
      }
    }
    if (!client.callback) continue;
    const auto before = clock::now();
    if (host_.before_render) host_.before_render();
    host_.call_guest(client.callback, client.callback_arg);
    dispatched_.fetch_add(1, std::memory_order_relaxed);
    static thread_local clock::time_point last_call{};
    if (last_call.time_since_epoch().count()) {
      const uint32_t gap =
          uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(before - last_call).count());
      uint32_t prev = max_gap_us_.load(std::memory_order_relaxed);
      while (gap > prev && !max_gap_us_.compare_exchange_weak(prev, gap, std::memory_order_relaxed)) {}
    }
    last_call = before;
  }
  pump_exited_ = true;
}

}  // namespace xn::audio
