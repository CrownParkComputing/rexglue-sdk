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
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

#if REX_HAS_VULKAN
#include <rex/ui/vulkan/device.h>
#include "native/texture_cache.h"
#endif

namespace rex::graphics {
class SpirvShaderTranslator;
class SpirvShader;
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
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSet constants_set = VK_NULL_HANDLE;
    VkDescriptorSet vertex_texture_set = VK_NULL_HANDLE;
    VkDescriptorSet pixel_texture_set = VK_NULL_HANDLE;
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

  // Register-derived graphics pipeline state (Phase 3: real blend / depth / cull
  // instead of the Phase 2 hardcoded blend-off / depth-off / cull-none).
  struct GuestPipelineState {
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // Color blend (render target 0).
    bool blend_enable = false;
    VkBlendFactor src_color_factor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_color_factor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp color_op = VK_BLEND_OP_ADD;
    VkBlendFactor src_alpha_factor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_alpha_factor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp alpha_op = VK_BLEND_OP_ADD;
    VkColorComponentFlags color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // Depth.
    bool depth_test_enable = false;
    bool depth_write_enable = false;
    VkCompareOp depth_compare_op = VK_COMPARE_OP_ALWAYS;
    // Rasterizer.
    VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
    VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    uint64_t Hash() const;
  };

  // Returns the cached VkShaderModule for a translation, creating it on first
  // use. Returns VK_NULL_HANDLE on failure.
  VkShaderModule GetShaderModule(const Shader::Translation* translation);
  // Returns (creating if needed) a graphics pipeline for the given state.
  VkPipeline GetPipeline(VkShaderModule vertex_module, VkShaderModule pixel_module,
                         VkPipelineLayout layout, const GuestPipelineState& state);
  // Fills a GuestPipelineState from the current register file + primitive type.
  GuestPipelineState BuildPipelineState(VkPrimitiveTopology topology, bool primitive_polygonal,
                                        const reg::RB_DEPTHCONTROL& depth_control,
                                        uint32_t pixel_writes_color_targets) const;

  // Texture-path helpers. Textured guest draws are rendered with a dummy white
  // texture bound to every image/sampler binding the shader declares, so the
  // geometry shape appears (flat/vertex-coloured) instead of being skipped.
  bool CreateDummyTextures();
  VkImageView DummyViewForDimension(xenos::FetchOpDimension dimension) const;
  // Texture descriptor set layout for a given image + sampler binding count.
  VkDescriptorSetLayout GetTextureSetLayout(uint32_t texture_count, uint32_t sampler_count);
  // Guest pipeline layout for the four per-stage texture/sampler counts.
  VkPipelineLayout GetGuestPipelineLayout(uint32_t vertex_texture_count,
                                          uint32_t vertex_sampler_count,
                                          uint32_t pixel_texture_count,
                                          uint32_t pixel_sampler_count);
  // Allocates and fills a per-draw texture descriptor set with dummy textures.
  VkDescriptorSet AllocateDummyTextureSet(const SpirvShader* shader, VkDescriptorSetLayout layout);
  // Allocates and fills a per-draw texture descriptor set with REAL guest
  // textures + samplers from the texture cache (Phase 3). Falls back to the dummy
  // white texture / default sampler for any binding with no valid texture.
  VkDescriptorSet AllocateTextureSet(SpirvShader* shader, VkDescriptorSetLayout layout);

  // Guest-shader descriptor layout (matches SpirvShaderTranslator):
  //   set 0 = shared memory storage buffer(s); set 1 = 5 constant UBOs;
  //   set 2 = vertex textures; set 3 = pixel textures.
  VkDescriptorSetLayout descriptor_set_layout_shared_memory_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout texture_set_layout_empty_ = VK_NULL_HANDLE;

  VkDescriptorPool shared_memory_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet shared_memory_descriptor_set_ = VK_NULL_HANDLE;
  uint32_t shared_memory_binding_count_ = 1;

  // Dummy white textures (one per Xenos image dimension) + a default sampler.
  VkImage dummy_image_2d_array_ = VK_NULL_HANDLE;   // covers 1D/2D (Dim2D arrayed)
  VkImageView dummy_view_2d_array_ = VK_NULL_HANDLE;
  VkImage dummy_image_3d_ = VK_NULL_HANDLE;
  VkImageView dummy_view_3d_ = VK_NULL_HANDLE;
  VkImage dummy_image_cube_ = VK_NULL_HANDLE;
  VkImageView dummy_view_cube_ = VK_NULL_HANDLE;
  VkDeviceMemory dummy_memory_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkSampler dummy_sampler_ = VK_NULL_HANDLE;
  VkDescriptorSet empty_texture_set_ = VK_NULL_HANDLE;

  // Cached texture set layouts (key: texture_count<<16 | sampler_count) and
  // guest pipeline layouts (key: packed four per-stage counts).
  std::unordered_map<uint32_t, VkDescriptorSetLayout> texture_set_layouts_;
  std::unordered_map<uint64_t, VkPipelineLayout> pipeline_layouts_;

  // Per-frame constant + texture descriptor sets, reset each frame.
  VkDescriptorPool constants_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorPool texture_descriptor_pool_ = VK_NULL_HANDLE;
  static constexpr uint32_t kMaxDrawsPerFrame = 8192;

  std::unique_ptr<NativeSharedMemory> shared_memory_;
  std::unique_ptr<NativeTextureCache> texture_cache_;
  std::unique_ptr<SpirvShaderTranslator> shader_translator_;

  // ----- Phase 3 depth buffer (guest-output sized, transient, cleared each
  // frame). Added so 3D scenes occlude correctly (no EDRAM emulation). -----
  static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
  bool EnsureDepthResources(uint32_t width, uint32_t height);
  void DestroyDepthResources();
  VkImage depth_image_ = VK_NULL_HANDLE;
  VkImageView depth_view_ = VK_NULL_HANDLE;
  VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
  uint32_t depth_width_ = 0;
  uint32_t depth_height_ = 0;

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
