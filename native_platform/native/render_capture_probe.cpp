#ifdef NDEBUG
#undef NDEBUG
#endif
// Links the capture wrapper with a fake backend, never a Vulkan device. The
// memory protection and trace container are the actual runtime implementations.
#include "gfx/render_trace.h"
#include "gfx/vulkan/render_capture.h"
#include "gfx/vulkan/vulkan_renderer.h"
#include "memory/physical_write_watch.h"
#include <array>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {
constexpr size_t kMemory = size_t(512) << 20;
uint8_t *live;
uint8_t *backend_input;
uint8_t draw_value;
bool mutate_during_draw;
unsigned synchronizations;
bool query_host_texture;
std::vector<uint8_t> host_bytes;
bool expect_deferred_io_abort;
xn::IRenderer *nested_writeback;
class HostTexture final : public xn::gfx::TextureUploadSource {
public:
  mutable unsigned revisions = 0, reads = 0;
  unsigned draws = 0;
  void OnDraw(uint64_t, const xn::GpuRegisterFile &) override { ++draws; }
  uint64_t Revision(uint32_t address, uint32_t, uint32_t,
                    uint32_t) const override {
    ++revisions;
    return address == 0x12000 ? 7 : 0;
  }
  bool Read(uint32_t, uint32_t, uint32_t, uint32_t,
            std::vector<uint8_t> &bytes) const override {
    bytes = {uint8_t(++reads), 19, 23, 255};
    return true;
  }
};
} // namespace
namespace xn::gfx {
struct VulkanRenderer::Impl {
  std::shared_ptr<TextureUploadSource> textures;
  std::function<void(uint32_t, uint32_t)> prepare;
};
VulkanRenderer::VulkanRenderer(const VulkanRendererConfig &config)
    : impl_(new Impl) {
  impl_->textures = config.texture_source;
  impl_->prepare = config.prepare_guest_read;
}
VulkanRenderer::~VulkanRenderer() = default;
bool VulkanRenderer::Initialize(SDL_Window *, uint8_t *, uint8_t *p) {
  backend_input = p;
  return true;
}
void VulkanRenderer::Shutdown() {}
void VulkanRenderer::SetGuestMemory(uint8_t *p, size_t n) {
  assert(n == kMemory);
  backend_input = p;
}
void VulkanRenderer::OnRegisterWrite(uint32_t, uint32_t) {}
void VulkanRenderer::OnConstants(uint32_t, const uint32_t *, uint32_t) {}
void VulkanRenderer::OnRegisterBatch(const GpuRegisterFile &,
                                     std::span<const RegisterRange>) {}
void VulkanRenderer::OnDrawWithRegisterBatch(const DrawPacket &draw,
                                             const GpuRegisterFile &regs,
                                             std::span<const RegisterRange> ranges) {
  OnRegisterBatch(regs, ranges);
  OnDraw(draw, regs);
}
void VulkanRenderer::OnShaderLoad(uint32_t, uint32_t, uint32_t,
                                  const uint32_t *) {}
void VulkanRenderer::OnDraw(const DrawPacket &, const GpuRegisterFile &regs) {
  uint8_t *cached_source = backend_input;
  if (impl_->prepare)
    impl_->prepare(0, 16384);
  if (expect_deferred_io_abort) {
    assert(backend_input == cached_source && cached_source != live);
    assert(cached_source[0] == live[0] && cached_source[12288] == live[12288]);
  }
  if (nested_writeback) {
    const uint8_t expected = cached_source[0];
    assert(nested_writeback->SynchronizeTextureToGuest(123, 1, 1, 6));
    assert(backend_input == live && cached_source != live &&
           cached_source[0] == expected);
  }
  if (mutate_during_draw) {
    assert(backend_input != live);
    live[0] = 99;
  }
  if (impl_->prepare)
    impl_->prepare(0, 1); // same callback must keep its first snapshot
  draw_value = backend_input[0];
  if (query_host_texture) {
    assert(impl_->textures);
    impl_->textures->OnDraw(42, regs);
    assert(impl_->textures->Revision(0x12000, 1, 1, 6) == 7);
    assert(impl_->textures->Revision(0x12000, 1, 1, 6) == 7);
    assert(impl_->textures->Read(0x12000, 1, 1, 6, host_bytes));
    std::vector<uint8_t> repeated;
    assert(impl_->textures->Read(0x12000, 1, 1, 6, repeated) &&
           repeated == host_bytes);
    assert(impl_->textures->Revision(0x13000, 1, 1, 6) == 0);
  }
}
void VulkanRenderer::OnEvent(uint32_t, const GpuRegisterFile &) {}
void VulkanRenderer::OnSwap(uint32_t, uint32_t, uint32_t) {
  if (impl_->prepare)
    impl_->prepare(0, 4097);
  if (query_host_texture)
    assert(impl_->textures->Revision(0x12000, 1, 1, 6) == 7);
}
void VulkanRenderer::Present() {}
void VulkanRenderer::SetFrameRateTarget(uint32_t) {}
void VulkanRenderer::FlushCaches() {}
bool VulkanRenderer::CaptureFrame(std::vector<uint8_t> &out, uint32_t &w,
                                  uint32_t &h) {
  w = h = 1;
  out = {backend_input[0], backend_input[4096], 0, 255};
  return true;
}
bool VulkanRenderer::SynchronizeTextureToGuest(uint32_t a, uint32_t, uint32_t,
                                               uint32_t) {
  assert(backend_input == live);
  backend_input[a] = 211;
  ++synchronizations;
  return true;
}
bool VulkanRenderer::OnD3DCall(const D3DCall &) { return false; }
} // namespace xn::gfx

int main() {
  using namespace xn;
  namespace trace = xn::gfx::trace;
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("forza4-render-capture-probe-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto physical_path = directory / "physical.trace";
  const auto aborted_path = directory / "aborted.trace";
  const auto host_path = directory / "host.trace";
  const auto io_path = directory / "io-abort.trace";
  const auto nested_path = directory / "nested-writeback.trace";
  live = static_cast<uint8_t *>(mmap(nullptr, kMemory, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(live != MAP_FAILED);
  {
    const PhysicalWriteWatch::Alias alias{live, kMemory, 0};
    PhysicalWriteWatch watch(live, kMemory, std::span(&alias, 1));
    assert(watch.Enable());
    setenv("XN_RENDER_TRACE", physical_path.c_str(), 1);
    setenv("XN_RENDER_TRACE_START_SWAP", "1", 1);
    setenv("XN_RENDER_TRACE_FRAMES", "2", 1);
    auto capture = std::unique_ptr<IRenderer>(CreateRenderCapture({}));
    assert(capture->Initialize(nullptr, live, live));
    capture->OnRegisterWrite(0x1921, 0x1234);
    const uint32_t shader[] = {0x01000000, 0x02000000, 0x03000000};
    capture->OnShaderLoad(0, 0x5000, 3, shader);
    capture->OnSwap(0, 1, 1);
    live[0] = 17;
    live[4096] = 23;
    live[8192] = 1;
    live[8201] = 2;
    live[8211] = 3;
    std::memset(live + 12288, 0xAB, 4096);
    GpuRegisterFile regs;
    regs.values[0x1921] = 0x1234;
    DrawPacket draw{};
    draw.opcode = 0x22;
    draw.prim_type = 4;
    draw.source = 2;
    draw.index_count = 3;
    draw.registers_notified = true;
    mutate_during_draw = true;
    capture->OnDraw(draw, regs);
    assert(draw_value == 17);
    assert(live[0] == 99);
    const auto faults = watch.fault_count();
    live[1 << 20] = 55;
    assert(watch.fault_count() ==
           faults); // unrelated CPU pages were never armed
    mutate_during_draw = false;
    capture->OnSwap(0, 1, 1);
    assert(backend_input != live);
    live[0] = 42;
    capture->OnDraw(draw, regs);
    assert(draw_value == 42);
    capture->OnSwap(0, 1, 1);
    assert(backend_input == live);
    assert(watch.BeginTrace());
    watch.EndTrace();
    trace::Reader reader(physical_path);
    std::string error;
    assert(reader.Open(error));
    trace::Event event;
    unsigned swaps = 0, refs = 0, ends = 0, gammas = 0, shaders = 0;
    std::map<uint32_t, std::array<uint8_t, 4096>> reconstructed;
    bool sparse_runs = false, literal = false;
    bool first_draw = true;
    while (reader.Next(event, error)) {
      trace::ByteReader bytes(event.payload);
      if (event.kind == trace::EventKind::kMemoryPage) {
        uint32_t page;
        uint64_t blob;
        assert(bytes.U32(page) && bytes.U64(blob) && bytes.Done());
        std::vector<uint8_t> patch;
        assert(reader.ReadBlob(blob, patch, error));
        trace::ByteReader data(patch);
        uint32_t encoding;
        assert(data.U32(encoding));
        if (!encoding) {
          std::span<const uint8_t> raw;
          assert(data.Bytes(4096, raw) && data.Done());
          std::memcpy(reconstructed[page].data(), raw.data(), 4096);
          if (page == 3)
            literal = true;
        } else {
          uint32_t count;
          assert(encoding == 1 && data.U32(count));
          if (page == 2)
            sparse_runs = count == 2;
          for (uint32_t i = 0; i < count; ++i) {
            uint32_t offset, length;
            std::span<const uint8_t> raw;
            assert(data.U32(offset) && data.U32(length) &&
                   offset + length <= 4096 && data.Bytes(length, raw));
            std::memcpy(reconstructed[page].data() + offset, raw.data(),
                        length);
          }
          assert(data.Done());
        }
      }
      if (event.kind == trace::EventKind::kSwap)
        ++swaps;
      if (event.kind == trace::EventKind::kShaderLoad)
        ++shaders;
      if (event.kind == trace::EventKind::kRegisterWrite)
        ++gammas;
      if (event.kind == trace::EventKind::kEndCapture)
        ++ends;
      if (event.kind == trace::EventKind::kDraw && first_draw) {
        std::array<uint32_t, 10> fields;
        for (auto &field : fields)
          assert(bytes.U32(field));
        assert(fields[9] == 0);
        first_draw = false; // fresh GPU needs full register seed
      }
      if (event.kind == trace::EventKind::kFrameReference) {
        uint32_t w, h;
        uint64_t hash, blob;
        assert(bytes.U32(w) && bytes.U32(h) && bytes.U64(hash) &&
               bytes.U64(blob) && bytes.Done());
        std::vector<uint8_t> pixels;
        assert(reader.ReadBlob(blob, pixels, error));
        assert(w == 1 && h == 1 &&
               pixels == std::vector<uint8_t>({42, 23, 0, 255}));
        ++refs;
      }
    }
    assert(error.empty() && swaps == 2 && refs == 1 && ends == 1 &&
           gammas == 1 && shaders == 1);
    assert(sparse_runs && literal && reconstructed[0][0] == 42 &&
           reconstructed[1][0] == 23);
    assert(reconstructed.size() == 4); // unrelated CPU activity is not captured
    assert(reconstructed[2][0] == 1 && reconstructed[2][9] == 2 &&
           reconstructed[2][19] == 3);
    assert(reconstructed[3][4095] == 0xAB);
    capture->Shutdown();
    setenv("XN_RENDER_TRACE", aborted_path.c_str(), 1);
    setenv("XN_RENDER_TRACE_START_SWAP", "0", 1);
    auto aborted = std::unique_ptr<IRenderer>(CreateRenderCapture({}));
    assert(aborted->Initialize(nullptr, live, live));
    aborted->OnRegisterWrite(0x2000, 1);
    assert(backend_input != live);
    assert(aborted->SynchronizeTextureToGuest(123, 1, 1, 6));
    assert(backend_input == live && live[123] == 211 && synchronizations == 1);
    assert(watch.BeginTrace());
    watch.EndTrace();
    trace::Reader incomplete(aborted_path);
    assert(!incomplete.Open(error));
    aborted->Shutdown();
    setenv("XN_RENDER_TRACE", host_path.c_str(), 1);
    setenv("XN_RENDER_TRACE_FRAMES", "1", 1);
    auto source = std::make_shared<HostTexture>();
    gfx::VulkanRendererConfig host_config;
    host_config.texture_source = source;
    auto host_capture =
        std::unique_ptr<IRenderer>(CreateRenderCapture(host_config));
    assert(host_capture->Initialize(nullptr, live, live));
    query_host_texture = true;
    host_capture->OnDraw(draw, regs);
    assert(source->draws == 1 && source->revisions == 2 && source->reads == 1 &&
           host_bytes[0] == 1);
    host_capture->OnSwap(0, 1, 1);
    assert(source->reads == 2 &&
           source->revisions == 3); // proactive Read despite backend cache hit
    query_host_texture = false;
    trace::Reader host_reader(host_path);
    assert(host_reader.Open(error));
    unsigned host_revisions = 0, host_reads = 0;
    bool saw_draw = false;
    while (host_reader.Next(event, error)) {
      if (event.kind == trace::EventKind::kCaptureInfo) {
        trace::ByteReader info(event.payload);
        uint32_t version, frames, flags;
        uint64_t memory;
        assert(info.U32(version) && info.U64(memory) && info.U32(frames) &&
               info.U32(flags) && info.Done());
        assert(version == 2 && flags == 1 && frames == 1);
      }
      if (event.kind == trace::EventKind::kTextureRevision)
        ++host_revisions;
      if (event.kind == trace::EventKind::kTextureRead)
        ++host_reads;
      if (event.kind == trace::EventKind::kDraw) {
        assert(host_revisions == 2 && host_reads == 1);
        saw_draw = true;
      }
    }
    assert(error.empty() && saw_draw && host_revisions == 3 && host_reads == 2);
    host_capture->Shutdown();

    // A failed disk write inside prepare_guest_read must not release a source
    // pointer cached by the backend earlier in that same callback.
    setenv("XN_RENDER_TRACE", io_path.c_str(), 1);
    setenv("XN_RENDER_TRACE_FRAMES", "2", 1);
    auto io_capture = std::unique_ptr<IRenderer>(CreateRenderCapture({}));
    assert(io_capture->Initialize(nullptr, live, live));
    io_capture->OnRegisterWrite(0x2000,
                                1); // bootstrap is written before limiting I/O
    for (size_t page = 0; page < 4; ++page)
      std::memset(live + page * 4096, int(0x41 + page), 4096);
    struct rlimit old_limit, limited;
    assert(getrlimit(RLIMIT_FSIZE, &old_limit) == 0);
    limited = old_limit;
    limited.rlim_cur = std::filesystem::file_size(io_path);
    struct sigaction ignored{}, previous{};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    assert(sigaction(SIGXFSZ, &ignored, &previous) == 0);
    assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
    expect_deferred_io_abort = true;
    GpuRegisterFile io_registers;
    io_registers.values[0x2000] = 1;
    io_capture->OnDraw(draw, io_registers);
    expect_deferred_io_abort = false;
    assert(setrlimit(RLIMIT_FSIZE, &old_limit) == 0);
    assert(sigaction(SIGXFSZ, &previous, nullptr) == 0);
    assert(backend_input == live);
    trace::Reader io_incomplete(io_path);
    assert(!io_incomplete.Open(error));
    io_capture->Shutdown();

    // Nested CPU writeback switches the backend to live memory immediately,
    // while the outer callback's old shadow source remains mapped until exit.
    setenv("XN_RENDER_TRACE", nested_path.c_str(), 1);
    auto nested_capture = std::unique_ptr<IRenderer>(CreateRenderCapture({}));
    assert(nested_capture->Initialize(nullptr, live, live));
    nested_writeback = nested_capture.get();
    nested_capture->OnDraw(draw, regs);
    nested_writeback = nullptr;
    assert(backend_input == live && live[123] == 211 && synchronizations == 2);
    trace::Reader nested_incomplete(nested_path);
    assert(!nested_incomplete.Open(error));
    nested_capture->Shutdown();
  }
  munmap(live, kMemory);
  std::filesystem::remove_all(directory);
  std::puts("PASS: capture page isolation, patches, host inputs, fresh "
            "bootstrap and safe aborts");
}
