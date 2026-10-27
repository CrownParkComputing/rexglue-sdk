#include "render_capture.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <sys/mman.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "gfx/render_trace.h"
#include "memory/physical_write_watch.h"
#include "vulkan_renderer.h"

namespace xn {
namespace {

namespace trace = gfx::trace;
constexpr size_t kPhysicalBytes = size_t(512) << 20;
constexpr size_t kPageBytes = PhysicalWriteWatch::kPageSize;
constexpr size_t kMaxGammaHistory = 4'000'000;
constexpr uint32_t kMaxShaderWords = (4u << 20) / sizeof(uint32_t);

bool ReadNumber(const char* name, uint64_t fallback, uint64_t maximum,
                uint64_t& value) {
  const char* input = std::getenv(name);
  if (!input || !*input) { value = fallback; return true; }
  char* end = nullptr;
  errno = 0;
  const auto number = std::strtoull(input, &end, 10);
  if (*input == '-' || errno || !end || *end || number > maximum) {
    std::fprintf(stderr, "xn-render-trace: invalid %s=%s\n", name, input);
    return false;
  }
  value = number;
  return true;
}

// Freeze host-decoded texture responses for one renderer callback. Querying
// every nonzero revision also snapshots its bytes even when the live upload
// cache does not need another Read, so a fresh replay can populate its cache.
class CaptureTextureSource final : public gfx::TextureUploadSource {
 public:
  using Key = std::array<uint32_t, 4>;
  struct Sample {
    uint64_t revision = 0;
    bool read_ok = false;
    std::vector<uint8_t> bytes;
  };
  explicit CaptureTextureSource(std::shared_ptr<gfx::TextureUploadSource> source)
      : source_(std::move(source)) {}
  void BeginCall(bool recording) {
    recording_ = recording;
    samples_.clear(); order_.clear(); error_.clear();
  }
  void EndCall() { recording_ = false; }
  const std::vector<Key>& Order() const { return order_; }
  const Sample& Get(const Key& key) const { return samples_.at(key); }
  const std::string& Error() const { return error_; }
  void OnDraw(uint64_t shader, const GpuRegisterFile& registers) override {
    source_->OnDraw(shader, registers);
  }
  uint64_t Revision(uint32_t address, uint32_t width, uint32_t height,
                    uint32_t format) const override {
    if (!recording_) return source_->Revision(address, width, height, format);
    const Key key{address, width, height, format};
    auto [sample, inserted] = samples_.try_emplace(key);
    if (inserted) {
      sample->second.revision = source_->Revision(address, width, height, format);
      if (sample->second.revision)
        sample->second.read_ok = source_->Read(address, width, height, format,
                                               sample->second.bytes);
      order_.push_back(key);
    }
    return sample->second.revision;
  }
  bool Read(uint32_t address, uint32_t width, uint32_t height, uint32_t format,
            std::vector<uint8_t>& bytes) const override {
    if (!recording_) return source_->Read(address, width, height, format, bytes);
    const auto sample = samples_.find(Key{address, width, height, format});
    if (sample == samples_.end() || !sample->second.revision) {
      error_ = "host texture Read occurred without a preceding nonzero Revision";
      bytes.clear(); return false;
    }
    bytes = sample->second.bytes;
    return sample->second.read_ok;
  }
 private:
  std::shared_ptr<gfx::TextureUploadSource> source_;
  mutable bool recording_ = false;
  mutable std::map<Key, Sample> samples_;
  mutable std::vector<Key> order_;
  mutable std::string error_;
};

gfx::VulkanRendererConfig WithCaptureHooks(
    gfx::VulkanRendererConfig config, const std::shared_ptr<CaptureTextureSource>& proxy,
    std::function<void(uint32_t, uint32_t)> prepare_guest_read) {
  config.texture_source = proxy;
  config.prepare_guest_read = std::move(prepare_guest_read);
  return config;
}

class RenderCapture final : public IRenderer {
 public:
  explicit RenderCapture(gfx::VulkanRendererConfig config)
      : texture_proxy_(config.texture_source
            ? std::make_shared<CaptureTextureSource>(config.texture_source) : nullptr),
        upstream_prepare_(config.prepare_guest_read),
        config_(WithCaptureHooks(std::move(config), texture_proxy_,
            [this](uint32_t address, uint32_t size) { ObserveGuestRead(address, size); })),
        renderer_(std::make_unique<gfx::VulkanRenderer>(config_)) {}
  ~RenderCapture() override { Shutdown(); }

  bool Initialize(SDL_Window* window, uint8_t* guest_base,
                  uint8_t* physical_base) override {
    std::lock_guard lock(mutex_);
    if (window) {
      std::fprintf(stderr, "xn-render-trace: capture requires headless rendering\n");
      return false;
    }
    const char* path = std::getenv("XN_RENDER_TRACE");
    if (!path || !*path || !physical_base ||
        !ReadNumber("XN_RENDER_TRACE_START_SWAP", 1500, UINT64_MAX, start_swap_) ||
        !ReadNumber("XN_RENDER_TRACE_FRAMES", 9, 600, frame_count_) || !frame_count_) {
      std::fprintf(stderr, "xn-render-trace: destination, physical memory and "
                           "a frame count from 1 to 600 are required\n");
      return false;
    }
    path_ = path;
    live_base_ = physical_base;
    initialized_ = renderer_->Initialize(nullptr, guest_base, physical_base);
    if (initialized_)
      std::fprintf(stderr, "xn-render-trace: armed %s after swap %llu for %llu swaps\n",
                   path_.string().c_str(), static_cast<unsigned long long>(start_swap_),
                   static_cast<unsigned long long>(frame_count_));
    return initialized_;
  }

  void Shutdown() override {
    std::lock_guard lock(mutex_);
    if (active_) Abort("renderer shut down before the requested window completed");
    RestoreInput();
    if (initialized_) { renderer_->Shutdown(); initialized_ = false; }
  }

  bool RequiresRegistersForShaderLoad() const override { return false; }

  void OnRegisterWrite(uint32_t index, uint32_t value) override {
    std::lock_guard lock(mutex_);
    Prepare();
    if (index < GpuRegisterFile::kCount) registers_.values[index] = value;
    if (index >= 0x1921 && index <= 0x1936 && !done_) {
      if (gamma_history_.size() >= kMaxGammaHistory)
        Abort("display gamma write history exceeded its bounded capacity");
      else gamma_history_.push_back({index, value});
    }
    if (active_) EmitRegisterWrite(index, value);
    renderer_->OnRegisterWrite(index, value);
  }

  void OnConstants(uint32_t index, const uint32_t* dwords, uint32_t count) override {
    std::lock_guard lock(mutex_);
    Prepare();
    std::vector<uint32_t> snapshot;
    const uint32_t valid_count = index < GpuRegisterFile::kCount
        ? std::min(count, GpuRegisterFile::kCount - index) : 0;
    if (dwords && valid_count) {
      if (active_) {
        snapshot.assign(dwords, dwords + valid_count);
        dwords = snapshot.data();
      }
      trace::ByteWriter event;
      if (active_) event.U32(valid_count);
      for (uint32_t i = 0; i < valid_count; ++i) {
        const uint32_t value = __builtin_bswap32(dwords[i]);
        registers_.values[index + i] = value;
        if (active_) { event.U32(index + i); event.U32(value); }
      }
      if (active_) Emit(trace::EventKind::kRegisterBatch, event);
    }
    renderer_->OnConstants(index, dwords, snapshot.empty() ? count : valid_count);
  }

  void OnRegisterBatch(const GpuRegisterFile& registers,
                       std::span<const RegisterRange> ranges) override {
    std::lock_guard lock(mutex_);
    Prepare();
    uint64_t count = 0;
    for (const auto& range : ranges)
      if (range.index < GpuRegisterFile::kCount &&
          range.count <= GpuRegisterFile::kCount - range.index) count += range.count;
    trace::ByteWriter event;
    if (active_) {
      if (count > UINT32_MAX) Abort("register batch is too large");
      else event.U32(uint32_t(count));
    }
    for (const auto& range : ranges) {
      if (range.index >= GpuRegisterFile::kCount ||
          range.count > GpuRegisterFile::kCount - range.index) continue;
      for (uint32_t i = 0; i < range.count; ++i) {
        const auto index = range.index + i;
        registers_.values[index] = registers.values[index];
        if (active_) { event.U32(index); event.U32(registers.values[index]); }
      }
    }
    if (active_) Emit(trace::EventKind::kRegisterBatch, event);
    renderer_->OnRegisterBatch(registers, ranges);
  }

  void OnShaderLoad(uint32_t type, uint32_t address, uint32_t count,
                    const uint32_t* dwords) override {
    std::lock_guard lock(mutex_);
    Prepare();
    if (dwords && count) {
      if (count > kMaxShaderWords || type > 1) {
        Abort("shader input exceeds the supported capture bounds");
      } else {
        auto& shader = shaders_[type];
        shader.address = address;
        shader.words.assign(dwords, dwords + count);
        if (active_) EmitShader(type, shader);
        // Both the recorded blob and the reference consume this owned copy.
        dwords = shader.words.data();
      }
    }
    renderer_->OnShaderLoad(type, address, count, dwords);
  }

  void OnDraw(const DrawPacket& draw, const GpuRegisterFile& registers) override {
    std::lock_guard lock(mutex_);
    Prepare(true);
    if (active_ || !draw.registers_notified) SynchronizeRegisters(registers);
    DrawPacket snapshot = draw;
    if (first_draw_snapshot_) {
      snapshot.registers_notified = false;
      first_draw_snapshot_ = false;
    }
    std::vector<uint32_t> immediate;
    trace::ByteWriter event;
    if (active_) {
      uint64_t blob = 0;
      if (draw.immediate_words) {
        if (!draw.immediate || uint64_t(draw.immediate_words) * 4 > (64u << 20))
          Abort("immediate index data exceeds the supported capture bounds");
        else {
          immediate.assign(draw.immediate, draw.immediate + draw.immediate_words);
          snapshot.immediate = immediate.data();
          blob = PutBlob(std::as_bytes(std::span(immediate)));
        }
      }
      if (active_) {
        for (uint32_t value : {draw.opcode, draw.prim_type, draw.index_count,
             draw.source, draw.index_base, draw.index_size, uint32_t(draw.index_32bit),
             draw.index_endian, draw.immediate_words, uint32_t(snapshot.registers_notified)})
          event.U32(value);
        event.U64(blob);
      }
    }
    ForwardWithTextures([&] { renderer_->OnDraw(snapshot, registers); });
    if (active_) Emit(trace::EventKind::kDraw, event);
  }

  void OnEvent(uint32_t initiator, const GpuRegisterFile& registers) override {
    std::lock_guard lock(mutex_);
    Prepare(true);
    SynchronizeRegisters(registers);
    ForwardWithTextures([&] { renderer_->OnEvent(initiator, registers); });
    if (active_) { trace::ByteWriter event; event.U32(initiator);
      Emit(trace::EventKind::kGpuEvent, event); }
  }

  void OnSwap(uint32_t address, uint32_t width, uint32_t height) override {
    std::lock_guard lock(mutex_);
    Prepare(true);
    ForwardWithTextures([&] { renderer_->OnSwap(address, width, height); });
    if (active_) {
      trace::ByteWriter event;
      event.U32(address); event.U32(width); event.U32(height);
      Emit(trace::EventKind::kSwap, event);
    }
    ++swaps_seen_;
    if (active_ && ++captured_swaps_ == frame_count_) FinishCapture();
  }

  void Present() override {
    // Headless presentation has no input or commands to record.
    std::lock_guard lock(mutex_);
    renderer_->Present();
  }
  void SetFpsOverlay(std::string_view text) override {
    std::lock_guard lock(mutex_); renderer_->SetFpsOverlay(text);
  }
  void SetFrameRateTarget(uint32_t fps) override {
    std::lock_guard lock(mutex_); renderer_->SetFrameRateTarget(fps);
  }
  bool CaptureFrame(std::vector<uint8_t>& pixels, uint32_t& width,
                    uint32_t& height) override {
    std::lock_guard lock(mutex_); return renderer_->CaptureFrame(pixels, width, height);
  }
  bool SynchronizeTextureToGuest(uint32_t address, uint32_t width,
                                 uint32_t height, uint32_t format) override {
    std::lock_guard lock(mutex_);
    if (active_) Abort("CPU texture synchronization occurred during capture; "
                       "live writeback restored before forwarding", true);
    else if (restore_pending_) RestoreInput(true);
    return renderer_->SynchronizeTextureToGuest(address, width, height, format);
  }
  bool OnD3DCall(const D3DCall& call) override {
    std::lock_guard lock(mutex_); return renderer_->OnD3DCall(call);
  }
  void FlushCaches() override {
    std::lock_guard lock(mutex_); renderer_->FlushCaches();
  }

 private:
  struct ShaderSnapshot { uint32_t address = 0; std::vector<uint32_t> words; };
  struct BackendCallLifetime {
    RenderCapture& owner;
    explicit BackendCallLifetime(RenderCapture& capture) : owner(capture) {
      ++owner.backend_depth_;
    }
    ~BackendCallLifetime() {
      if (!--owner.backend_depth_ && owner.restore_pending_) owner.RestoreInput();
    }
  };

  template <typename Call> void ForwardWithTextures(Call&& call) {
    if (texture_proxy_) texture_proxy_->BeginCall(active_);
    try {
      BackendCallLifetime lifetime(*this);
      call();
    } catch (...) {
      Abort("renderer callback exited with an exception");
      throw;
    }
    if (!texture_proxy_) return;
    texture_proxy_->EndCall();
    if (!active_) return;
    if (!texture_proxy_->Error().empty()) { Abort(texture_proxy_->Error()); return; }
    // The matching renderer event is emitted AFTER these buffered responses.
    // Its replay callback can therefore issue a different number of cache
    // queries while consuming exactly the same frozen response for each key.
    for (const auto& key : texture_proxy_->Order()) {
      const auto& sample = texture_proxy_->Get(key);
      trace::ByteWriter revision;
      for (uint32_t component : key) revision.U32(component);
      revision.U64(sample.revision);
      if (!Emit(trace::EventKind::kTextureRevision, revision)) return;
      if (sample.revision) {
        const uint64_t blob = sample.read_ok ? PutBlob(sample.bytes) : 0;
        if (!active_) return;
        trace::ByteWriter read;
        for (uint32_t component : key) read.U32(component);
        read.U64(sample.revision); read.U32(uint32_t(sample.read_ok)); read.U64(blob);
        if (!Emit(trace::EventKind::kTextureRead, read)) return;
      }
    }
  }

  void Prepare(bool reads_physical_memory = false) {
    if (!initialized_ || done_) return;
    if (!active_ && swaps_seen_ >= start_swap_) BeginCapture();
    if (active_ && reads_physical_memory && ++read_epoch_ == 0) {
      std::fill(page_epochs_.begin(), page_epochs_.end(), 0);
      read_epoch_ = 1;
    }
  }

  void BeginCapture() {
    watch_ = PhysicalWriteWatch::Find(live_base_, kPhysicalBytes);
    if (!watch_) { Abort("physical write watch is unavailable"); return; }
    void* allocation = mmap(nullptr, kPhysicalBytes, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (allocation == MAP_FAILED) { Abort("cannot allocate the capture input image"); return; }
    shadow_ = static_cast<uint8_t*>(allocation);
    shadow_tokens_.assign(kPhysicalBytes / kPageBytes, 0);
    page_epochs_.assign(kPhysicalBytes / kPageBytes, 0);
    read_epoch_ = 1;
    std::error_code error_code;
    if (!path_.parent_path().empty())
      std::filesystem::create_directories(path_.parent_path(), error_code);
    if (error_code) { Abort("cannot create trace destination directory"); return; }
    writer_ = std::make_unique<trace::Writer>(path_);
    std::string error;
    if (!writer_->Open(error)) { Abort(error); return; }
    active_ = true;
    trace::ByteWriter info;
    info.U32(2); info.U64(kPhysicalBytes); info.U32(uint32_t(frame_count_));
    // Preserve the original physical-only payload; bit 0 opts into captured
    // host texture inputs when the title supplies a TextureUploadSource.
    if (texture_proxy_) info.U32(1);
    if (!Emit(trace::EventKind::kCaptureInfo, info) || !EmitRegisterFile()) return;
    for (const auto& [index, value] : gamma_history_)
      if (!EmitRegisterWrite(index, value)) return;
    for (uint32_t type = 0; type < shaders_.size(); ++type)
      if (!shaders_[type].words.empty() && !EmitShader(type, shaders_[type])) return;
    // Both reference and replay start from the same empty GPU state. Keeping
    // the live backend's earlier temporal images or upload caches would make
    // its reference depend on work that predates this bounded trace window.
    renderer_->FlushCaches();
    renderer_->Shutdown();
    renderer_ = std::make_unique<gfx::VulkanRenderer>(config_);
    input_switched_ = true;
    if (!renderer_->Initialize(nullptr, nullptr, shadow_)) {
      Abort("could not initialize the fresh capture Vulkan backend");
      renderer_->Shutdown();
      initialized_ = renderer_->Initialize(nullptr, nullptr, live_base_);
      return;
    }
    for (const auto& [index, value] : gamma_history_)
      renderer_->OnRegisterWrite(index, value);
    for (uint32_t type = 0; type < shaders_.size(); ++type) {
      const auto& shader = shaders_[type];
      if (!shader.words.empty())
        renderer_->OnShaderLoad(type, shader.address, uint32_t(shader.words.size()),
                                shader.words.data());
    }
    first_draw_snapshot_ = true;
    std::fprintf(stderr, "xn-render-trace: recording window at swap %llu\n",
                 static_cast<unsigned long long>(swaps_seen_));
  }

  void ObserveGuestRead(uint32_t address, uint32_t size) {
    if (upstream_prepare_) upstream_prepare_(address, size);
    // An I/O failure can abort recording while the renderer still holds a
    // pointer into this image. Keep preparing its remaining reads until that
    // callback returns; only then may the image be released.
    if ((!active_ && !restore_pending_) || !input_switched_ || !size) return;
    const uint64_t end = uint64_t(address) + size;
    if (end > kPhysicalBytes) {
      Abort("renderer requested a capture read outside physical memory"); return;
    }
    for (uint32_t page = address / kPageBytes;
         page < (end + kPageBytes - 1) / kPageBytes; ++page) {
      if (page_epochs_[page] == read_epoch_) continue;
      // One renderer callback consumes one version of every page, including
      // later reads after a concurrent write to its live CPU allocation.
      page_epochs_[page] = read_epoch_;
      const uint64_t token = watch_->Token(page);
      if (token && token == shadow_tokens_[page]) continue;
      if (!SnapshotPage(page, token) && (!restore_pending_ || !input_switched_)) return;
    }
  }

  bool SnapshotPage(uint32_t page, uint64_t before_token) {
    std::array<uint8_t, kPageBytes> bytes;
    const auto offset = size_t(page) * kPageBytes;
    std::memcpy(bytes.data(), live_base_ + offset, kPageBytes);
    const uint64_t after_token = watch_->Token(page);
    shadow_tokens_[page] = before_token && before_token == after_token ? before_token : 0;
    if (std::memcmp(bytes.data(), shadow_ + offset, kPageBytes) == 0) return true;
    if (!active_) {
      std::memcpy(shadow_ + offset, bytes.data(), kPageBytes);
      return true;
    }
    // Counter-heavy guest CPU pages often differ in only a few bytes. Store
    // exact changes against the preceding frozen page, merging short gaps
    // so metadata does not overwhelm small nearby writes.
    std::array<std::pair<uint32_t, uint32_t>, kPageBytes / 8> runs;
    size_t run_count = 0, cursor = 0, patch_size = 8;
    const uint8_t* previous = shadow_ + offset;
    while (cursor < kPageBytes) {
      while (cursor < kPageBytes && bytes[cursor] == previous[cursor]) ++cursor;
      if (cursor == kPageBytes) break;
      const size_t begin = cursor;
      size_t end = cursor + 1;
      for (;;) {
        cursor = end;
        while (cursor < kPageBytes && bytes[cursor] == previous[cursor]) ++cursor;
        if (cursor == kPageBytes || cursor - end > 8) break;
        end = cursor + 1;
      }
      runs[run_count++] = {uint32_t(begin), uint32_t(end - begin)};
      patch_size += 8 + end - begin;
    }
    trace::ByteWriter patch;
    if (patch_size < kPageBytes + 4) {
      patch.U32(1); patch.U32(uint32_t(run_count));
      for (const auto& [begin, length] : std::span(runs).first(run_count)) {
        patch.U32(begin); patch.U32(length);
        patch.Bytes(std::span<const uint8_t>(bytes).subspan(begin, length));
      }
    } else {
      patch.U32(0); patch.Bytes(bytes);
    }
    const uint64_t blob = PutBlob(patch.Data());
    // Abort defers releasing this image while the inner callback still uses
    // it. Even a failed record must populate the page needed by that draw.
    if (shadow_) std::memcpy(shadow_ + offset, bytes.data(), kPageBytes);
    if (!active_) return false;
    trace::ByteWriter event; event.U32(page); event.U64(blob);
    if (!Emit(trace::EventKind::kMemoryPage, event)) return false;
    ++pages_recorded_;
    page_patch_bytes_ += patch.Data().size();
    return true;
  }

  void SynchronizeRegisters(const GpuRegisterFile& registers) {
    if (std::memcmp(registers_.values, registers.values, sizeof(registers_.values)) == 0) return;
    std::memcpy(registers_.values, registers.values, sizeof(registers_.values));
    if (active_) EmitRegisterFile();
  }
  bool EmitRegisterFile() {
    trace::ByteWriter words;
    for (uint32_t value : registers_.values) words.U32(value);
    const uint64_t blob = PutBlob(words.Data());
    if (!active_) return false;
    trace::ByteWriter event; event.U64(blob);
    return Emit(trace::EventKind::kRegisterFile, event);
  }
  bool EmitRegisterWrite(uint32_t index, uint32_t value) {
    trace::ByteWriter event; event.U32(index); event.U32(value);
    return Emit(trace::EventKind::kRegisterWrite, event);
  }
  bool EmitShader(uint32_t type, const ShaderSnapshot& shader) {
    const uint64_t blob = PutBlob(std::as_bytes(std::span(shader.words)));
    if (!active_) return false;
    trace::ByteWriter event;
    event.U32(type); event.U32(shader.address); event.U32(uint32_t(shader.words.size()));
    event.U64(blob);
    return Emit(trace::EventKind::kShaderLoad, event);
  }
  uint64_t PutBlob(std::span<const std::byte> bytes) {
    return PutBlob(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  }
  uint64_t PutBlob(std::span<const uint8_t> bytes) {
    std::string error;
    const uint64_t id = writer_->PutBlob(bytes, error);
    if (!id) Abort(error);
    return id;
  }
  bool Emit(trace::EventKind kind, const trace::ByteWriter& event) {
    if (!active_) return false;
    std::string error;
    if (!writer_->WriteEvent(kind, event.Data(), error)) { Abort(error); return false; }
    return true;
  }
  void FinishCapture() {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    if (!renderer_->CaptureFrame(pixels, width, height) || !width || !height ||
        uint64_t(width) * height * 4 != pixels.size()) {
      Abort("the final swap did not produce a tightly packed RGBA reference"); return;
    }
    const uint64_t blob = PutBlob(pixels);
    if (!active_) return;
    trace::ByteWriter reference;
    reference.U32(width); reference.U32(height);
    reference.U64(XXH3_64bits(pixels.data(), pixels.size())); reference.U64(blob);
    const trace::ByteWriter empty;
    if (!Emit(trace::EventKind::kFrameReference, reference) ||
        !Emit(trace::EventKind::kEndCapture, empty)) return;
    std::string error;
    if (!writer_->Finish(error)) { Abort(error); return; }
    const auto statistics = writer_->GetStatistics();
    std::fprintf(stderr, "xn-render-trace: completed %s swaps=%llu pages=%llu patch_bytes=%llu "
                         "events=%llu bytes=%llu reference=%ux%u\n", path_.string().c_str(),
                 static_cast<unsigned long long>(captured_swaps_),
                 static_cast<unsigned long long>(pages_recorded_),
                 static_cast<unsigned long long>(page_patch_bytes_),
                 static_cast<unsigned long long>(statistics.events),
                 static_cast<unsigned long long>(statistics.file_bytes), width, height);
    done_ = true;
    active_ = false;
    RestoreInput();
    writer_.reset();
    gamma_history_.clear();
  }
  void Abort(const std::string& reason, bool restore_live_base = false) {
    if (!done_) std::fprintf(stderr, "xn-render-trace: aborted: %s\n", reason.c_str());
    done_ = true;
    active_ = false;
    if (texture_proxy_) texture_proxy_->EndCall();
    RestoreInput(restore_live_base);
    writer_.reset();  // No footer: an interrupted file must never replay.
  }
  void RestoreInput(bool restore_live_base = false) {
    if (backend_depth_ && !restore_live_base) {
      restore_pending_ = true;
      return;
    }
    if (input_switched_) {
      renderer_->SetGuestMemory(live_base_, kPhysicalBytes);
      input_switched_ = false;
    }
    if (backend_depth_) {
      // CPU writeback needs the actual base immediately, but source pointers
      // cached by an outer renderer call must remain mapped until it returns.
      restore_pending_ = true;
      return;
    }
    if (shadow_) { munmap(shadow_, kPhysicalBytes); shadow_ = nullptr; }
    restore_pending_ = false;
  }

  std::recursive_mutex mutex_;
  bool initialized_ = false, active_ = false;
  bool done_ = false, input_switched_ = false, restore_pending_ = false;
  bool first_draw_snapshot_ = false;
  uint32_t backend_depth_ = 0;
  std::shared_ptr<CaptureTextureSource> texture_proxy_;
  std::function<void(uint32_t, uint32_t)> upstream_prepare_;
  gfx::VulkanRendererConfig config_;
  std::unique_ptr<gfx::VulkanRenderer> renderer_;
  uint8_t* live_base_ = nullptr;
  uint8_t* shadow_ = nullptr;
  PhysicalWriteWatch* watch_ = nullptr;
  GpuRegisterFile registers_{};
  std::array<ShaderSnapshot, 2> shaders_;
  std::vector<std::pair<uint32_t, uint32_t>> gamma_history_;
  std::vector<uint64_t> shadow_tokens_, page_epochs_;
  std::filesystem::path path_;
  std::unique_ptr<trace::Writer> writer_;
  uint64_t start_swap_ = 1500, frame_count_ = 9, swaps_seen_ = 0;
  uint64_t captured_swaps_ = 0, pages_recorded_ = 0, page_patch_bytes_ = 0, read_epoch_ = 0;
};

}  // namespace

IRenderer* CreateRenderCapture(gfx::VulkanRendererConfig config) {
  return new RenderCapture(std::move(config));
}

}  // namespace xn
