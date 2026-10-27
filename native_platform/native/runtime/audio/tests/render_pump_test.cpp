// xenon-native: end-to-end audio check without the guest. A stand-in mixer
// plays one XMA wave-bank entry through the whole chain the title uses -
// XmaEngine context (valid bits, ring, read offsets) -> a "guest" callback
// that drains the ring into a 5.1 big-endian render frame -> RenderDriver
// (187.5 Hz pump, 5.1 -> stereo fold) -> SDL3 - and checks the cadence, the
// device drain and the samples that reached the sink.
//
//   render_pump_test <assets dir> <out dir> [seconds] [--silent]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/render_driver.h"
#include "audio/xma.h"
#include "audio/xwb.h"

using namespace xn::audio;

namespace {

// A flat host address space standing in for guest memory.
struct HostMemory : XmaMemory {
  std::vector<uint8_t> mem;
  uint8_t* Physical(uint32_t a) override { return a && a < mem.size() ? mem.data() + a : nullptr; }
};

constexpr uint32_t kTable = 0x1000, kRing = 0x2000, kRingBlocks = 10, kInput = 0x10000;
constexpr uint32_t kRenderFrame = 0x8000;  // where the "guest" builds its 5.1 frame

struct Mixer {
  HostMemory mem;
  XmaEngine xma;
  RenderDriver* driver = nullptr;
  uint32_t ctx = 0, handle = 0;
  uint32_t packets = 0, next_packet = 0;
  bool stereo = true;
  // Leftover decoded samples between callbacks (the ring hands out 128-sample
  // blocks; a render frame needs 256).
  std::vector<int16_t> pending;
  std::atomic<uint64_t> callbacks{0}, starved{0}, samples_out{0};

  // Fills one input buffer in the caller's context copy (the caller stores it).
  bool FillInput(XmaContextData& d, uint32_t which) {
    if (next_packet >= packets) return false;
    const uint32_t n = std::min(8u, packets - next_packet);
    const uint32_t addr = kInput + next_packet * kXmaPacketBytes;
    if (which == 0) { d.input_buffer_0_ptr = addr; d.input_buffer_0_packet_count = n; d.input_buffer_0_valid = 1; }
    else { d.input_buffer_1_ptr = addr; d.input_buffer_1_packet_count = n; d.input_buffer_1_valid = 1; }
    next_packet += n;
    return true;
  }

  // What the title's XACT voice does every callback: stop the decoder, read
  // the offsets, drain what is there, hand the blocks back, restart it.
  void DrainRing(uint32_t want_samples) {
    const uint32_t channels = stereo ? 2 : 1;
    xma.Disable(ctx);
    XmaContextData d = XmaContextData::Load(mem.mem.data() + ctx);
    const uint32_t r = d.output_buffer_read_offset, w = d.output_buffer_write_offset;
    uint32_t avail = (!d.output_buffer_valid && r == w) ? kRingBlocks : (w + kRingBlocks - r) % kRingBlocks;
    while (avail && pending.size() < want_samples * channels) {
      const uint16_t* be = reinterpret_cast<const uint16_t*>(mem.mem.data() + kRing + (d.output_buffer_read_offset % kRingBlocks) * kXmaOutputBlockBytes);
      for (uint32_t i = 0; i < kXmaOutputBlockBytes / 2; i++) pending.push_back(int16_t(__builtin_bswap16(be[i])));
      d.output_buffer_read_offset = (d.output_buffer_read_offset + 1) % kRingBlocks;
      avail--;
    }
    d.output_buffer_valid = 1;
    if (!d.input_buffer_0_valid) FillInput(d, 0);
    if (!d.input_buffer_1_valid) FillInput(d, 1);
    d.Store(mem.mem.data() + ctx);
    xma.Enable(ctx);
  }

  // Stands in for the guest mixer callback 0x82092510: builds one 256-sample
  // 5.1 frame (channel-sequential big-endian floats) and submits it.
  void Callback() {
    callbacks++;
    DrainRing(kFrameSamples);
    const uint32_t channels = stereo ? 2 : 1;
    float* out = reinterpret_cast<float*>(mem.mem.data() + kRenderFrame);
    std::memset(out, 0, kRenderFrameBytes);
    const size_t have = pending.size() / channels;
    if (have < kFrameSamples) starved++;
    for (uint32_t i = 0; i < kFrameSamples && i < have; i++) {
      for (uint32_t c = 0; c < 2; c++) {
        const float v = pending[i * channels + (channels == 2 ? c : 0)] / 32768.0f;
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        reinterpret_cast<uint32_t*>(out)[c * kFrameSamples + i] = __builtin_bswap32(bits);
      }
    }
    const size_t used = std::min<size_t>(have, kFrameSamples) * channels;
    pending.erase(pending.begin(), pending.begin() + ptrdiff_t(used));
    samples_out += used / channels;
    driver->SubmitFrame(handle, kRenderFrame);
  }
};

bool WriteWav(const std::string& path, const std::vector<float>& pcm, int channels, int rate) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::vector<int16_t> s(pcm.size());
  for (size_t i = 0; i < pcm.size(); i++) s[i] = int16_t(std::clamp(pcm[i], -1.0f, 1.0f) * 32767.0f);
  const uint32_t bytes = uint32_t(s.size() * 2);
  auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
  w32(16); w16(1); w16(uint16_t(channels)); w32(uint32_t(rate)); w32(uint32_t(rate * channels * 2));
  w16(uint16_t(channels * 2)); w16(16); std::fwrite("data", 1, 4, f); w32(bytes);
  std::fwrite(s.data(), 2, s.size(), f);
  std::fclose(f);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s <assets dir> <out dir> [seconds] [--silent]\n", argv[0]); return 2; }
  const std::string assets = argv[1], outdir = argv[2];
  const double seconds = argc > 3 ? std::atof(argv[3]) : 3.0;
  bool silent = getenv("XN_AUDIO_SILENT") != nullptr;
  for (int i = 4; i < argc; i++) silent |= std::string(argv[i]) == "--silent";

  XwbBank bank;
  std::string err;
  if (!XwbLoad(assets + "/GW2_Music.xwb", bank, err)) { std::printf("xwb: %s\n", err.c_str()); return 1; }
  const XwbEntry& e = bank.entries[1];
  std::printf("source: GW2_Music.xwb entry 1, %s %uch %u Hz, %u bytes\n", e.tag ? "XMA" : "PCM", e.channels, e.sample_rate, e.play_length);

  Mixer m;
  m.stereo = e.channels == 2;
  m.packets = e.play_length / kXmaPacketBytes;
  m.mem.mem.resize(kInput + m.packets * kXmaPacketBytes + 0x1000);
  std::memcpy(m.mem.mem.data() + kInput, bank.wave_data(e), m.packets * kXmaPacketBytes);
  m.xma.Setup(&m.mem, m.mem.mem.data() + kTable, kTable);
  m.ctx = m.xma.AllocateContext();

  XmaContextData d{};
  d.output_buffer_ptr = kRing;
  d.output_buffer_block_count = kRingBlocks;
  d.subframe_decode_count = 4;
  d.is_stereo = m.stereo;
  d.sample_rate = 3;  // 48000
  d.input_buffer_read_offset = kXmaPacketHeaderBits;
  d.output_buffer_valid = 1;
  m.FillInput(d, 0);
  m.FillInput(d, 1);
  d.Store(m.mem.mem.data() + m.ctx);
  m.xma.Enable(m.ctx);

  std::vector<float> captured;
  std::mutex capture_lock;
  RenderDriver driver;
  m.driver = &driver;
  RenderDriverHost host;
  host.headless = silent;
  host.spawn_guest_thread = [](const char* name, std::function<void()> body) {
    std::thread(std::move(body)).detach();
  };
  host.call_guest = [&m](uint32_t, uint32_t) { m.Callback(); };
  host.translate = [&m](uint32_t a) -> const uint8_t* { return m.mem.mem.data() + a; };
  host.on_output = [&](const float* s, uint32_t frames) {
    std::lock_guard<std::mutex> g(capture_lock);
    captured.insert(captured.end(), s, s + frames * kOutputChannels);
  };
  driver.Setup(std::move(host));
  m.handle = driver.RegisterClient(0x82092510, 0x4005B2DC);
  std::printf("driver handle %#010x, device %s\n", m.handle, driver.device_open() ? "open" : (silent ? "silent (forced)" : "unavailable"));

  // Watch the cadence and the device backlog while it plays.
  const auto start = std::chrono::steady_clock::now();
  uint32_t queued_max = 0, samples_at_1s = 0;
  while (std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    queued_max = std::max(queued_max, driver.queued_frames());
    if (!samples_at_1s && std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
      samples_at_1s = uint32_t(driver.callbacks_dispatched());
  }
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  driver.Shutdown();
  m.xma.Shutdown();

  const uint64_t cb = driver.callbacks_dispatched();
  const double rate = cb / elapsed;
  std::printf("  %llu callbacks in %.2f s = %.1f Hz (target 187.5), %llu frames to the sink, max device backlog %u frames\n",
              (unsigned long long)cb, elapsed, rate, (unsigned long long)driver.frames_submitted(), queued_max);
  std::printf("  mixer: %llu samples drained from the XMA ring, %llu starved callbacks\n",
              (unsigned long long)m.samples_out.load(), (unsigned long long)m.starved.load());

  double peak = 0, sum2 = 0;
  size_t silent_frames = 0;
  {
    std::lock_guard<std::mutex> g(capture_lock);
    for (float v : captured) { peak = std::max(peak, std::abs(double(v))); sum2 += double(v) * v; }
    for (size_t i = 0; i + 1 < captured.size(); i += 2) if (captured[i] == 0.0f && captured[i + 1] == 0.0f) silent_frames++;
    WriteWav(outdir + "/render_pump.wav", captured, int(kOutputChannels), int(kOutputSampleRate));
  }
  const size_t frames = captured.size() / kOutputChannels;
  std::printf("  sink: %zu stereo frames (%.2f s), peak %.3f (%.1f dBFS), rms %.1f dBFS, %zu silent frames\n",
              frames, double(frames) / kOutputSampleRate, peak, 20 * std::log10(std::max(peak, 1e-9)),
              10 * std::log10(sum2 / std::max<size_t>(1, captured.size())), silent_frames);
  std::printf("  wrote %s/render_pump.wav\n", outdir.c_str());

  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-34s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
  };
  check("callback rate within 2% of 187.5", rate > 183.0 && rate < 191.5);
  check("no starved callbacks", m.starved.load() == 0);
  check("every callback submitted a frame", driver.frames_submitted() == cb);
  check("audio reached the sink", frames > size_t(seconds * kOutputSampleRate * 0.9));
  check("sink audio is not silence", peak > 0.01 && silent_frames * 20 < frames);
  if (driver.device_open()) check("device drains (backlog bounded)", queued_max <= kMaxQueuedFrames + 1);
  std::printf("%s\n", failures ? "FAILED" : "ALL PASS");
  return failures ? 1 : 0;
}
