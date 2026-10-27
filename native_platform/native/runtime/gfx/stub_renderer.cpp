// The stub renderer: no GPU, just an account of what the title asked for.
#include <atomic>
#include <chrono>
#include <unordered_map>

#include <rex/logging.h>

#include "command_processor.h"
#include "renderer.h"

namespace xn {
namespace {

class StubRenderer : public IRenderer {
 public:
  bool Initialize(SDL_Window*, uint8_t*, uint8_t*) override { return true; }
  void Shutdown() override {}
  void OnRegisterWrite(uint32_t, uint32_t) override { register_writes_++; }
  void OnShaderLoad(uint32_t shader_type, uint32_t address, uint32_t size, const uint32_t*) override {
    if (shader_loads_++ < 12)
      REXGPU_INFO("stub: shader load type={} addr={:#010x} {} dwords", shader_type, address, size);
  }
  void OnConstants(uint32_t, const uint32_t*, uint32_t) override { constant_loads_++; }
  void OnRegisterBatch(const GpuRegisterFile&, std::span<const RegisterRange>) override {
    constant_loads_++;
  }
  void OnDraw(const DrawPacket& draw, const GpuRegisterFile& regs) override {
    frame_draws_++;
    if (logged_draws_++ < 16) {
      REXGPU_INFO("stub: draw op={:#x} prim={} count={} src={} idx={:#010x}{} vs={:#x} ps={:#x}",
                  draw.opcode, draw.prim_type, draw.index_count, draw.source, draw.index_base,
                  draw.index_32bit ? " 32bit" : "", regs[0x2180] /*SQ_PROGRAM_CNTL*/, regs[0x2007]);
    }
  }
  void OnEvent(uint32_t initiator, const GpuRegisterFile&) override { events_++; (void)initiator; }
  void OnSwap(uint32_t physical_address, uint32_t width, uint32_t height) override {
    swaps_++;
    auto now = std::chrono::steady_clock::now();
    if (last_report_ == std::chrono::steady_clock::time_point{}) last_report_ = now;
    double dt = std::chrono::duration<double>(now - last_report_).count();
    if (swaps_ <= 3 || dt >= 2.0) {
      REXGPU_INFO("stub: swap #{} frontbuffer={:#010x} {}x{} | {:.1f} swaps/s, {} draws/frame, "
                  "{} reg writes, {} const loads, {} events",
                  swaps_, physical_address, width, height,
                  dt > 0 ? double(swaps_ - swaps_at_report_) / dt : 0.0, frame_draws_,
                  register_writes_, constant_loads_, events_);
      if (dt >= 2.0) { last_report_ = now; swaps_at_report_ = swaps_; }
    }
    frame_draws_ = 0;
  }
  void Present() override {}
  bool OnD3DCall(const D3DCall& call) override {
    uint32_t& n = d3d_calls_[call.address];
    if (n++ < 2) {
      REXGPU_DEBUG("stub: d3d {} ({:#010x}) r3={:#x} r4={:#x} r5={:#x} r6={:#x}", call.name,
                   call.address, call.gprs[0], call.gprs[1], call.gprs[2], call.gprs[3]);
    }
    return false;  // let the guest's own D3D run so the ring stays authoritative
  }

 private:
  uint64_t swaps_ = 0, swaps_at_report_ = 0, frame_draws_ = 0, logged_draws_ = 0;
  uint64_t register_writes_ = 0, constant_loads_ = 0, events_ = 0, shader_loads_ = 0;
  std::chrono::steady_clock::time_point last_report_{};
  std::unordered_map<uint32_t, uint32_t> d3d_calls_;
};

}  // namespace

IRenderer* CreateStubRenderer() { return new StubRenderer(); }

}  // namespace xn
