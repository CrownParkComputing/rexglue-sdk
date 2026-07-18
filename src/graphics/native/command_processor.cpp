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

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "native/graphics_system.h"

#if REX_HAS_VULKAN
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#endif

// Per-draw stream logging is very chatty (PGR3 issues thousands of draws per
// frame). Off by default; enable with --native_log_draws to watch the seam
// receive fully-decoded PM4 work for any title.
REXCVAR_DEFINE_BOOL(native_log_draws, false, "GPU/Native",
                    "Log every draw/copy the native renderer receives from the PM4 stream");

namespace rex::graphics::native {

namespace {

// Cheap FNV-1a over the microcode words. A real backend uses xxHash; this only
// needs to deduplicate identical shaders within the scaffold's shader cache.
uint64_t HashUcode(const uint32_t* dwords, uint32_t count) {
  uint64_t hash = 1469598103934665603ull;
  for (uint32_t i = 0; i < count; ++i) {
    hash ^= dwords[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

NativeCommandProcessor::NativeCommandProcessor(NativeGraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

NativeCommandProcessor::~NativeCommandProcessor() = default;

bool NativeCommandProcessor::SetupContext() {
#if REX_HAS_VULKAN
  // The base graphics system already stood up the Vulkan provider (and, when
  // presenting, a presenter) - borrow that device rather than creating one.
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
  REXLOG_INFO("rexgpu-native: SetupContext - Phase 1 clear-present path ready (device={})",
              vulkan_device_->properties().deviceName);
  return true;
#else
  REXLOG_ERROR("rexgpu-native: SetupContext - built without Vulkan support");
  return false;
#endif
}

void NativeCommandProcessor::ShutdownContext() {
  REXLOG_INFO("rexgpu-native: ShutdownContext (draws={} copies={} swaps={})", draw_count_,
              copy_count_, swap_count_);
#if REX_HAS_VULKAN
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

  // One command pool + primary command buffer on the graphics/compute queue
  // family - the same queue the presenter uses to refresh and paint.
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

  // Render pass that clears the guest output image. The presenter's guest output
  // image is VK_FORMAT_A2B10G10R10_UNORM_PACK32 with COLOR_ATTACHMENT usage (but
  // not TRANSFER_DST), so a LOAD_OP_CLEAR render pass is how we paint it a solid
  // colour. Acquire/release layout transitions are done with explicit barriers
  // around the render pass, so the attachment stays COLOR_ATTACHMENT_OPTIMAL for
  // the pass itself.
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

  // All our submitted work is fenced-and-waited each swap, so nothing is in
  // flight here.
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
    // Frees command_buffer_ implicitly.
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

  // Safe to destroy the previous framebuffer: the previous swap waited on its
  // fence, so no GPU work still references it.
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

#endif  // REX_HAS_VULKAN

Shader* NativeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  const uint64_t hash = HashUcode(host_address, dword_count);
  auto it = shader_map_.find(hash);
  if (it != shader_map_.end()) {
    return it->second;
  }

  // The base Shader keeps the parsed microcode. A native backend attaches a
  // translated host module (reusing the existing SpirvShaderTranslator).
  auto shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
  Shader* ptr = shader.get();
  shader_storage_.push_back(std::move(shader));
  shader_map_.emplace(hash, ptr);

  REXLOG_TRACE("rexgpu-native: LoadShader type={} guest=0x{:08X} dwords={} hash=0x{:016X}",
               shader_type == xenos::ShaderType::kVertex ? "VS" : "PS", guest_address, dword_count,
               hash);
  return ptr;
}

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info, bool major_mode_explicit) {
  (void)major_mode_explicit;
  ++draw_count_;
  if (REXCVAR_GET(native_log_draws)) {
    REXLOG_INFO("rexgpu-native: IssueDraw #{} prim={} indices={} indexed={} vs={} ps={}",
                draw_count_, static_cast<uint32_t>(prim_type), index_count,
                index_buffer_info != nullptr, static_cast<void*>(active_vertex_shader()),
                static_cast<void*>(active_pixel_shader()));
  }
  // Return true: the draw is "consumed". Nothing is rendered yet (Phase 2).
  return true;
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
    // Headless (no presentation) - nothing to show, but keep counting swaps.
    return;
  }
  if (!vulkan_device_ || clear_render_pass_ == VK_NULL_HANDLE) {
    static bool no_device_logged = false;
    if (!no_device_logged) {
      no_device_logged = true;
      REXLOG_ERROR("rexgpu-native: IssueSwap has a presenter but no clear device state");
    }
    return;
  }

  const uint32_t width = frontbuffer_width ? frontbuffer_width : 1280u;
  const uint32_t height = frontbuffer_height ? frontbuffer_height : 720u;

  // Phase 1: decode the guest clear colour from RB_COLOR_CLEAR. Titles usually
  // clear via a full-screen draw rather than this register, so it is frequently
  // 0; in that case fall back to a fixed, recognisable mid-blue so the native
  // path still presents a clearly non-black frame. Correct per-frame clear
  // colour is a Phase 2 concern.
  const uint32_t clear_raw = register_file_->values[XE_GPU_REG_RB_COLOR_CLEAR];
  const bool used_guest = clear_raw != 0;
  std::array<float, 4> clear_rgba;
  if (used_guest) {
    // Interpret as 8:8:8:8 A8R8G8B8 (the common 32bpp surface packing).
    clear_rgba[0] = float((clear_raw >> 16) & 0xFF) / 255.0f;  // R
    clear_rgba[1] = float((clear_raw >> 8) & 0xFF) / 255.0f;   // G
    clear_rgba[2] = float(clear_raw & 0xFF) / 255.0f;          // B
    clear_rgba[3] = float((clear_raw >> 24) & 0xFF) / 255.0f;  // A
  } else {
    clear_rgba = {0.16f, 0.36f, 0.72f, 1.0f};  // mid-blue placeholder
  }

  if (clear_raw != last_logged_clear_raw_) {
    last_logged_clear_raw_ = clear_raw;
    REXLOG_INFO(
        "rexgpu-native: IssueSwap #{} {}x{} RB_COLOR_CLEAR=0x{:08X} -> "
        "rgba({:.3f},{:.3f},{:.3f},{:.3f}) [{}]",
        swap_count_, width, height, clear_raw, clear_rgba[0], clear_rgba[1], clear_rgba[2],
        clear_rgba[3], used_guest ? "guest" : "placeholder-mid-blue");
  }

  const bool presented = presenter->RefreshGuestOutput(
      width, height, width, height,
      [this, width, height, clear_rgba](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vk_ctx =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        const VkImage image = vk_ctx.image();
        const VkImageView image_view = vk_ctx.image_view();
        const bool ever_written = vk_ctx.image_ever_written_previously();
        // The clear produces a flat colour; treat the source as 8bpc so the
        // presenter can take its cheaper 8bpc paths.
        context.SetIs8bpc(true);

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
        subresource_range.baseMipLevel = 0;
        subresource_range.levelCount = 1;
        subresource_range.baseArrayLayer = 0;
        subresource_range.layerCount = 1;

        // Acquire barrier -> COLOR_ATTACHMENT_OPTIMAL. On the first write to a
        // fresh mailbox image the old layout is UNDEFINED; otherwise the
        // presenter left it in SHADER_READ_ONLY_OPTIMAL after sampling it.
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
        rp_begin.renderArea.offset = {0, 0};
        rp_begin.renderArea.extent = {width, height};
        rp_begin.clearValueCount = 1;
        rp_begin.pClearValues = &clear_value;
        dfn.vkCmdBeginRenderPass(command_buffer_, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
        // No draws: LOAD_OP_CLEAR fills the whole attachment with clear_value.
        dfn.vkCmdEndRenderPass(command_buffer_);

        // Release barrier: hand the image back to the presenter in the layout it
        // expects to sample it from (SHADER_READ_ONLY_OPTIMAL / fragment read).
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
          // Same graphics/compute queue 0 the presenter refreshes and paints on.
          const ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
              vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
          if (dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit_info, clear_fence_) !=
              VK_SUCCESS) {
            REXLOG_ERROR("rexgpu-native: vkQueueSubmit failed for clear present");
            return false;
          }
        }
        // Wait so the single command buffer + framebuffer are safe to recycle on
        // the next swap. Phase 1 serializes here for simplicity.
        dfn.vkWaitForFences(device, 1, &clear_fence_, VK_TRUE, UINT64_MAX);
        return true;
      });

  if (!presented) {
    static bool present_fail_logged = false;
    if (!present_fail_logged) {
      present_fail_logged = true;
      REXLOG_WARN("rexgpu-native: RefreshGuestOutput returned false ({}x{})", width, height);
    }
    return;
  }

  // [TEMP DIAG] Dump the presented frame as a PPM so the native clear can be
  // verified headlessly (same env knobs as the Vulkan backend so identical run
  // commands work): REX_DUMP_FRAME=<prefix> dumps every REX_DUMP_FRAME_EVERY
  // swaps (default 150), up to REX_DUMP_FRAME_MAX files (default 12), starting
  // once REX_DUMP_FRAME_START swaps have elapsed.
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
