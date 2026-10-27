/**
 ******************************************************************************
 * xenon-native - VulkanRenderer
 ******************************************************************************
 * xn::IRenderer (gfx/renderer.h, the PM4 ring feed the runtime's command
 * processor drives) on SDL3 + Vulkan 1.3 dynamic rendering, plus the Shape A
 * D3D9-hook entry points (shape_a.h). See docs/RENDERER.md.
 */
#pragma once

#include <memory>
#include <vector>

#include "gfx/renderer.h"
#include "gfx/host_overlay.h"
#include "shape_a.h"

namespace xn::gfx {

class VulkanRenderer final : public xn::IRenderer {
 public:
  explicit VulkanRenderer(const VulkanRendererConfig& config = VulkanRendererConfig());
  ~VulkanRenderer() override;

  // --- xn::IRenderer (ring feed; CP thread, Present on the main thread) ---
  bool Initialize(SDL_Window* window, uint8_t* guest_base, uint8_t* physical_base) override;
  void Shutdown() override;
  void OnRegisterWrite(uint32_t index, uint32_t value) override;
  void OnShaderLoad(uint32_t shader_type, uint32_t physical_address, uint32_t size_dwords,
                    const uint32_t* dwords) override;
  bool RequiresRegistersForShaderLoad() const override { return false; }
  void OnConstants(uint32_t register_index, const uint32_t* dwords, uint32_t count) override;
  void OnRegisterBatch(const GpuRegisterFile& registers,
                       std::span<const RegisterRange> ranges) override;
  void OnDraw(const DrawPacket& draw, const GpuRegisterFile& regs) override;
  void OnDrawWithRegisterBatch(const DrawPacket& draw, const GpuRegisterFile& registers,
                               std::span<const RegisterRange> ranges) override;
  void OnEvent(uint32_t initiator, const GpuRegisterFile& regs) override;
  void OnSwap(uint32_t physical_address, uint32_t width, uint32_t height) override;
  void Present() override;
  void SetFrameRateTarget(uint32_t fps) override;
  void SetHostOverlay(std::shared_ptr<const HostUiFrame> frame);
  void SetPresentationSettings(const PresentationSettings& settings);
  std::string DeviceName() const;
  uint64_t PresentedFrames() const;
  void RedrawHostOverlay();
  void FlushCaches() override;
  bool CaptureFrame(std::vector<uint8_t>& rgba8, uint32_t& width, uint32_t& height) override;
  bool SynchronizeTextureToGuest(uint32_t address, uint32_t width,
                                 uint32_t height, uint32_t format) override;
  bool OnD3DCall(const D3DCall& call) override;

  // --- Shape A (D3D9 band hooks; guest thread) ---
  // Offscreen/test initialisation without the runtime: window may be nullptr.
  bool Init(SDL_Window* window);
  // Host mapping of guest physical memory: base + physical address; size is
  // the extent the game may reference.
  void SetGuestMemory(uint8_t* base, size_t size);
  void SetRenderTarget(uint32_t index, const SurfaceDesc& surface);
  void SetDepthStencil(const SurfaceDesc& surface);
  void Draw(const DeviceShadow& device, const DrawParams& params);
  void Clear(const DeviceShadow& device, const ClearParams& params);
  void Resolve(const DeviceShadow& device, const ResolveParams& params);
  void Present(const PresentParams& params);
  // The game wrote guest memory the renderer may have cached (texture upload,
  // dynamic vertex buffer unlock): drop cached copies overlapping the range.
  void InvalidateGuestRange(uint32_t guest_address, uint32_t size);
  RendererStats GetStats() const;

  // Tests: flushes pending GPU work and reads a colour target back as RGBA8.
  bool DebugReadbackColorTarget(uint32_t edram_base, std::vector<uint8_t>& rgba8_out,
                                uint32_t& width_out, uint32_t& height_out);
  // Tests: same for a texture the renderer resolved into (by guest address).
  bool DebugReadbackResolvedTexture(uint32_t guest_address, std::vector<uint8_t>& rgba8_out,
                                    uint32_t& width_out, uint32_t& height_out);
  bool DebugReadbackDepthPixel(uint32_t edram_base, uint32_t x, uint32_t y, float& depth_out);
  bool DebugReadbackGuestTexture(const GpuRegisterFile& registers, uint32_t fetch_slot,
                                 std::vector<uint8_t>& rgba8_out, bool signed_view = false);

  // Tests: draws a fixed cyan triangle with hand-written SPIR-V into the
  // colour target the register file selects (no vertex fetch, no constants).
  bool DebugDrawTriangle(const GpuRegisterFile& regs);

  struct Impl;

 private:
  void ApplyRegisterBatchUnlocked(const GpuRegisterFile& registers,
                                  std::span<const RegisterRange> ranges);
  void DrawUnlocked(const DrawPacket& draw, const GpuRegisterFile& regs);
  std::unique_ptr<Impl> impl_;
};

}  // namespace xn::gfx

namespace xn::gfx {
// Routes the renderer's (and the vendored translator's) log lines to the
// runtime's logger. level: 0 trace, 1 debug, 2 info, 3 warn, 4 error.
void SetRendererLogSink(void (*sink)(int level, const char* message));
}  // namespace xn::gfx

namespace xn {
// Factory for the runtime (mirrors CreateStubRenderer in gfx/renderer.h).
// Environment: XN_VK_VALIDATION=1, XN_FRAME_DUMP=<dir> (PPM every
// XN_FRAME_DUMP_EVERY presents, default 60), XN_VK_NO_VSYNC=1.
IRenderer* CreateVulkanRenderer();
IRenderer* CreateVulkanRenderer(gfx::VulkanRendererConfig config);
}  // namespace xn
