/**
 * @file        graphics/native/command_processor.h
 * @brief       Native GPU renderer - PM4 -> native GPU command translation
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 *
 * @remarks     NativeCommandProcessor derives from the shared, game-agnostic
 *              rex::graphics::CommandProcessor. The base class does ALL PM4
 *              ring-buffer parsing and hands us fully-decoded work through the
 *              virtual seams (SetupContext, LoadShader, IssueDraw, IssueCopy,
 *              IssueSwap).
 *
 *              PHASE 2 STATUS: LoadShader translates Xenos microcode -> SPIR-V
 *              (reusing SpirvShaderTranslator); IssueDraw builds a real Vulkan
 *              graphics pipeline from register-derived state, binds shared
 *              memory + constant buffers (the SPIR-V does in-shader vertex fetch
 *              from shared memory - no classic vertex input), records viewport /
 *              index / draw state into a per-frame deferred draw list; IssueSwap
 *              replays those draws into the presenter's guest-output image and
 *              presents it. Textured draws are skipped for now (Phase 3).
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>

#if REX_HAS_VULKAN
#include <rex/ui/vulkan/device.h>
#endif

namespace rex::graphics {
class SpirvShaderTranslator;
}  // namespace rex::graphics

namespace rex::graphics::native {

class NativeGraphicsSystem;
class NativeSharedMemory;

class NativeCommandProcessor : public CommandProcessor {
 public:
  NativeCommandProcessor(NativeGraphicsSystem* graphics_system,
                         system::KernelState* kernel_state);
  ~NativeCommandProcessor() override;

  // Trace / save-state seams (no-ops until the native backend owns memory).
  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;
  void RestoreEdramSnapshot(const void* snapshot) override;

  // Present seam. Native renderer will hand a host image to the presenter here.
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;

  bool IssueCopy() override;

 private:
  // Shader cache keyed by microcode hash. Stores SpirvShader instances (parsed
  // microcode + translated SPIR-V translations).
  std::vector<std::unique_ptr<Shader>> shader_storage_;
  std::unordered_map<uint64_t, Shader*> shader_map_;

  uint64_t draw_count_ = 0;
  uint64_t copy_count_ = 0;
  uint64_t swap_count_ = 0;
  uint64_t deferred_draw_total_ = 0;
  uint64_t skipped_draw_total_ = 0;

#if REX_HAS_VULKAN
  // ----- Phase 1 clear/present state (reused by Phase 2 as the guest-draw
  // render pass and replay submission) -----
  bool CreateClearResources();
  void DestroyClearResources();
  bool EnsureClearFramebuffer(VkImageView image_view, uint64_t image_version, uint32_t width,
                              uint32_t height);

  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;
  VkRenderPass clear_render_pass_ = VK_NULL_HANDLE;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkFence clear_fence_ = VK_NULL_HANDLE;

  VkFramebuffer clear_framebuffer_ = VK_NULL_HANDLE;
  VkImageView clear_framebuffer_view_ = VK_NULL_HANDLE;
  uint64_t clear_framebuffer_version_ = UINT64_MAX;
  uint32_t clear_framebuffer_width_ = 0;
  uint32_t clear_framebuffer_height_ = 0;

  uint32_t last_logged_clear_raw_ = 0xFFFFFFFFu;

  // ----- Phase 2 draw state -----
  // A per-frame growing host-visible buffer with a bump allocator, reset each
  // frame. Used for constant UBOs and converted index buffers.
  struct HostRingBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapping = nullptr;
    VkDeviceSize size = 0;
    VkDeviceSize cursor = 0;
  };

  // One deferred draw captured during IssueDraw, replayed at IssueSwap.
  struct DeferredDraw {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet constants_set = VK_NULL_HANDLE;
    VkViewport viewport = {};
    VkRect2D scissor = {};
    bool indexed = false;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    uint32_t draw_count = 0;
  };

  bool CreateDrawResources();
  void DestroyDrawResources();
  bool CreateHostRingBuffer(VkBufferUsageFlags usage, VkDeviceSize size, HostRingBuffer& out);
  void DestroyHostRingBuffer(HostRingBuffer& ring);
  // Sub-allocates from a host ring buffer. Returns nullptr on overflow.
  uint8_t* RingAllocate(HostRingBuffer& ring, VkDeviceSize bytes, VkDeviceSize alignment,
                        VkBuffer& buffer_out, VkDeviceSize& offset_out);
  void BeginFrameIfNeeded();

  // Returns the cached VkShaderModule for a translation, creating it on first
  // use. Returns VK_NULL_HANDLE on failure.
  VkShaderModule GetShaderModule(const Shader::Translation* translation);
  // Returns (creating if needed) a graphics pipeline for the given state.
  VkPipeline GetPipeline(VkShaderModule vertex_module, VkShaderModule pixel_module,
                         VkPrimitiveTopology topology);

  // Guest-shader descriptor layout (matches SpirvShaderTranslator):
  //   set 0 = shared memory storage buffer(s); set 1 = 5 constant UBOs.
  VkDescriptorSetLayout descriptor_set_layout_shared_memory_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  VkPipelineLayout guest_pipeline_layout_ = VK_NULL_HANDLE;

  VkDescriptorPool shared_memory_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_descriptor_set_ = VK_NULL_HANDLE;
  uint32_t shared_memory_binding_count_ = 1;

  // Per-frame constant descriptor sets, reset each frame.
  VkDescriptorPool constants_descriptor_pool_ = VK_NULL_HANDLE;
  static constexpr uint32_t kMaxDrawsPerFrame = 4096;

  std::unique_ptr<NativeSharedMemory> shared_memory_;
  std::unique_ptr<SpirvShaderTranslator> shader_translator_;

  HostRingBuffer uniform_ring_;
  HostRingBuffer index_ring_;

  std::unordered_map<const Shader::Translation*, VkShaderModule> shader_modules_;
  std::unordered_map<uint64_t, VkPipeline> pipelines_;

  std::vector<DeferredDraw> deferred_draws_;
  bool frame_open_ = false;
  bool draw_resources_ok_ = false;
#endif  // REX_HAS_VULKAN
};

}  // namespace rex::graphics::native
