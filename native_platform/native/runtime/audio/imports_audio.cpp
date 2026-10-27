// xenon-native: the audio kernel imports (xboxkrnl XMA* and XAudio*) on top of
// runtime/audio. This is the only audio file that touches the kernel; see
// docs/AUDIO.md for the semantics and for how it is wired into the build.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/hook.h>

#include "audio/imports_audio.h"

#include "audio/render_driver.h"
#include "audio/xma.h"
#include "kernel/kernel.h"
#include "kernel/xmp.h"
#include "kernel/threads.h"
#include "kernel/xtypes.h"

namespace xn::audio {
namespace {

// Addresses the title hands us (input/output buffers) are stored in the
// context as the hardware wants them: physical addresses. Both shapes reach
// this class, and they resolve through different arena views:
//
//   physical, below the 512 MB mark   - what a title that programs the XMA
//     block through its registers writes (Geometry Wars 2 writes 0x11db4000).
//     Physical memory lives at file offset kGuestSize + p in the arena, so it
//     is only reachable through TranslatePhysical; Translate() would read the
//     virtual window at the same number, which is unallocated (zeros), and the
//     decoder would find an all-zero packet header and swap the input buffer
//     without ever emitting a sample.
//   physical-alias virtual address   - what the XMA* imports receive
//     (Geometry Wars 1 passes 0xfc4c0000). The arena maps the 0xA0000000 /
//     0xC0000000 / 0xE0000000 windows at the physical file offsets, so
//     Translate() already yields the physical bytes.
//   plain virtual                    - any other guest address: Translate().
struct ArenaMemory : XmaMemory {
  uint8_t* Physical(uint32_t addr) override {
    if (!addr) return nullptr;
    if (addr < Arena::kPhysicalSize) return K().arena.TranslatePhysical(addr);
    return K().arena.Translate(addr);
  }
};

struct AudioState {
  std::once_flag once;
  ArenaMemory memory;
  XmaEngine xma;
  RenderDriver render;
  uint32_t context_table = 0;  // guest address of kXmaContextCount contexts
  // XN_AUDIO_DUMP=<path>: tee the folded stereo sink to a 48 kHz WAV.
  std::FILE* dump = nullptr;
  std::mutex dump_lock;
  uint32_t dump_bytes = 0;
  std::atomic<bool> stats_running{false};
};

void WavHeader(std::FILE* f, uint32_t data_bytes) {
  auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f); w32(36 + data_bytes); std::fwrite("WAVEfmt ", 1, 8, f);
  w32(16); w16(3); w16(uint16_t(kOutputChannels)); w32(kOutputSampleRate);
  w32(kOutputSampleRate * kOutputChannels * 4); w16(uint16_t(kOutputChannels * 4)); w16(32);
  std::fwrite("data", 1, 4, f); w32(data_bytes);
}

AudioState& S() {
  static AudioState s;
  std::call_once(s.once, [] {
    // The context table lives in a physical-alias window like the console's.
    const uint32_t bytes = kXmaContextCount * kXmaContextBytes;
    s.context_table = K().arena.AllocPhysical(bytes, 0x1000, 256, 0, 0xFFFFFFFFu);
    if (!s.context_table) s.context_table = K().arena.SystemAlloc(bytes, 256);
    s.xma.Setup(&s.memory, G(s.context_table), s.context_table);

    RenderDriverHost host;
    host.headless = K().config.headless;
    host.spawn_guest_thread = [](const char* name, std::function<void()> body) {
      CreateHostWorker(name, [body = std::move(body)](GuestThread&) { body(); });
    };
    host.call_guest = [](uint32_t fn, uint32_t arg) { CallGuest(fn, {arg}); };
    host.before_render = [] {
      const auto capture = XmpPrepareFrame();
      if (capture.callback) CallGuest(capture.callback, {capture.buffer, capture.context, 1});
    };
    host.mix_output = XmpMixFrame;
    host.translate = [](uint32_t guest) -> const uint8_t* { return guest ? G(guest) : nullptr; };
    if (const char* path = std::getenv("XN_AUDIO_DUMP")) {
      s.dump = std::fopen(path, "wb");
      if (s.dump) {
        WavHeader(s.dump, 0);
        REXKRNL_INFO("audio: dumping the sink to {}", path);
        host.on_output = [ps = &s](const float* pcm, uint32_t frames) {
          AudioState& s = *ps;
          std::lock_guard<std::mutex> g(s.dump_lock);
          std::fwrite(pcm, 4, frames * kOutputChannels, s.dump);
          s.dump_bytes += frames * kOutputChannels * 4;
        };
      }
    }
    s.render.Setup(std::move(host));
    // XN_AUDIO_STATS=<seconds>: one line per interval with the pump cadence,
    // the sink level and every allocated XMA context's registers.
    if (const char* iv = std::getenv("XN_AUDIO_STATS")) {
      const double period = std::max(0.25, std::atof(iv) ? std::atof(iv) : 1.0);
      s.stats_running = true;
      std::thread([ps = &s, period] {
        AudioState& s = *ps;
        uint64_t last_cb = 0;
        auto t0 = std::chrono::steady_clock::now();
        while (s.stats_running.load()) {
          std::this_thread::sleep_for(std::chrono::duration<double>(period));
          const auto now = std::chrono::steady_clock::now();
          const double dt = std::chrono::duration<double>(now - t0).count();
          t0 = now;
          const uint64_t cb = s.render.callbacks_dispatched();
          float peak = 0;
          uint32_t silent = 0;
          s.render.TakeSinkLevel(&peak, &silent);
          REXKRNL_INFO("audio: {:.1f} Hz callbacks ({} total, {} skipped, {} late, max gap {:.1f} ms), "
                       "sink peak {:.3f} silent {} samples, device {} ({} frames queued, {} drained), "
                       "sweeps {}",
                       (cb - last_cb) / dt, cb, s.render.slots_skipped(), s.render.slots_late(),
                       s.render.TakeMaxGapUs() / 1000.0, peak, silent,
                       s.render.device_open() ? "open" : "none", s.render.queued_frames(),
                       s.render.frames_drained(), s.xma.worker_sweeps());
          last_cb = cb;
          for (XmaContext* c : s.xma.AllocatedContexts()) REXKRNL_INFO("audio: {}", c->Describe());
        }
      }).detach();
    }
    REXKRNL_INFO("audio: XMA context table at {:#010x} ({} contexts), render driver {}",
                 s.context_table, kXmaContextCount, host.headless ? "headless" : "SDL");
  });
  return s;
}

bool TraceEnabled() {
  static const bool on = std::getenv("XN_AUDIO_TRACE") != nullptr;
  return on;
}
// Hex of the first n bytes a guest pointer points at, for layout questions.
std::string HexDump(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  std::string out;
  char buf[8];
  for (size_t i = 0; i < n; i++) {
    std::snprintf(buf, sizeof buf, "%02x", b[i]);
    out += buf;
    if ((i & 3) == 3) out += ' ';
  }
  return out;
}

XmaContextData LoadCtx(mapped_void p) { return XmaContextData::Load(p.host_address()); }
void StoreCtx(mapped_void p, const XmaContextData& d) { d.Store(p.host_address()); }

// ---- XAudio ----------------------------------------------------------------

u32 XAudioRegisterRenderDriverClient_entry(mapped_u32 callback_ptr, mapped_u32 driver_ptr) {
  if (!callback_ptr) return X_E_INVALIDARG;
  const uint32_t callback = Load32(callback_ptr.guest_address());
  const uint32_t arg = Load32(callback_ptr.guest_address() + 4);
  if (!callback) return X_E_INVALIDARG;
  // The render-driver ABI calls fn(&context), not fn(context). Forza's
  // callback immediately dereferences r3 before accessing the mixer object.
  const auto wrapped_arg = K().arena.SystemAlloc(4, 4);
  if (!wrapped_arg) return X_STATUS_NO_MEMORY;
  Store32(wrapped_arg, arg);
  auto storage = std::shared_ptr<void>(G(wrapped_arg), [wrapped_arg](void*) {
    K().arena.SystemFree(wrapped_arg);
  });
  const uint32_t handle = S().render.RegisterClient(callback, wrapped_arg, std::move(storage));
  if (!handle) return X_STATUS_UNSUCCESSFUL;
  if (driver_ptr) Store32(driver_ptr.guest_address(), handle);
  // Info, not debug: "did a client register" is the first question asked of
  // every silent-audio report, and a session log at info level must answer it.
  REXKRNL_INFO("XAudioRegisterRenderDriverClient callback={:#010x} arg={:#010x} -> driver {:#010x}"
               " (pump started, 187.5 Hz)", callback, arg, handle);
  return X_ERROR_SUCCESS;
}

u32 XAudioUnregisterRenderDriverClient_entry(mapped_void driver) {
  REXKRNL_INFO("XAudioUnregisterRenderDriverClient driver={:#010x}", driver.guest_address());
  S().render.UnregisterClient(driver.guest_address());
  return X_ERROR_SUCCESS;
}

u32 XAudioSubmitRenderDriverFrame_entry(mapped_void driver, mapped_void samples) {
  S().render.SubmitFrame(driver.guest_address(), samples.guest_address());
  return X_ERROR_SUCCESS;
}

u32 XAudioGetVoiceCategoryVolumeChangeMask_entry(mapped_void driver, mapped_u32 out_mask) {
  if (out_mask) Store32(out_mask.guest_address(), 0);  // no category changed
  return X_ERROR_SUCCESS;
}

u32 XAudioGetVoiceCategoryVolume_entry(u32 category, mapped_u32 out_volume) {
  if (out_volume) Store32(out_volume.guest_address(), 0x3F800000u);  // 1.0f
  return X_ERROR_SUCCESS;
}

u32 XAudioGetSpeakerConfig_entry(mapped_u32 out_config) {
  if (out_config) Store32(out_config.guest_address(), 0);  // stereo
  return X_ERROR_SUCCESS;
}

u32 XAudioEnableDucker_entry(u32 enabled) {
  // Dashboard/voice ducking has no source in this offline native runtime.
  // Enabling its policy therefore leaves the title's audio gain unchanged.
  return X_ERROR_SUCCESS;
}

// ---- XMA -------------------------------------------------------------------

u32 XMACreateContext_entry(mapped_u32 out_context) {
  const uint32_t ctx = S().xma.AllocateContext();
  if (out_context) Store32(out_context.guest_address(), ctx);
  return ctx ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY;
}

u32 XMAReleaseContext_entry(mapped_void context) {
  S().xma.ReleaseContext(context.guest_address());
  return 0;
}

u32 XMAInitializeContext_entry(mapped_void context, mapped_void init_ptr) {
  if (TraceEnabled())
    REXKRNL_INFO("XMAInitializeContext ctx={:#010x} init={:#010x}: {}", context.guest_address(),
                 init_ptr.guest_address(), HexDump(init_ptr.host_address(), 56));
  const XmaContextInit init = XmaContextInit::Load(init_ptr.host_address());
  XmaContextData d{};
  d.input_buffer_0_ptr = init.input_buffer_0_ptr;
  d.input_buffer_0_packet_count = init.input_buffer_0_packet_count;
  d.input_buffer_1_ptr = init.input_buffer_1_ptr;
  d.input_buffer_1_packet_count = init.input_buffer_1_packet_count;
  d.input_buffer_read_offset = init.input_buffer_read_offset;
  d.output_buffer_ptr = init.output_buffer_ptr;
  d.output_buffer_block_count = init.output_buffer_block_count;
  d.subframe_decode_count = init.subframe_decode_count;
  d.is_stereo = init.channel_count >= 1;
  d.sample_rate = init.sample_rate;
  d.loop_start = init.loop_start;
  d.loop_end = init.loop_end;
  d.loop_count = init.loop_count;
  d.loop_subframe_end = init.loop_subframe_end;
  d.loop_subframe_skip = init.loop_subframe_skip;
  if (XmaContext* c = S().xma.Lookup(context.guest_address())) {
    c->Disable();
    c->Clear();  // drops decoder state from a previous stream
  }
  StoreCtx(context, d);
  REXKRNL_DEBUG("XMAInitializeContext {:#010x}: in0={:#010x}x{} in1={:#010x}x{} out={:#010x}x{} blocks sdc={} {}ch {} Hz loop {}..{} x{}",
                context.guest_address(), init.input_buffer_0_ptr, init.input_buffer_0_packet_count,
                init.input_buffer_1_ptr, init.input_buffer_1_packet_count, init.output_buffer_ptr,
                init.output_buffer_block_count, init.subframe_decode_count, d.is_stereo ? 2 : 1,
                XmaSampleRate(init.sample_rate), init.loop_start, init.loop_end, init.loop_count);
  return 0;
}

// loop_data points at an XMA_CONTEXT_DATA-shaped block the title filled in.
u32 XMASetLoopData_entry(mapped_void context, mapped_void loop_data) {
  if (TraceEnabled())
    REXKRNL_INFO("XMASetLoopData ctx={:#010x} data={:#010x}: {}", context.guest_address(),
                 loop_data.guest_address(), HexDump(loop_data.host_address(), 32));
  const XmaLoopData l = XmaLoopData::Load(loop_data.host_address());
  XmaContextData d = LoadCtx(context);
  d.loop_start = l.loop_start;
  d.loop_end = l.loop_end;
  d.loop_count = l.loop_count;
  d.loop_subframe_end = l.loop_subframe_end;
  d.loop_subframe_skip = l.loop_subframe_skip;
  StoreCtx(context, d);
  return 0;
}

u32 XMAGetInputBufferReadOffset_entry(mapped_void context) { return LoadCtx(context).input_buffer_read_offset; }
u32 XMASetInputBufferReadOffset_entry(mapped_void context, u32 value) {
  XmaContextData d = LoadCtx(context);
  d.input_buffer_read_offset = value;
  StoreCtx(context, d);
  return 0;
}

u32 XMASetInputBuffer0_entry(mapped_void context, mapped_void buffer, u32 packet_count) {
  XmaContextData d = LoadCtx(context);
  d.input_buffer_0_ptr = buffer.guest_address();
  d.input_buffer_0_packet_count = packet_count;
  StoreCtx(context, d);
  return 0;
}
u32 XMASetInputBuffer1_entry(mapped_void context, mapped_void buffer, u32 packet_count) {
  XmaContextData d = LoadCtx(context);
  d.input_buffer_1_ptr = buffer.guest_address();
  d.input_buffer_1_packet_count = packet_count;
  StoreCtx(context, d);
  return 0;
}
u32 XMAIsInputBuffer0Valid_entry(mapped_void context) { return LoadCtx(context).input_buffer_0_valid; }
u32 XMAIsInputBuffer1Valid_entry(mapped_void context) { return LoadCtx(context).input_buffer_1_valid; }
u32 XMASetInputBuffer0Valid_entry(mapped_void context) {
  if (TraceEnabled()) {
    if (XmaContext* c = S().xma.Lookup(context.guest_address()))
      REXKRNL_INFO("XMASetInputBuffer0Valid: {}", c->Describe());
  }
  XmaContextData d = LoadCtx(context);
  d.input_buffer_0_valid = 1;
  StoreCtx(context, d);
  return 0;
}
u32 XMASetInputBuffer1Valid_entry(mapped_void context) {
  if (TraceEnabled()) {
    if (XmaContext* c = S().xma.Lookup(context.guest_address()))
      REXKRNL_INFO("XMASetInputBuffer1Valid: {}", c->Describe());
  }
  XmaContextData d = LoadCtx(context);
  d.input_buffer_1_valid = 1;
  StoreCtx(context, d);
  return 0;
}

u32 XMAIsOutputBufferValid_entry(mapped_void context) { return LoadCtx(context).output_buffer_valid; }
u32 XMASetOutputBufferValid_entry(mapped_void context) {
  if (TraceEnabled()) {
    if (XmaContext* c = S().xma.Lookup(context.guest_address()))
      REXKRNL_INFO("XMASetOutputBufferValid: {}", c->Describe());
  }
  XmaContextData d = LoadCtx(context);
  d.output_buffer_valid = 1;
  StoreCtx(context, d);
  return 0;
}
u32 XMAGetOutputBufferReadOffset_entry(mapped_void context) { return LoadCtx(context).output_buffer_read_offset; }
u32 XMASetOutputBufferReadOffset_entry(mapped_void context, u32 value) {
  XmaContextData d = LoadCtx(context);
  d.output_buffer_read_offset = value;
  StoreCtx(context, d);
  return 0;
}
u32 XMAGetOutputBufferWriteOffset_entry(mapped_void context) { return LoadCtx(context).output_buffer_write_offset; }
u32 XMAGetPacketMetadata_entry(mapped_void context) { return LoadCtx(context).packet_metadata; }

u32 XMAEnableContext_entry(mapped_void context) {
  if (TraceEnabled()) {
    if (XmaContext* c = S().xma.Lookup(context.guest_address()))
      REXKRNL_INFO("XMAEnableContext: {}", c->Describe());
  }
  S().xma.Enable(context.guest_address());
  return 0;
}
u32 XMADisableContext_entry(mapped_void context, u32 wait) {
  if (TraceEnabled()) {
    if (XmaContext* c = S().xma.Lookup(context.guest_address()))
      REXKRNL_INFO("XMADisableContext(wait={}): {}", wait, c->Describe());
  }
  S().xma.Disable(context.guest_address());  // always waits for the pass in flight
  return 0;
}
u32 XMABlockWhileInUse_entry(mapped_void context) {
  if (XmaContext* c = S().xma.Lookup(context.guest_address())) c->Block();
  return 0;
}

}  // namespace
}  // namespace xn::audio

// The __imp__ symbols. XN_AUDIO_IMPORTS lists every import this file
// answers; imports_audio.h exposes the same list for a registration table.
#define XN_AUDIO_IMPORTS(X)                    \
  X(XAudioRegisterRenderDriverClient)         \
  X(XAudioUnregisterRenderDriverClient)       \
  X(XAudioSubmitRenderDriverFrame)            \
  X(XAudioGetVoiceCategoryVolumeChangeMask)   \
  X(XAudioGetVoiceCategoryVolume)             \
  X(XAudioGetSpeakerConfig)                   \
  X(XAudioEnableDucker)                       \
  X(XMACreateContext)                         \
  X(XMAReleaseContext)                        \
  X(XMAInitializeContext)                     \
  X(XMASetLoopData)                           \
  X(XMAGetInputBufferReadOffset)              \
  X(XMASetInputBufferReadOffset)              \
  X(XMASetInputBuffer0)                       \
  X(XMASetInputBuffer1)                       \
  X(XMAIsInputBuffer0Valid)                   \
  X(XMAIsInputBuffer1Valid)                   \
  X(XMASetInputBuffer0Valid)                  \
  X(XMASetInputBuffer1Valid)                  \
  X(XMAIsOutputBufferValid)                   \
  X(XMASetOutputBufferValid)                  \
  X(XMAGetOutputBufferReadOffset)             \
  X(XMASetOutputBufferReadOffset)             \
  X(XMAGetOutputBufferWriteOffset)            \
  X(XMAGetPacketMetadata)                     \
  X(XMAEnableContext)                         \
  X(XMADisableContext)                        \
  X(XMABlockWhileInUse)

#define XN_AUDIO_DEFINE(name) XN_IMPORT(__imp__##name, xn::audio::name##_entry)
XN_AUDIO_IMPORTS(XN_AUDIO_DEFINE)
#undef XN_AUDIO_DEFINE

namespace xn::audio {

XmaEngine& Engine() { return S().xma; }
uint32_t ContextTableAddress() { return S().context_table; }

#define XN_AUDIO_ENTRY(name) {#name, &__imp__##name},
const AudioImport kAudioImports[] = {XN_AUDIO_IMPORTS(XN_AUDIO_ENTRY)};
#undef XN_AUDIO_ENTRY
const size_t kAudioImportCount = sizeof(kAudioImports) / sizeof(kAudioImports[0]);
void AudioShutdown() {
  AudioState& s = S();
  s.stats_running = false;
  s.render.Shutdown();
  XmpShutdown();
  s.xma.Shutdown();
  std::lock_guard<std::mutex> g(s.dump_lock);
  if (s.dump) {
    std::fseek(s.dump, 0, SEEK_SET);
    WavHeader(s.dump, s.dump_bytes);
    std::fclose(s.dump);
    s.dump = nullptr;
    REXKRNL_INFO("audio: sink dump closed ({} bytes)", s.dump_bytes);
  }
}
}  // namespace xn::audio
