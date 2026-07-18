/**
 * @file        graphics/native/command_processor.cpp
 * @brief       Native GPU renderer - PM4 -> native GPU command translation
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 */

#include "native/command_processor.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>
#include <rex/system/kernel_state.h>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>

#include "native/graphics_system.h"
#include "native/native_shared_memory.h"

#if REX_HAS_VULKAN
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#endif

REXCVAR_DEFINE_BOOL(native_log_draws, false, "GPU/Native",
                    "Log every draw/copy the native renderer receives from the PM4 stream");

namespace rex::graphics::native {

namespace {

uint64_t HashUcode(const uint32_t* dwords, uint32_t count) {
  uint64_t hash = 1469598103934665603ull;
  for (uint32_t i = 0; i < count; ++i) {
    hash ^= dwords[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

#if REX_HAS_VULKAN
// Xenos primitive type -> Vulkan topology. Returns false for types not yet
// supported by the native backend (rect/quad list, line loop, polygon - these
// need geometry-shader-style expansion, a Phase 3 concern).
bool MapPrimitiveTopology(xenos::PrimitiveType prim_type, VkPrimitiveTopology& out) {
  switch (prim_type) {
    case xenos::PrimitiveType::kPointList:
      out = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      return true;
    case xenos::PrimitiveType::kLineList:
      out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      return true;
    case xenos::PrimitiveType::kLineStrip:
      out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      return true;
    case xenos::PrimitiveType::kTriangleList:
      out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      return true;
    case xenos::PrimitiveType::kTriangleStrip:
      out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      return true;
    case xenos::PrimitiveType::kTriangleFan:
      out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      return true;
    default:
      return false;
  }
}

inline bool IndexEndianSwaps(xenos::Endian e) {
  return e == xenos::Endian::k8in16 || e == xenos::Endian::k8in32 || e == xenos::Endian::k16in32;
}

uint16_t ConvertIndex16(const uint8_t* p, xenos::Endian e) {
  uint16_t v = uint16_t(p[0]) | (uint16_t(p[1]) << 8);
  if (IndexEndianSwaps(e)) {
    v = uint16_t((v >> 8) | (v << 8));
  }
  return v;
}

uint32_t ConvertIndex32(const uint8_t* p, xenos::Endian e) {
  uint32_t v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
               (uint32_t(p[3]) << 24);
  switch (e) {
    case xenos::Endian::k8in16:
      return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    case xenos::Endian::k8in32:
      return __builtin_bswap32(v);
    case xenos::Endian::k16in32:
      return (v >> 16) | (v << 16);
    default:
      return v;
  }
}
#endif  // REX_HAS_VULKAN

}  // namespace

NativeCommandProcessor::NativeCommandProcessor(NativeGraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

NativeCommandProcessor::~NativeCommandProcessor() = default;

bool NativeCommandProcessor::SetupContext() {
#if REX_HAS_VULKAN
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(graphics_system_->provider());
  if (!provider) {
    REXLOG_ERROR("rexgpu-native: SetupContext - no Vulkan provider");
    return false;
  }
  vulkan_device_ = provider->vulkan_device();
  if (!vulkan_device_) {
    REXLOG_ERROR("rexgpu-native: SetupContext - provider has no Vulkan device");
    return false;
  }
  if (!CreateClearResources()) {
    REXLOG_ERROR("rexgpu-native: SetupContext - failed to create clear-present resources");
    DestroyClearResources();
    vulkan_device_ = nullptr;
    return false;
  }
  // Phase 2: shared memory, shader translator, descriptor layouts, pools. If any
  // of this fails, keep the Phase 1 clear-present path working (draw_resources_ok_
  // stays false and every draw is skipped, so the frame is at least a clean clear).
  draw_resources_ok_ = CreateDrawResources();
  if (!draw_resources_ok_) {
    REXLOG_WARN("rexgpu-native: SetupContext - draw resources unavailable, clear-only fallback");
    DestroyDrawResources();
  }
  REXLOG_INFO("rexgpu-native: SetupContext ready (device={}, draw_path={})",
              vulkan_device_->properties().deviceName, draw_resources_ok_ ? "geometry" : "clear-only");
  return true;
#else
  REXLOG_ERROR("rexgpu-native: SetupContext - built without Vulkan support");
  return false;
#endif
}

void NativeCommandProcessor::ShutdownContext() {
  REXLOG_INFO("rexgpu-native: ShutdownContext (draws={} deferred={} skipped={} copies={} swaps={})",
              draw_count_, deferred_draw_total_, skipped_draw_total_, copy_count_, swap_count_);
#if REX_HAS_VULKAN
  DestroyDrawResources();
  DestroyClearResources();
  vulkan_device_ = nullptr;
#endif
  shader_map_.clear();
  shader_storage_.clear();
}

#if REX_HAS_VULKAN

bool NativeCommandProcessor::CreateClearResources() {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkCommandPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags =
      VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to create command pool");
    return false;
  }

  VkCommandBufferAllocateInfo cb_info = {};
  cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cb_info.commandPool = command_pool_;
  cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device, &cb_info, &command_buffer_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to allocate command buffer");
    return false;
  }

  VkFenceCreateInfo fence_info = {};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (dfn.vkCreateFence(device, &fence_info, nullptr, &clear_fence_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to create fence");
    return false;
  }

  // Render pass with a single guest-output-format color attachment. Used both to
  // clear the guest output and (Phase 2) as the render pass for guest draw
  // pipelines and the swap-time replay.
  VkAttachmentDescription attachment = {};
  attachment.format = ui::vulkan::VulkanPresenter::kGuestOutputFormat;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference color_ref = {};
  color_ref.attachment = 0;
  color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;

  VkRenderPassCreateInfo rp_info = {};
  rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rp_info.attachmentCount = 1;
  rp_info.pAttachments = &attachment;
  rp_info.subpassCount = 1;
  rp_info.pSubpasses = &subpass;
  if (dfn.vkCreateRenderPass(device, &rp_info, nullptr, &clear_render_pass_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to create clear render pass");
    return false;
  }

  return true;
}

void NativeCommandProcessor::DestroyClearResources() {
  if (!vulkan_device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  if (clear_framebuffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFramebuffer(device, clear_framebuffer_, nullptr);
    clear_framebuffer_ = VK_NULL_HANDLE;
  }
  if (clear_render_pass_ != VK_NULL_HANDLE) {
    dfn.vkDestroyRenderPass(device, clear_render_pass_, nullptr);
    clear_render_pass_ = VK_NULL_HANDLE;
  }
  if (clear_fence_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFence(device, clear_fence_, nullptr);
    clear_fence_ = VK_NULL_HANDLE;
  }
  if (command_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyCommandPool(device, command_pool_, nullptr);
    command_pool_ = VK_NULL_HANDLE;
    command_buffer_ = VK_NULL_HANDLE;
  }
  clear_framebuffer_view_ = VK_NULL_HANDLE;
  clear_framebuffer_version_ = UINT64_MAX;
  clear_framebuffer_width_ = 0;
  clear_framebuffer_height_ = 0;
}

bool NativeCommandProcessor::EnsureClearFramebuffer(VkImageView image_view, uint64_t image_version,
                                                    uint32_t width, uint32_t height) {
  if (clear_framebuffer_ != VK_NULL_HANDLE && clear_framebuffer_view_ == image_view &&
      clear_framebuffer_version_ == image_version && clear_framebuffer_width_ == width &&
      clear_framebuffer_height_ == height) {
    return true;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  if (clear_framebuffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFramebuffer(device, clear_framebuffer_, nullptr);
    clear_framebuffer_ = VK_NULL_HANDLE;
  }

  VkFramebufferCreateInfo fb_info = {};
  fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fb_info.renderPass = clear_render_pass_;
  fb_info.attachmentCount = 1;
  fb_info.pAttachments = &image_view;
  fb_info.width = width;
  fb_info.height = height;
  fb_info.layers = 1;
  if (dfn.vkCreateFramebuffer(device, &fb_info, nullptr, &clear_framebuffer_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: failed to create clear framebuffer {}x{}", width, height);
    return false;
  }
  clear_framebuffer_view_ = image_view;
  clear_framebuffer_version_ = image_version;
  clear_framebuffer_width_ = width;
  clear_framebuffer_height_ = height;
  return true;
}

// ---------------------------------------------------------------------------
// Phase 2 draw resources
// ---------------------------------------------------------------------------

bool NativeCommandProcessor::CreateHostRingBuffer(VkBufferUsageFlags usage, VkDeviceSize size,
                                                  HostRingBuffer& out) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkBufferCreateInfo buffer_info = {};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dfn.vkCreateBuffer(device, &buffer_info, nullptr, &out.buffer) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req;
  dfn.vkGetBufferMemoryRequirements(device, out.buffer, &req);
  uint32_t type_index;
  uint32_t candidates =
      req.memoryTypeBits & vulkan_device_->memory_types().host_visible &
      vulkan_device_->memory_types().host_coherent;
  if (!rex::bit_scan_forward(candidates, &type_index)) {
    return false;
  }
  VkMemoryAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type_index;
  if (dfn.vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS) {
    return false;
  }
  if (dfn.vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS) {
    return false;
  }
  void* mapping = nullptr;
  if (dfn.vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &mapping) != VK_SUCCESS) {
    return false;
  }
  out.mapping = static_cast<uint8_t*>(mapping);
  out.size = size;
  out.cursor = 0;
  return true;
}

void NativeCommandProcessor::DestroyHostRingBuffer(HostRingBuffer& ring) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (ring.buffer != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, ring.buffer, nullptr);
    ring.buffer = VK_NULL_HANDLE;
  }
  if (ring.memory != VK_NULL_HANDLE) {
    if (ring.mapping) {
      dfn.vkUnmapMemory(device, ring.memory);
      ring.mapping = nullptr;
    }
    dfn.vkFreeMemory(device, ring.memory, nullptr);
    ring.memory = VK_NULL_HANDLE;
  }
  ring.size = 0;
  ring.cursor = 0;
}

uint8_t* NativeCommandProcessor::RingAllocate(HostRingBuffer& ring, VkDeviceSize bytes,
                                              VkDeviceSize alignment, VkBuffer& buffer_out,
                                              VkDeviceSize& offset_out) {
  VkDeviceSize aligned = (ring.cursor + (alignment - 1)) & ~(alignment - 1);
  if (aligned + bytes > ring.size) {
    return nullptr;
  }
  buffer_out = ring.buffer;
  offset_out = aligned;
  ring.cursor = aligned + bytes;
  return ring.mapping + aligned;
}

bool NativeCommandProcessor::CreateDrawResources() {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  if (!memory_) {
    REXLOG_ERROR("rexgpu-native: draw resources - no guest memory");
    return false;
  }

  shared_memory_ = std::make_unique<NativeSharedMemory>(vulkan_device_, *memory_);
  if (!shared_memory_->Initialize()) {
    REXLOG_ERROR("rexgpu-native: draw resources - shared memory init failed");
    return false;
  }

  shader_translator_ = std::make_unique<SpirvShaderTranslator>(
      SpirvShaderTranslator::Features(vulkan_device_),
      /*native_2x_msaa_with_attachments=*/true, /*native_2x_msaa_no_attachments=*/false,
      /*edram_fragment_shader_interlock=*/false);

  shared_memory_binding_count_ =
      1u << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
               vulkan_device_->properties().maxStorageBufferRange);

  const VkShaderStageFlags guest_stages =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  // Set 0: shared memory storage buffer (array of shared_memory_binding_count_).
  VkDescriptorSetLayoutBinding shared_binding = {};
  shared_binding.binding = 0;
  shared_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_binding.descriptorCount = shared_memory_binding_count_;
  shared_binding.stageFlags = guest_stages;
  VkDescriptorSetLayoutCreateInfo shared_layout_info = {};
  shared_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_layout_info.bindingCount = 1;
  shared_layout_info.pBindings = &shared_binding;
  if (dfn.vkCreateDescriptorSetLayout(device, &shared_layout_info, nullptr,
                                      &descriptor_set_layout_shared_memory_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - shared memory set layout failed");
    return false;
  }

  // Set 1: 5 constant UBOs (system, float vertex, float pixel, bool/loop, fetch).
  VkDescriptorSetLayoutBinding constant_bindings[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    constant_bindings[i].binding = i;
    constant_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constant_bindings[i].descriptorCount = 1;
    constant_bindings[i].stageFlags = guest_stages;
  }
  VkDescriptorSetLayoutCreateInfo constants_layout_info = {};
  constants_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  constants_layout_info.bindingCount = SpirvShaderTranslator::kConstantBufferCount;
  constants_layout_info.pBindings = constant_bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &constants_layout_info, nullptr,
                                      &descriptor_set_layout_constants_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - constants set layout failed");
    return false;
  }

  // Pipeline layout: set 0 + set 1 (textures sets 2/3 omitted - textured draws
  // are skipped in Phase 2).
  VkDescriptorSetLayout set_layouts[2] = {descriptor_set_layout_shared_memory_,
                                          descriptor_set_layout_constants_};
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 2;
  pipeline_layout_info.pSetLayouts = set_layouts;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &guest_pipeline_layout_) !=
      VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - pipeline layout failed");
    return false;
  }

  // Static shared-memory descriptor set (never changes; points at the buffer).
  VkDescriptorPoolSize shared_pool_size = {};
  shared_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_pool_size.descriptorCount = shared_memory_binding_count_;
  VkDescriptorPoolCreateInfo shared_pool_info = {};
  shared_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  shared_pool_info.maxSets = 1;
  shared_pool_info.poolSizeCount = 1;
  shared_pool_info.pPoolSizes = &shared_pool_size;
  if (dfn.vkCreateDescriptorPool(device, &shared_pool_info, nullptr,
                                 &shared_memory_descriptor_pool_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - shared memory pool failed");
    return false;
  }
  VkDescriptorSetAllocateInfo shared_alloc = {};
  shared_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  shared_alloc.descriptorPool = shared_memory_descriptor_pool_;
  shared_alloc.descriptorSetCount = 1;
  shared_alloc.pSetLayouts = &descriptor_set_layout_shared_memory_;
  if (dfn.vkAllocateDescriptorSets(device, &shared_alloc, &shared_memory_descriptor_set_) !=
      VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - shared memory set alloc failed");
    return false;
  }
  std::array<VkDescriptorBufferInfo, 4> shared_buffer_infos = {};
  const VkDeviceSize shared_range = SharedMemory::kBufferSize / shared_memory_binding_count_;
  for (uint32_t i = 0; i < shared_memory_binding_count_; ++i) {
    shared_buffer_infos[i].buffer = shared_memory_->buffer();
    shared_buffer_infos[i].offset = shared_range * i;
    shared_buffer_infos[i].range = shared_range;
  }
  VkWriteDescriptorSet shared_write = {};
  shared_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  shared_write.dstSet = shared_memory_descriptor_set_;
  shared_write.dstBinding = 0;
  shared_write.dstArrayElement = 0;
  shared_write.descriptorCount = shared_memory_binding_count_;
  shared_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_write.pBufferInfo = shared_buffer_infos.data();
  dfn.vkUpdateDescriptorSets(device, 1, &shared_write, 0, nullptr);

  // Per-frame constant descriptor set pool.
  VkDescriptorPoolSize constant_pool_size = {};
  constant_pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  constant_pool_size.descriptorCount =
      kMaxDrawsPerFrame * SpirvShaderTranslator::kConstantBufferCount;
  VkDescriptorPoolCreateInfo constant_pool_info = {};
  constant_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  constant_pool_info.maxSets = kMaxDrawsPerFrame;
  constant_pool_info.poolSizeCount = 1;
  constant_pool_info.pPoolSizes = &constant_pool_size;
  if (dfn.vkCreateDescriptorPool(device, &constant_pool_info, nullptr,
                                 &constants_descriptor_pool_) != VK_SUCCESS) {
    REXLOG_ERROR("rexgpu-native: draw resources - constants pool failed");
    return false;
  }

  // Per-frame host-visible upload rings (constants + converted indices).
  if (!CreateHostRingBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 32u << 20, uniform_ring_)) {
    REXLOG_ERROR("rexgpu-native: draw resources - uniform ring failed");
    return false;
  }
  if (!CreateHostRingBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 32u << 20, index_ring_)) {
    REXLOG_ERROR("rexgpu-native: draw resources - index ring failed");
    return false;
  }

  return true;
}

void NativeCommandProcessor::DestroyDrawResources() {
  if (!vulkan_device_) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  for (auto& kv : pipelines_) {
    if (kv.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, kv.second, nullptr);
    }
  }
  pipelines_.clear();
  for (auto& kv : shader_modules_) {
    if (kv.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, kv.second, nullptr);
    }
  }
  shader_modules_.clear();

  DestroyHostRingBuffer(uniform_ring_);
  DestroyHostRingBuffer(index_ring_);

  if (constants_descriptor_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorPool(device, constants_descriptor_pool_, nullptr);
    constants_descriptor_pool_ = VK_NULL_HANDLE;
  }
  if (shared_memory_descriptor_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorPool(device, shared_memory_descriptor_pool_, nullptr);
    shared_memory_descriptor_pool_ = VK_NULL_HANDLE;
    shared_memory_descriptor_set_ = VK_NULL_HANDLE;
  }
  if (guest_pipeline_layout_ != VK_NULL_HANDLE) {
    dfn.vkDestroyPipelineLayout(device, guest_pipeline_layout_, nullptr);
    guest_pipeline_layout_ = VK_NULL_HANDLE;
  }
  if (descriptor_set_layout_constants_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorSetLayout(device, descriptor_set_layout_constants_, nullptr);
    descriptor_set_layout_constants_ = VK_NULL_HANDLE;
  }
  if (descriptor_set_layout_shared_memory_ != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorSetLayout(device, descriptor_set_layout_shared_memory_, nullptr);
    descriptor_set_layout_shared_memory_ = VK_NULL_HANDLE;
  }

  shader_translator_.reset();
  if (shared_memory_) {
    shared_memory_->Shutdown();
    shared_memory_.reset();
  }
  deferred_draws_.clear();
  frame_open_ = false;
  draw_resources_ok_ = false;
}

void NativeCommandProcessor::BeginFrameIfNeeded() {
  if (frame_open_) {
    return;
  }
  // Safe to recycle: the previous frame's replay submission was fenced-and-waited
  // in IssueSwap, so nothing references last frame's sets / ring data.
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  dfn.vkResetDescriptorPool(device, constants_descriptor_pool_, 0);
  uniform_ring_.cursor = 0;
  index_ring_.cursor = 0;
  deferred_draws_.clear();
  frame_open_ = true;
}

VkShaderModule NativeCommandProcessor::GetShaderModule(const Shader::Translation* translation) {
  auto it = shader_modules_.find(translation);
  if (it != shader_modules_.end()) {
    return it->second;
  }
  const std::vector<uint8_t>& spirv = translation->translated_binary();
  VkShaderModule module = VK_NULL_HANDLE;
  if (!spirv.empty() && (spirv.size() % sizeof(uint32_t)) == 0) {
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size();
    info.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
    if (dfn.vkCreateShaderModule(vulkan_device_->device(), &info, nullptr, &module) != VK_SUCCESS) {
      module = VK_NULL_HANDLE;
    }
  }
  shader_modules_.emplace(translation, module);
  return module;
}

VkPipeline NativeCommandProcessor::GetPipeline(VkShaderModule vertex_module,
                                               VkShaderModule pixel_module,
                                               VkPrimitiveTopology topology) {
  uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(vertex_module));
  key = key * 1099511628211ull ^ uint64_t(reinterpret_cast<uintptr_t>(pixel_module));
  key = key * 1099511628211ull ^ uint64_t(topology);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second;
  }

  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertex_module;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = pixel_module;
  stages[1].pName = "main";

  // Empty vertex input - the translated shaders fetch vertices from shared
  // memory in-shader (vfetch), so there are no classic vertex bindings.
  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = topology;
  input_assembly.primitiveRestartEnable = VK_FALSE;

  VkPipelineViewportStateCreateInfo viewport_state = {};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster = {};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
  depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_stencil.depthTestEnable = VK_FALSE;
  depth_stencil.depthWriteEnable = VK_FALSE;
  depth_stencil.stencilTestEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState blend_attachment = {};
  blend_attachment.blendEnable = VK_FALSE;
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo color_blend = {};
  color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend.attachmentCount = 1;
  color_blend.pAttachments = &blend_attachment;

  VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  VkGraphicsPipelineCreateInfo pipeline_info = {};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pDepthStencilState = &depth_stencil;
  pipeline_info.pColorBlendState = &color_blend;
  pipeline_info.pDynamicState = &dynamic;
  pipeline_info.layout = guest_pipeline_layout_;
  pipeline_info.renderPass = clear_render_pass_;
  pipeline_info.subpass = 0;

  VkPipeline pipeline = VK_NULL_HANDLE;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) !=
      VK_SUCCESS) {
    pipeline = VK_NULL_HANDLE;
  }
  pipelines_.emplace(key, pipeline);
  return pipeline;
}

#endif  // REX_HAS_VULKAN

Shader* NativeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  const uint64_t hash = HashUcode(host_address, dword_count);
  auto it = shader_map_.find(hash);
  if (it != shader_map_.end()) {
    return it->second;
  }

  // A SpirvShader keeps the parsed microcode and its SPIR-V translations.
  auto shader = std::make_unique<SpirvShader>(shader_type, hash, host_address, dword_count);
  Shader* ptr = shader.get();
  shader_storage_.push_back(std::move(shader));
  shader_map_.emplace(hash, ptr);

  REXLOG_TRACE("rexgpu-native: LoadShader type={} guest=0x{:08X} dwords={} hash=0x{:016X}",
               shader_type == xenos::ShaderType::kVertex ? "VS" : "PS", guest_address, dword_count,
               hash);
  return ptr;
}

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  (void)major_mode_explicit;
  ++draw_count_;

  if (REXCVAR_GET(native_log_draws)) {
    REXLOG_INFO("rexgpu-native: IssueDraw #{} prim={} indices={} indexed={} vs={} ps={}",
                draw_count_, static_cast<uint32_t>(prim_type), index_count,
                index_buffer_info != nullptr, static_cast<void*>(active_vertex_shader()),
                static_cast<void*>(active_pixel_shader()));
  }

#if REX_HAS_VULKAN
  if (!draw_resources_ok_) {
    return true;
  }
  const RegisterFile& regs = *register_file_;

  auto skip = [&](const char* reason) {
    ++skipped_draw_total_;
    static uint32_t skip_log_budget = 64;
    if (skip_log_budget) {
      --skip_log_budget;
      REXLOG_TRACE("rexgpu-native: skip draw #{} ({}) prim={}", draw_count_, reason,
                   uint32_t(prim_type));
    }
    return true;  // Draw is consumed; just not rendered natively yet.
  };

  const xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
  if (edram_mode != xenos::EdramMode::kColorDepth) {
    return skip("edram_mode");
  }

  VkPrimitiveTopology topology;
  if (!MapPrimitiveTopology(prim_type, topology)) {
    return skip("primitive_type");
  }

  auto* vertex_shader = static_cast<SpirvShader*>(active_vertex_shader());
  auto* pixel_shader = static_cast<SpirvShader*>(active_pixel_shader());
  if (!vertex_shader || !pixel_shader) {
    return skip("missing_shader");
  }

  rex::string::StringBuffer ucode_buffer;
  vertex_shader->AnalyzeUcode(ucode_buffer);
  pixel_shader->AnalyzeUcode(ucode_buffer);

  if (!vertex_shader->texture_bindings().empty() || !pixel_shader->texture_bindings().empty()) {
    return skip("textured");  // Phase 3.
  }
  if (vertex_shader->memexport_eM_written() || pixel_shader->memexport_eM_written()) {
    return skip("memexport");
  }

  const bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal)) {
    return skip("no_rasterization");
  }
  if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
    return skip("pixel_shader_not_needed");
  }

  const reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  uint32_t ps_param_gen_pos = UINT32_MAX;
  const uint32_t interpolator_mask =
      vertex_shader->writes_interpolators() &
      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                             regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos);

  // --- Shader modifications (host-render-target path; no tessellation, UCP or
  // memexport in this milestone). ---
  const auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  SpirvShaderTranslator::Modification vmod(shader_translator_->GetDefaultVertexShaderModification(
      vertex_shader->GetDynamicAddressableRegisterCount(sq_program_cntl.vs_num_reg),
      Shader::HostVertexShaderType::kVertex));
  vmod.vertex.interpolator_mask = interpolator_mask;
  vmod.vertex.output_point_parameters =
      uint32_t((vertex_shader->writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               prim_type == xenos::PrimitiveType::kPointList);
  vmod.vertex.user_clip_plane_count = 0;
  vmod.vertex.user_clip_plane_cull = 0;
  vmod.vertex.vertex_kill_and = 0;

  SpirvShaderTranslator::Modification pmod(shader_translator_->GetDefaultPixelShaderModification(
      pixel_shader->GetDynamicAddressableRegisterCount(sq_program_cntl.ps_num_reg)));
  pmod.pixel.interpolator_mask = interpolator_mask;
  pmod.pixel.interpolators_centroid = 0;
  if (ps_param_gen_pos < xenos::kMaxInterpolators) {
    pmod.pixel.param_gen_enable = 1;
    pmod.pixel.param_gen_interpolator = ps_param_gen_pos;
    pmod.pixel.param_gen_point =
        uint32_t(prim_type == xenos::PrimitiveType::kPointList);
  }
  pmod.pixel.depth_stencil_mode = SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers;

  // --- Translate. ---
  Shader::Translation* vtrans = vertex_shader->GetOrCreateTranslation(vmod.value);
  Shader::Translation* ptrans = pixel_shader->GetOrCreateTranslation(pmod.value);
  if (!vtrans->is_translated()) {
    if (!shader_translator_->TranslateAnalyzedShader(*vtrans)) {
      return skip("vs_translate_failed");
    }
  }
  if (!ptrans->is_translated()) {
    if (!shader_translator_->TranslateAnalyzedShader(*ptrans)) {
      return skip("ps_translate_failed");
    }
  }
  if (!vtrans->is_valid() || !ptrans->is_valid()) {
    return skip("translation_invalid");
  }

  VkShaderModule vs_module = GetShaderModule(vtrans);
  VkShaderModule ps_module = GetShaderModule(ptrans);
  if (vs_module == VK_NULL_HANDLE || ps_module == VK_NULL_HANDLE) {
    return skip("shader_module");
  }
  VkPipeline pipeline = GetPipeline(vs_module, ps_module, topology);
  if (pipeline == VK_NULL_HANDLE) {
    return skip("pipeline");
  }

  BeginFrameIfNeeded();
  if (deferred_draws_.size() >= kMaxDrawsPerFrame) {
    return skip("draw_budget");
  }

  // --- Viewport / NDC. ---
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(regs, 1, 1, false,
                                 vulkan_device_->properties().maxViewportDimensions[0],
                                 vulkan_device_->properties().maxViewportDimensions[1], true,
                                 normalized_depth_control, false, false,
                                 pixel_shader->writes_depth(), viewport_info);

  // --- System constants. ---
  SpirvShaderTranslator::SystemConstants system_constants;
  std::memset(&system_constants, 0, sizeof(system_constants));
  uint32_t flags = 0;
  const auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  if (pa_cl_vte_cntl.vtx_xy_fmt) flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  if (pa_cl_vte_cntl.vtx_z_fmt) flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  if (pa_cl_vte_cntl.vtx_w0_fmt) flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  if (primitive_polygonal) flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(regs)) flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  const auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  const xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  system_constants.flags = flags;
  system_constants.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  system_constants.vertex_index_min = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);
  system_constants.vertex_index_max = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  system_constants.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  for (uint32_t i = 0; i < 3; ++i) {
    system_constants.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants.ndc_offset[i] = viewport_info.ndc_offset[i];
  }
  for (uint32_t i = 0; i < 4; ++i) {
    system_constants.color_exp_bias[i] = 1.0f;
  }

  // --- Upload constant UBOs and build the constants descriptor set. ---
  const VkDeviceSize ubo_align =
      VkDeviceSize(vulkan_device_->properties().minUniformBufferOffsetAlignment);
  std::array<VkDescriptorBufferInfo, SpirvShaderTranslator::kConstantBufferCount> ubo_infos = {};
  auto upload_ubo = [&](uint32_t binding, const void* src, size_t size) -> bool {
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* mapping = RingAllocate(uniform_ring_, size, ubo_align, buffer, offset);
    if (!mapping) {
      return false;
    }
    std::memcpy(mapping, src, size);
    ubo_infos[binding].buffer = buffer;
    ubo_infos[binding].offset = offset;
    ubo_infos[binding].range = size;
    return true;
  };

  if (!upload_ubo(SpirvShaderTranslator::kConstantBufferSystem, &system_constants,
                  sizeof(system_constants))) {
    return skip("ubo_overflow_system");
  }

  // Vertex float constants (tightly packed per the shader's bitmap; base 000).
  const Shader::ConstantRegisterMap& vmap = vertex_shader->constant_register_map();
  {
    size_t float_size = sizeof(float) * 4 * std::max(vmap.float_count, UINT32_C(1));
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* mapping = RingAllocate(uniform_ring_, float_size, ubo_align, buffer, offset);
    if (!mapping) {
      return skip("ubo_overflow_vfloat");
    }
    uint8_t* write = mapping;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = vmap.float_bitmap[i];
      uint32_t index;
      while (rex::bit_scan_forward(bits, &index)) {
        bits &= ~(1ull << index);
        std::memcpy(write, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (index << 2)],
                    sizeof(float) * 4);
        write += sizeof(float) * 4;
      }
    }
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatVertex].buffer = buffer;
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatVertex].offset = offset;
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatVertex].range = float_size;
  }

  // Pixel float constants (base 256).
  const Shader::ConstantRegisterMap& pmap = pixel_shader->constant_register_map();
  {
    size_t float_size = sizeof(float) * 4 * std::max(pmap.float_count, UINT32_C(1));
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t* mapping = RingAllocate(uniform_ring_, float_size, ubo_align, buffer, offset);
    if (!mapping) {
      return skip("ubo_overflow_pfloat");
    }
    uint8_t* write = mapping;
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = pmap.float_bitmap[i];
      uint32_t index;
      while (rex::bit_scan_forward(bits, &index)) {
        bits &= ~(1ull << index);
        std::memcpy(write, &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (index << 2)],
                    sizeof(float) * 4);
        write += sizeof(float) * 4;
      }
    }
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatPixel].buffer = buffer;
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatPixel].offset = offset;
    ubo_infos[SpirvShaderTranslator::kConstantBufferFloatPixel].range = float_size;
  }

  if (!upload_ubo(SpirvShaderTranslator::kConstantBufferBoolLoop,
                  &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], sizeof(uint32_t) * (8 + 32))) {
    return skip("ubo_overflow_bool");
  }
  if (!upload_ubo(SpirvShaderTranslator::kConstantBufferFetch,
                  &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], sizeof(uint32_t) * 6 * 32)) {
    return skip("ubo_overflow_fetch");
  }

  VkDescriptorSet constants_set = VK_NULL_HANDLE;
  {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
    VkDescriptorSetAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = constants_descriptor_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &descriptor_set_layout_constants_;
    if (dfn.vkAllocateDescriptorSets(vulkan_device_->device(), &alloc, &constants_set) !=
        VK_SUCCESS) {
      return skip("descriptor_alloc");
    }
    VkWriteDescriptorSet writes[SpirvShaderTranslator::kConstantBufferCount] = {};
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = constants_set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      writes[i].pBufferInfo = &ubo_infos[i];
    }
    dfn.vkUpdateDescriptorSets(vulkan_device_->device(),
                               SpirvShaderTranslator::kConstantBufferCount, writes, 0, nullptr);
  }

  // --- Make vertex buffers resident in shared memory (in-shader vfetch). ---
  for (uint32_t i = 0; i < rex::countof(vmap.vertex_fetch_bitmap); ++i) {
    uint32_t bits = vmap.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(bits, &j)) {
      bits &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      xenos::xe_gpu_vertex_fetch_t vfetch = regs.GetVertexFetch(vfetch_index);
      if (vfetch.type != xenos::FetchConstantType::kVertex &&
          vfetch.type != xenos::FetchConstantType::kInvalidVertex) {
        continue;
      }
      if (vfetch.size) {
        shared_memory_->RequestRange(vfetch.address << 2, vfetch.size << 2);
      }
    }
  }

  // --- Index buffer conversion (guest big-endian -> host little-endian). ---
  bool indexed = false;
  VkBuffer index_buffer = VK_NULL_HANDLE;
  VkDeviceSize index_offset = 0;
  VkIndexType index_type = VK_INDEX_TYPE_UINT16;
  if (index_buffer_info != nullptr && index_count) {
    const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    const size_t elem = is32 ? sizeof(uint32_t) : sizeof(uint16_t);
    const size_t dst_size = size_t(index_count) * elem;
    uint8_t* dst = RingAllocate(index_ring_, dst_size, elem, index_buffer, index_offset);
    if (!dst) {
      return skip("index_overflow");
    }
    const uint8_t* src = memory_->TranslatePhysical(index_buffer_info->guest_base);
    if (is32) {
      auto* out = reinterpret_cast<uint32_t*>(dst);
      for (uint32_t k = 0; k < index_count; ++k) {
        out[k] = ConvertIndex32(src + size_t(k) * 4, index_buffer_info->endianness);
      }
      index_type = VK_INDEX_TYPE_UINT32;
    } else {
      auto* out = reinterpret_cast<uint16_t*>(dst);
      for (uint32_t k = 0; k < index_count; ++k) {
        out[k] = ConvertIndex16(src + size_t(k) * 2, index_buffer_info->endianness);
      }
      index_type = VK_INDEX_TYPE_UINT16;
    }
    indexed = true;
  }

  // --- Capture the deferred draw. ---
  DeferredDraw draw = {};
  draw.pipeline = pipeline;
  draw.constants_set = constants_set;
  draw.viewport.x = float(viewport_info.xy_offset[0]);
  draw.viewport.y = float(viewport_info.xy_offset[1]);
  draw.viewport.width = float(std::max(viewport_info.xy_extent[0], UINT32_C(1)));
  draw.viewport.height = float(std::max(viewport_info.xy_extent[1], UINT32_C(1)));
  draw.viewport.minDepth = viewport_info.z_min;
  draw.viewport.maxDepth = viewport_info.z_max;
  draw.scissor.offset = {int32_t(viewport_info.xy_offset[0]), int32_t(viewport_info.xy_offset[1])};
  draw.scissor.extent = {std::max(viewport_info.xy_extent[0], UINT32_C(1)),
                         std::max(viewport_info.xy_extent[1], UINT32_C(1))};
  draw.indexed = indexed;
  draw.index_buffer = index_buffer;
  draw.index_offset = index_offset;
  draw.index_type = index_type;
  draw.draw_count = index_count;
  deferred_draws_.push_back(draw);
  ++deferred_draw_total_;

  if (REXCVAR_GET(native_log_draws)) {
    REXLOG_INFO("rexgpu-native: draw #{} queued prim={} verts={} indexed={} vp={}x{}+{}+{}",
                draw_count_, uint32_t(prim_type), index_count, indexed,
                viewport_info.xy_extent[0], viewport_info.xy_extent[1], viewport_info.xy_offset[0],
                viewport_info.xy_offset[1]);
  }
  return true;
#else
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  return true;
#endif  // REX_HAS_VULKAN
}

bool NativeCommandProcessor::IssueCopy() {
  ++copy_count_;
  if (REXCVAR_GET(native_log_draws)) {
    REXLOG_INFO("rexgpu-native: IssueCopy #{} (EDRAM resolve - not yet native)", copy_count_);
  }
  return true;
}

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  ++swap_count_;
#if REX_HAS_VULKAN
  if (!graphics_system_) {
    return;
  }
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    // Headless (no presentation) - drop the deferred frame and keep counting.
    deferred_draws_.clear();
    frame_open_ = false;
    return;
  }
  if (!vulkan_device_ || clear_render_pass_ == VK_NULL_HANDLE) {
    static bool no_device_logged = false;
    if (!no_device_logged) {
      no_device_logged = true;
      REXLOG_ERROR("rexgpu-native: IssueSwap has a presenter but no device state");
    }
    return;
  }

  const uint32_t width = frontbuffer_width ? frontbuffer_width : 1280u;
  const uint32_t height = frontbuffer_height ? frontbuffer_height : 720u;

  const uint32_t clear_raw = register_file_->values[XE_GPU_REG_RB_COLOR_CLEAR];
  const bool used_guest = clear_raw != 0;
  std::array<float, 4> clear_rgba;
  if (used_guest) {
    clear_rgba[0] = float((clear_raw >> 16) & 0xFF) / 255.0f;
    clear_rgba[1] = float((clear_raw >> 8) & 0xFF) / 255.0f;
    clear_rgba[2] = float(clear_raw & 0xFF) / 255.0f;
    clear_rgba[3] = float((clear_raw >> 24) & 0xFF) / 255.0f;
  } else {
    // Draw path clears via the render pass to black; only fall back to the
    // recognisable mid-blue when there's nothing to draw.
    clear_rgba = deferred_draws_.empty() ? std::array<float, 4>{0.16f, 0.36f, 0.72f, 1.0f}
                                         : std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
  }

  const size_t draw_replay_count = deferred_draws_.size();
  if (clear_raw != last_logged_clear_raw_ || draw_replay_count) {
    last_logged_clear_raw_ = clear_raw;
    REXLOG_INFO("rexgpu-native: IssueSwap #{} {}x{} draws={} clear=0x{:08X}", swap_count_, width,
                height, draw_replay_count, clear_raw);
  }

  const bool presented = presenter->RefreshGuestOutput(
      width, height, width, height,
      [this, width, height, clear_rgba](
          ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vk_ctx =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        const VkImage image = vk_ctx.image();
        const VkImageView image_view = vk_ctx.image_view();
        const bool ever_written = vk_ctx.image_ever_written_previously();
        context.SetIs8bpc(deferred_draws_.empty());

        if (!EnsureClearFramebuffer(image_view, vk_ctx.image_version(), width, height)) {
          return false;
        }

        const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device_->functions();
        const VkDevice device = vulkan_device_->device();

        dfn.vkResetCommandPool(device, command_pool_, 0);

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dfn.vkBeginCommandBuffer(command_buffer_, &begin_info) != VK_SUCCESS) {
          return false;
        }

        VkImageSubresourceRange subresource_range = {};
        subresource_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        subresource_range.levelCount = 1;
        subresource_range.layerCount = 1;

        // Before the guest draws execute, make all host writes to shared memory /
        // uniform / index buffers (memcpy'd during IssueDraw) visible to the GPU.
        VkMemoryBarrier host_barrier = {};
        host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                                     VK_ACCESS_UNIFORM_READ_BIT;
        dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_HOST_BIT,
                                 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 1, &host_barrier, 0, nullptr, 0, nullptr);

        VkImageMemoryBarrier acquire_barrier = {};
        acquire_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire_barrier.srcAccessMask =
            ever_written ? VkAccessFlags(VK_ACCESS_SHADER_READ_BIT) : VkAccessFlags(0);
        acquire_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        acquire_barrier.oldLayout = ever_written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_UNDEFINED;
        acquire_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        acquire_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire_barrier.image = image;
        acquire_barrier.subresourceRange = subresource_range;
        dfn.vkCmdPipelineBarrier(
            command_buffer_,
            ever_written ? VkPipelineStageFlags(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                         : VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &acquire_barrier);

        VkClearValue clear_value = {};
        clear_value.color.float32[0] = clear_rgba[0];
        clear_value.color.float32[1] = clear_rgba[1];
        clear_value.color.float32[2] = clear_rgba[2];
        clear_value.color.float32[3] = clear_rgba[3];

        VkRenderPassBeginInfo rp_begin = {};
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_begin.renderPass = clear_render_pass_;
        rp_begin.framebuffer = clear_framebuffer_;
        rp_begin.renderArea.extent = {width, height};
        rp_begin.clearValueCount = 1;
        rp_begin.pClearValues = &clear_value;
        dfn.vkCmdBeginRenderPass(command_buffer_, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

        // Replay the frame's deferred guest draws into the guest output image.
        for (const DeferredDraw& draw : deferred_draws_) {
          dfn.vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
          VkDescriptorSet sets[2] = {shared_memory_descriptor_set_, draw.constants_set};
          dfn.vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      guest_pipeline_layout_, 0, 2, sets, 0, nullptr);
          VkViewport viewport = draw.viewport;
          dfn.vkCmdSetViewport(command_buffer_, 0, 1, &viewport);
          // Clamp scissor to the framebuffer.
          VkRect2D scissor = draw.scissor;
          if (uint32_t(scissor.offset.x) >= width || uint32_t(scissor.offset.y) >= height) {
            scissor.offset = {0, 0};
            scissor.extent = {width, height};
          } else {
            scissor.extent.width = std::min(scissor.extent.width, width - uint32_t(scissor.offset.x));
            scissor.extent.height =
                std::min(scissor.extent.height, height - uint32_t(scissor.offset.y));
          }
          dfn.vkCmdSetScissor(command_buffer_, 0, 1, &scissor);
          if (draw.indexed) {
            dfn.vkCmdBindIndexBuffer(command_buffer_, draw.index_buffer, draw.index_offset,
                                     draw.index_type);
            dfn.vkCmdDrawIndexed(command_buffer_, draw.draw_count, 1, 0, 0, 0);
          } else {
            dfn.vkCmdDraw(command_buffer_, draw.draw_count, 1, 0, 0);
          }
        }

        dfn.vkCmdEndRenderPass(command_buffer_);

        VkImageMemoryBarrier release_barrier = acquire_barrier;
        release_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        release_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        release_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        release_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dfn.vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &release_barrier);

        if (dfn.vkEndCommandBuffer(command_buffer_) != VK_SUCCESS) {
          return false;
        }

        dfn.vkResetFences(device, 1, &clear_fence_);
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer_;
        {
          const ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
              vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
          if (dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit_info, clear_fence_) !=
              VK_SUCCESS) {
            REXLOG_ERROR("rexgpu-native: vkQueueSubmit failed for guest draw present");
            return false;
          }
        }
        dfn.vkWaitForFences(device, 1, &clear_fence_, VK_TRUE, UINT64_MAX);
        return true;
      });

  // The frame's transient resources are now safe to recycle on the next draw.
  deferred_draws_.clear();
  frame_open_ = false;

  if (!presented) {
    static bool present_fail_logged = false;
    if (!present_fail_logged) {
      present_fail_logged = true;
      REXLOG_WARN("rexgpu-native: RefreshGuestOutput returned false ({}x{})", width, height);
    }
    return;
  }

  // [TEMP DIAG] Dump the presented frame as a PPM (same env knobs as the Vulkan
  // backend): REX_DUMP_FRAME=<prefix>.
  {
    static const char* dump_prefix = getenv("REX_DUMP_FRAME");
    if (dump_prefix) {
      static const uint32_t dump_every =
          getenv("REX_DUMP_FRAME_EVERY") ? uint32_t(atoi(getenv("REX_DUMP_FRAME_EVERY"))) : 150u;
      static const uint32_t dump_start =
          getenv("REX_DUMP_FRAME_START") ? uint32_t(atoi(getenv("REX_DUMP_FRAME_START"))) : 0u;
      static const uint32_t dump_max =
          getenv("REX_DUMP_FRAME_MAX") ? uint32_t(atoi(getenv("REX_DUMP_FRAME_MAX"))) : 12u;
      static uint32_t dumped = 0;
      const uint32_t frame_counter = uint32_t(swap_count_);
      if (dumped < dump_max && dump_every && frame_counter >= dump_start &&
          (frame_counter % dump_every) == 0) {
        ui::RawImage raw_image;
        if (presenter->CaptureGuestOutput(raw_image)) {
          char path[512];
          snprintf(path, sizeof(path), "%s_%04u.ppm", dump_prefix, frame_counter);
          FILE* f = fopen(path, "wb");
          if (f) {
            fprintf(f, "P6\n%u %u\n255\n", raw_image.width, raw_image.height);
            const uint8_t* src = raw_image.data.data();
            for (uint32_t y = 0; y < raw_image.height; ++y) {
              const uint8_t* row = src + size_t(y) * raw_image.stride;
              for (uint32_t x = 0; x < raw_image.width; ++x) {
                fwrite(row + size_t(x) * 4, 1, 3, f);
              }
            }
            fclose(f);
            ++dumped;
            REXLOG_WARN("rexgpu-native: [DUMP] swap {} -> {}", frame_counter, path);
          }
        }
      }
    }
  }
#else
  (void)frontbuffer_ptr;
  (void)frontbuffer_width;
  (void)frontbuffer_height;
#endif  // REX_HAS_VULKAN
}

void NativeCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) {
  (void)base_ptr;
  (void)length;
}

void NativeCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  (void)snapshot;
}

}  // namespace rex::graphics::native
