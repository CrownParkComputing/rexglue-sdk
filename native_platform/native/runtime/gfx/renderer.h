// xenon-native: the renderer seam. W3 implements IRenderer; the runtime owns
// everything above it (Vd* imports, the ring walk, the window/event loop).
//
// Two feeds reach the renderer:
//   1. Ring-level (always on): the command processor walks the PM4 stream the
//      title's D3D writes and reports draws, register state and the swap.
//   2. D3D9-level (band hooks in titles/<title>/d3d9_hooks.cpp): the entry
//      points of the title's statically-linked D3D are intercepted before they
//      run; the hook forwards the raw guest arguments and lets the renderer
//      decide whether to consume the call (return true) or let the original
//      run (return false, the default for the stub).
#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include "register_file.h"

struct SDL_Window;

namespace xn {

struct RegisterRange {
  uint32_t index = 0;
  uint32_t count = 0;
};

struct D3DCall {
  uint32_t address;         // guest function address of the entry point
  std::string_view name;    // "sub_8202...." or a human name when known
  uint32_t gpr_count;       // how many of r3.. carry arguments (from arity.json)
  const uint64_t* gprs;     // r3..r10 (8 entries)
  const double* fprs;       // f1..f8 (8 entries)
  uint8_t* base;            // guest memory base for pointer arguments
};

struct DrawPacket {
  uint32_t opcode;          // PM4_DRAW_INDX (0x22) or PM4_DRAW_INDX_2 (0x36)
  uint32_t prim_type;       // xenos primitive type (VGT_DRAW_INITIATOR bits 0-5)
  uint32_t index_count;
  uint32_t source;          // 0 = DMA (index buffer at index_base), 2 = auto-index, 1 = immediate
  uint32_t index_base;      // physical address of the index buffer (DMA)
  uint32_t index_size;      // bytes
  bool index_32bit;
  uint32_t index_endian;
  const uint32_t* immediate;  // immediate indices (DRAW_INDX_2), big-endian words
  uint32_t immediate_words;
  // The CP has delivered every preceding register/constant write. Other
  // callers leave this false and supply a complete standalone snapshot.
  bool registers_notified = false;
};

class IRenderer {
 public:
  virtual ~IRenderer() = default;
  // Called on the host main thread before the guest starts (window may be null when headless).
  virtual bool Initialize(SDL_Window* window, uint8_t* guest_base, uint8_t* physical_base) = 0;
  virtual void Shutdown() = 0;

  // --- ring feed (command processor thread) ---
  // A GPU register written by a type-0 packet (or CP-internal update).
  virtual void OnRegisterWrite(uint32_t index, uint32_t value) = 0;
  // Shader microcode load (IM_LOAD / IM_LOAD_IMMEDIATE): type 0 vertex, 1 pixel.
  virtual void OnShaderLoad(uint32_t shader_type, uint32_t physical_address, uint32_t size_dwords,
                            const uint32_t* dwords) = 0;
  // Most implementations keep the historical ordering contract. A backend
  // that only consumes microcode may let plain register updates stay pending
  // across shader loads; draw/event/swap barriers are still mandatory.
  virtual bool RequiresRegistersForShaderLoad() const { return true; }
  // Contiguous draw-state or ALU/fetch/bool/loop registers from type-0,
  // SET_CONSTANT or LOAD_ALU_CONSTANT packets.
  // Words retain guest big-endian byte order; these need no per-word callback.
  virtual void OnConstants(uint32_t register_index, const uint32_t* dwords, uint32_t count) = 0;
  // Ordered update of side-effect-free register storage in HOST byte order.
  // The ranges reference the CP's current file only for this synchronous
  // callback; implementations must consume them before returning. The default
  // preserves compatibility for renderers without a batched implementation.
  virtual void OnRegisterBatch(const GpuRegisterFile& registers,
                               std::span<const RegisterRange> ranges) {
    for (const auto& range : ranges) {
      if (range.index >= GpuRegisterFile::kCount ||
          range.count > GpuRegisterFile::kCount - range.index) continue;
      for (uint32_t i = 0; i < range.count; ++i)
        OnRegisterWrite(range.index + i, registers.values[range.index + i]);
    }
  }
  // A draw. Register state at this point is in `regs`.
  virtual void OnDraw(const DrawPacket& draw, const GpuRegisterFile& regs) = 0;
  // An ordered register batch immediately followed by a draw. Ranges are
  // valid only for this synchronous call. Wrappers and other renderers keep
  // their existing callback sequence through this compatibility default.
  virtual void OnDrawWithRegisterBatch(const DrawPacket& draw,
                                       const GpuRegisterFile& registers,
                                       std::span<const RegisterRange> ranges) {
    OnRegisterBatch(registers, ranges);
    OnDraw(draw, registers);
  }
  // EVENT_WRITE / EVENT_WRITE_SHD initiator (resolves are driven by register
  // state + RB_COPY_* and are reported here through the initiator value).
  virtual void OnEvent(uint32_t initiator, const GpuRegisterFile& regs) = 0;
  // VdSwap reached the ring: the front buffer at `physical_address` (w x h,
  // k_8_8_8_8) is complete. Runs on the CP thread; presentation happens on the
  // host main thread via Present().
  virtual void OnSwap(uint32_t physical_address, uint32_t width, uint32_t height) = 0;

  // --- host main thread, once per host frame ---
  virtual void Present() = 0;
  // Host HUD, composited after the game's gamma/bloom passes. Empty hides it.
  // Called by the SDL owner thread; implementations synchronize with OnSwap.
  virtual void SetFpsOverlay(std::string_view text) { (void)text; }
  // Presentation may need a nonblocking mode above the desktop refresh rate.
  // Guest timing remains the caller's responsibility.
  virtual void SetFrameRateTarget(uint32_t fps) { (void)fps; }
  // Optional (oracle harness, W5): copy the most recently presented image as
  // tightly packed RGBA8. Called on the host main thread after Present().
  // Return false when nothing has been presented yet or the renderer cannot
  // read back; the frame recorder then falls back to the draw log.
  virtual bool CaptureFrame(std::vector<uint8_t>& rgba8, uint32_t& width, uint32_t& height) {
    (void)rgba8; (void)width; (void)height;
    return false;
  }

  // A guest texture lock is about to consume GPU-written pixels on the CPU.
  // Materialize only that resolved base image, in guest byte order/layout.
  // False means no matching supported GPU image is available.
  virtual bool SynchronizeTextureToGuest(uint32_t address, uint32_t width,
                                         uint32_t height, uint32_t format) {
    (void)address; (void)width; (void)height; (void)format;
    return false;
  }

  // --- D3D9 band feed (guest thread) ---
  // Return true to consume the call (the original guest code does not run).
  virtual bool OnD3DCall(const D3DCall& call) = 0;
  // Persist optional compilation caches before titles exit without unwinding.
  virtual void FlushCaches() {}
};

IRenderer* CreateStubRenderer();

}  // namespace xn
