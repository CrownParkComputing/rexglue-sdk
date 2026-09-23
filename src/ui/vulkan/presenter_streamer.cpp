/**
 * @file        presenter_streamer.cpp
 * @brief       GPU-direct guest frame streaming to an external consumer.
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 *
 * @remarks     When REX_PRESENT_STREAM=<socket path> is set, every refreshed
 *              guest output image is copied (GPU-side, zero CPU pixel traffic)
 *              into one of three stream images whose memory is exported as an
 *              opaque POSIX file descriptor (VK_KHR_external_memory_fd). A
 *              small socket server hands the memory + copy-signal semaphore
 *              descriptors to the external consumer (the rexmenu launcher,
 *              which imports them via GL_EXT_memory_object_fd and samples them
 *              directly), replacing the CaptureGuestOutput readback + shared
 *              memory ring on the launcher display path.
 *
 *              Synchronization:
 *               - Vulkan signals copy_done after releasing an image to EXTERNAL.
 *               - GL waits copy_done, samples, then signals released after its
 *                 final draw and tells the server which sequence was released.
 *               - Vulkan waits released only on reuse, acquiring ownership back.
 *               - Socket frame notices follow submission, keeping the menu from
 *                 waiting on nonexistent frames during loading or a pause.
 *
 *              All state lives in a function-local static guarded by a mutex;
 *              entry points run on the presenter's refresher/paint (UI) thread
 *              and in the destructor.
 */

#include "rex/ui/vulkan/presenter_streamer.h"

#include <rex/logging.h>
#include <rex/ui/vulkan/presenter.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace rex {
namespace ui {
namespace vulkan {
namespace {

// The SDK's device/instance function tables only carry the Vulkan entry
// points the SDK itself uses; the streamer needs two more (fd export and
// image-to-image copy), loaded once per device here.
struct StreamerFunctions {
  PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR = nullptr;
  PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR = nullptr;
  PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
  PFN_vkGetPhysicalDeviceImageFormatProperties2 vkGetPhysicalDeviceImageFormatProperties2 =
      nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
};

StreamerFunctions& Funcs() {
  static StreamerFunctions funcs;
  return funcs;
}

bool LoadStreamerFunctions(const VulkanDevice* vulkan_device) {
  StreamerFunctions& funcs = Funcs();
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VulkanInstance::Functions& ifn = vulkan_device->vulkan_instance()->functions();
  funcs.vkGetMemoryFdKHR =
      dfn.vkGetMemoryFdKHR
          ? dfn.vkGetMemoryFdKHR
          : reinterpret_cast<PFN_vkGetMemoryFdKHR>(
                ifn.vkGetDeviceProcAddr(vulkan_device->device(), "vkGetMemoryFdKHR"));
  funcs.vkGetSemaphoreFdKHR =
      dfn.vkGetSemaphoreFdKHR
          ? dfn.vkGetSemaphoreFdKHR
          : reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
                ifn.vkGetDeviceProcAddr(vulkan_device->device(), "vkGetSemaphoreFdKHR"));
  funcs.vkCmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(
      ifn.vkGetDeviceProcAddr(vulkan_device->device(), "vkCmdCopyImage"));
  funcs.vkGetPhysicalDeviceImageFormatProperties2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
          ifn.vkGetInstanceProcAddr(vulkan_device->vulkan_instance()->instance(),
                                    "vkGetPhysicalDeviceImageFormatProperties2"));
  funcs.vkGetPhysicalDeviceMemoryProperties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          ifn.vkGetInstanceProcAddr(vulkan_device->vulkan_instance()->instance(),
                                    "vkGetPhysicalDeviceMemoryProperties"));
  return funcs.vkGetMemoryFdKHR && funcs.vkGetSemaphoreFdKHR && funcs.vkCmdCopyImage &&
         funcs.vkGetPhysicalDeviceImageFormatProperties2 &&
         funcs.vkGetPhysicalDeviceMemoryProperties;
}

constexpr uint32_t kStreamBufferCount = 3;

// Wire header sent to the consumer before the file descriptors. All slots
// share one allocation size (same format/extent/dedicated allocation).
struct StreamHello {
  uint32_t magic;          // 'SXMS'
  uint32_t version;        // 2: memory, ready semaphore, released semaphore per slot
  uint32_t width;
  uint32_t height;
  uint32_t buffer_count;
  uint32_t alloc_size_low;
  uint32_t alloc_size_high;
  uint32_t format;         // VkFormat (A2B10G10R10_UNORM_PACK32), informational
};
constexpr uint32_t kStreamHelloMagic = 0x534D5853u;  // 'SXMS' little-endian
constexpr uint32_t kStreamHelloVersion = 2;

struct StreamSlot {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize allocation_size = 0;
  int memory_fd = -1;
  VkSemaphore copy_done_semaphore = VK_NULL_HANDLE;  // signalled by each copy
  int semaphore_fd = -1;
  VkSemaphore released_semaphore = VK_NULL_HANDLE;
  int released_fd = -1;
  VkFence copy_fence = VK_NULL_HANDLE;               // host-visible completion
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  uint64_t sequence = 0;      // highest sequence copied into this slot (0 = never)
  bool fence_submitted = false;  // copy_fence armed, not yet reaped
};

struct StreamState {
  std::mutex mutex;

  bool attempted = false;   // lazy initialization attempted at least once
  bool active = false;      // streaming enabled and resources created
  bool warned = false;      // one warning logged

  std::string socket_path;
  const VulkanDevice* device = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;

  // Newest refreshed guest output (tracked by the notify entry point).
  VkImage last_guest_output_image = VK_NULL_HANDLE;
  uint64_t last_guest_output_sequence = 0;  // stream sequences issued so far

  VkCommandPool command_pool = VK_NULL_HANDLE;
  std::vector<StreamSlot> slots;
  uint64_t head_sequence = 0;  // newest sequence with a copy submitted (0 = none)

  int listen_fd = -1;
  int client_fd = -1;
  std::thread server_thread;
  std::atomic<bool> server_running{false};
  std::atomic<uint64_t> retired_sequence{0};
  std::atomic<bool> consumer_connected{false};
};

StreamState& State() {
  static StreamState state;
  return state;
}

bool CheckExternalFormatSupport(const VulkanDevice* vulkan_device) {
  VkPhysicalDeviceExternalImageFormatInfo external_info = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
  external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkPhysicalDeviceImageFormatInfo2 format_info = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  format_info.pNext = &external_info;
  format_info.format = VulkanPresenter::kGuestOutputFormat;
  format_info.type = VK_IMAGE_TYPE_2D;
  format_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  format_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  VkExternalImageFormatProperties external_properties = {
      VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 properties2 = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  properties2.pNext = &external_properties;
  const VulkanInstance::Functions& ifn = vulkan_device->vulkan_instance()->functions();
  if (Funcs().vkGetPhysicalDeviceImageFormatProperties2(
          vulkan_device->physical_device(), &format_info, &properties2) != VK_SUCCESS ||
      !(external_properties.externalMemoryProperties.externalMemoryFeatures &
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
    return false;
  }
  return true;
}

bool CreateStreamResources(const VulkanDevice* vulkan_device) {
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  StreamState& state = State();

  VkCommandPoolCreateInfo command_pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  command_pool_info.queueFamilyIndex = vulkan_device->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &command_pool_info, nullptr, &state.command_pool) !=
      VK_SUCCESS) {
    REXLOG_ERROR("PresenterStream: Failed to create the command pool");
    return false;
  }

  VkCommandBufferAllocateInfo command_buffer_info = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_buffer_info.commandPool = state.command_pool;
  command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_buffer_info.commandBufferCount = kStreamBufferCount;
  VkCommandBuffer command_buffers[kStreamBufferCount];
  if (dfn.vkAllocateCommandBuffers(device, &command_buffer_info, command_buffers) != VK_SUCCESS) {
    REXLOG_ERROR("PresenterStream: Failed to allocate the command buffers");
    return false;
  }

  state.slots.resize(kStreamBufferCount);
  for (uint32_t i = 0; i < kStreamBufferCount; ++i) {
    StreamSlot& slot = state.slots[i];
    slot.command_buffer = command_buffers[i];

    VkExternalMemoryImageCreateInfo external_image_info = {
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external_image_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.pNext = &external_image_info;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VulkanPresenter::kGuestOutputFormat;
    image_info.extent.width = state.width;
    image_info.extent.height = state.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC kept for future effects; dedicated allocation for the fd.
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dfn.vkCreateImage(device, &image_info, nullptr, &slot.image) != VK_SUCCESS) {
      REXLOG_ERROR("PresenterStream: Failed to create the stream image {}", i);
      return false;
    }

    VkMemoryRequirements requirements;
    dfn.vkGetImageMemoryRequirements(device, slot.image, &requirements);
    slot.allocation_size = requirements.size;

    // Exportable, device-local memory type permitted by the requirements.
    VkPhysicalDeviceMemoryProperties memory_properties;
    Funcs().vkGetPhysicalDeviceMemoryProperties(vulkan_device->physical_device(),
                                                &memory_properties);
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t type_index = 0; type_index < memory_properties.memoryTypeCount; ++type_index) {
      if (!(requirements.memoryTypeBits & (1u << type_index))) {
        continue;
      }
      if (!(memory_properties.memoryTypes[type_index].propertyFlags &
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        continue;
      }
      memory_type = type_index;
      break;
    }
    if (memory_type == UINT32_MAX) {
      REXLOG_ERROR("PresenterStream: No device-local memory type for the stream images");
      return false;
    }

    VkExportMemoryAllocateInfo export_allocate_info = {
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    export_allocate_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryDedicatedAllocateInfo dedicated_allocate_info = {
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated_allocate_info.image = slot.image;
    dedicated_allocate_info.pNext = export_allocate_info.pNext;
    export_allocate_info.pNext = &dedicated_allocate_info;
    VkMemoryAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate_info.pNext = &export_allocate_info;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type;
    if (dfn.vkAllocateMemory(device, &allocate_info, nullptr, &slot.memory) != VK_SUCCESS) {
      REXLOG_ERROR("PresenterStream: Failed to allocate the stream image memory {}", i);
      return false;
    }
    if (dfn.vkBindImageMemory(device, slot.image, slot.memory, 0) != VK_SUCCESS) {
      REXLOG_ERROR("PresenterStream: Failed to bind the stream image memory {}", i);
      return false;
    }

    VkMemoryGetFdInfoKHR get_fd_info = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    get_fd_info.memory = slot.memory;
    get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (Funcs().vkGetMemoryFdKHR(device, &get_fd_info, &slot.memory_fd) != VK_SUCCESS) {
      REXLOG_ERROR("PresenterStream: Failed to export the stream image memory {}", i);
      return false;
    }

    VkExportSemaphoreCreateInfo export_semaphore_info = {
        VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    export_semaphore_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphore_info.pNext = &export_semaphore_info;
    if (dfn.vkCreateSemaphore(device, &semaphore_info, nullptr, &slot.copy_done_semaphore) !=
        VK_SUCCESS) {
      REXLOG_ERROR("PresenterStream: Failed to create the copy semaphore {}", i);
      return false;
    }
    VkSemaphoreGetFdInfoKHR semaphore_fd_info = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    semaphore_fd_info.semaphore = slot.copy_done_semaphore;
    semaphore_fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (Funcs().vkGetSemaphoreFdKHR(device, &semaphore_fd_info, &slot.semaphore_fd) !=
        VK_SUCCESS) {
      REXLOG_ERROR("PresenterStream: Failed to export the copy semaphore {}", i);
      return false;
    }

    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    // First submission needs an unsignaled fence, just like every reuse.
    if (dfn.vkCreateSemaphore(device, &semaphore_info, nullptr, &slot.released_semaphore) != VK_SUCCESS) return false;
    semaphore_fd_info.semaphore = slot.released_semaphore;
    if (Funcs().vkGetSemaphoreFdKHR(device, &semaphore_fd_info, &slot.released_fd) != VK_SUCCESS) return false;
    if (dfn.vkCreateFence(device, &fence_info, nullptr, &slot.copy_fence) != VK_SUCCESS) {
      REXLOG_ERROR("PresenterStream: Failed to create the copy fence {}", i);
      return false;
    }
  }
  return true;
}

void SendHello(int client_fd) {
  StreamState& state = State();
  StreamHello hello = {};
  hello.magic = kStreamHelloMagic;
  hello.version = kStreamHelloVersion;
  hello.width = state.width;
  hello.height = state.height;
  hello.buffer_count = kStreamBufferCount;
  hello.alloc_size_low = uint32_t(state.slots[0].allocation_size & 0xFFFFFFFFu);
  hello.alloc_size_high = uint32_t(state.slots[0].allocation_size >> 32);
  hello.format = uint32_t(VulkanPresenter::kGuestOutputFormat);

  int fds[kStreamBufferCount * 3];
  for (uint32_t i = 0; i < kStreamBufferCount; ++i) {
    fds[i * 3] = state.slots[i].memory_fd;
    fds[i * 3 + 1] = state.slots[i].semaphore_fd;
    fds[i * 3 + 2] = state.slots[i].released_fd;
  }

  struct msghdr message = {};
  struct iovec iov = {};
  iov.iov_base = &hello;
  iov.iov_len = sizeof(hello);
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(fds))];
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  struct cmsghdr* control_header = CMSG_FIRSTHDR(&message);
  control_header->cmsg_level = SOL_SOCKET;
  control_header->cmsg_type = SCM_RIGHTS;
  control_header->cmsg_len = CMSG_LEN(sizeof(fds));
  std::memcpy(CMSG_DATA(control_header), fds, sizeof(fds));
  if (sendmsg(client_fd, &message, MSG_NOSIGNAL) < 0) {
    REXLOG_WARN("PresenterStream: Failed to send the descriptors to the consumer");
  }
}

void ServerLoop() {
  StreamState& state = State();
  while (state.server_running.load(std::memory_order_relaxed)) {
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (!state.server_running.load(std::memory_order_relaxed)) break;
      if (state.client_fd < 0) {
        int client = accept4(state.listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client >= 0) {
          if (!state.active) { close(client); }
          else {
            state.client_fd = client;
            SendHello(client);
            state.retired_sequence.store(0, std::memory_order_relaxed);
            state.consumer_connected.store(true, std::memory_order_release);
            REXLOG_INFO("PresenterStream: Consumer connected");
          }
        }
      } else {
        uint64_t retired = 0;
        ssize_t received = recv(state.client_fd, &retired, sizeof(retired), MSG_DONTWAIT);
        if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
          close(state.client_fd);
          state.client_fd = -1;
          state.consumer_connected.store(false, std::memory_order_release);
          state.active = false;
          REXLOG_INFO("PresenterStream: Consumer disconnected; using readback fallback");
        } else if (received == ssize_t(sizeof(retired)) && retired <= state.head_sequence &&
                   retired >= state.retired_sequence.load(std::memory_order_relaxed)) {
          state.retired_sequence.store(retired, std::memory_order_relaxed);
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool EnsureServer() {
  StreamState& state = State();
  if (state.server_running.load(std::memory_order_relaxed)) {
    return true;
  }
  state.listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
  if (state.listen_fd < 0) {
    REXLOG_ERROR("PresenterStream: Failed to create the socket");
    return false;
  }
  struct sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, state.socket_path.c_str(), sizeof(address.sun_path) - 1);
  unlink(address.sun_path);
  if (bind(state.listen_fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) !=
      0) {
    REXLOG_ERROR("PresenterStream: Failed to bind {}", state.socket_path);
    close(state.listen_fd);
    state.listen_fd = -1;
    return false;
  }
  if (listen(state.listen_fd, 2) != 0) {
    REXLOG_ERROR("PresenterStream: Failed to listen on {}", state.socket_path);
    close(state.listen_fd);
    state.listen_fd = -1;
    return false;
  }
  state.server_running.store(true, std::memory_order_relaxed);
  state.server_thread = std::thread(ServerLoop);
  return true;
}

// Records and submits the copy of the newest guest output into the slot.
bool SubmitCopyForSlot(const VulkanDevice* vulkan_device, VkImage guest_output_image,
                       StreamSlot& slot, uint64_t sequence) {
  const VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  StreamState& state = State();

  // Reap the copy fence before re-recording. Reuse additionally waits on
  // the separate semaphore signaled after the consumer's last draw.
  if (slot.fence_submitted) {
    if (dfn.vkGetFenceStatus(device, slot.copy_fence) != VK_SUCCESS) {
      return false;  // still in flight; the next refresh retries
    }
    slot.fence_submitted = false;
    dfn.vkResetFences(device, 1, &slot.copy_fence);
  }

  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn.vkBeginCommandBuffer(slot.command_buffer, &begin_info) != VK_SUCCESS) {
    return false;
  }

  // Guest output: kGuestOutputInternalLayout -> TRANSFER_SRC (and back after
  // the copy, preserving the presenter's internal layout contract).
  VkImageMemoryBarrier guest_to_transfer = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  guest_to_transfer.srcAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
  guest_to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  guest_to_transfer.oldLayout = VulkanPresenter::kGuestOutputInternalLayout;
  guest_to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  guest_to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  guest_to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  guest_to_transfer.image = guest_output_image;
  guest_to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  guest_to_transfer.subresourceRange.levelCount = 1;
  guest_to_transfer.subresourceRange.layerCount = 1;
  dfn.vkCmdPipelineBarrier(slot.command_buffer, VulkanPresenter::kGuestOutputInternalStageMask,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &guest_to_transfer);

  // Stream image: previous GENERAL (or UNDEFINED first use) -> TRANSFER_DST.
  VkImageMemoryBarrier stream_to_transfer = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  stream_to_transfer.srcAccessMask = VK_ACCESS_NONE;
  stream_to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  stream_to_transfer.oldLayout =
      slot.sequence ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
  stream_to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  stream_to_transfer.srcQueueFamilyIndex = slot.sequence ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
  stream_to_transfer.dstQueueFamilyIndex = slot.sequence ? vulkan_device->queue_family_graphics_compute() : VK_QUEUE_FAMILY_IGNORED;
  stream_to_transfer.image = slot.image;
  stream_to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  stream_to_transfer.subresourceRange.levelCount = 1;
  stream_to_transfer.subresourceRange.layerCount = 1;
  dfn.vkCmdPipelineBarrier(slot.command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &stream_to_transfer);

  VkImageCopy region = {};
  region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.dstSubresource.layerCount = 1;
  region.extent.width = state.width;
  region.extent.height = state.height;
  region.extent.depth = 1;
  Funcs().vkCmdCopyImage(slot.command_buffer, guest_output_image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  // Park the stream image in GENERAL - the layout external consumers may
  // sample imported memory in.
  VkImageMemoryBarrier stream_to_general = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  stream_to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  stream_to_general.dstAccessMask = VK_ACCESS_NONE;
  stream_to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  stream_to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  stream_to_general.srcQueueFamilyIndex = vulkan_device->queue_family_graphics_compute();
  stream_to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
  stream_to_general.image = slot.image;
  stream_to_general.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  stream_to_general.subresourceRange.levelCount = 1;
  stream_to_general.subresourceRange.layerCount = 1;
  dfn.vkCmdPipelineBarrier(slot.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &stream_to_general);

  VkImageMemoryBarrier guest_back_internal = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  guest_back_internal.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  guest_back_internal.dstAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
  guest_back_internal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  guest_back_internal.newLayout = VulkanPresenter::kGuestOutputInternalLayout;
  guest_back_internal.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  guest_back_internal.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  guest_back_internal.image = guest_output_image;
  guest_back_internal.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  guest_back_internal.subresourceRange.levelCount = 1;
  guest_back_internal.subresourceRange.layerCount = 1;
  dfn.vkCmdPipelineBarrier(slot.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0,
                           nullptr, 1, &guest_back_internal);

  dfn.vkEndCommandBuffer(slot.command_buffer);

  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.waitSemaphoreCount = slot.sequence ? 1 : 0; // No wait on first use.
  submit_info.pWaitSemaphores = &slot.released_semaphore;
  submit_info.pWaitDstStageMask = &wait_stage;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &slot.command_buffer;
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &slot.copy_done_semaphore;
  {
    const VulkanDevice::Queue::Acquisition queue_acquisition =
        vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
    if (dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit_info, slot.copy_fence) !=
        VK_SUCCESS) {
      REXLOG_ERROR("PresenterStream: Failed to submit the stream copy");
      return false;
    }
  }
  slot.fence_submitted = true;
  slot.sequence = sequence;
  return true;
}

}  // namespace

void PresenterStreamNotifyImage(const VulkanDevice* vulkan_device, VkImage image, uint32_t width,
                                uint32_t height) {
  StreamState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.attempted) {
    state.attempted = true;
    const char* socket_path = std::getenv("REX_PRESENT_STREAM");
    if (!socket_path || !*socket_path) {
      return;  // feature off - silent, zero cost afterwards
    }
    state.socket_path = socket_path;
    state.device = vulkan_device;
    state.width = width;
    state.height = height;
    if (!LoadStreamerFunctions(vulkan_device)) {
      if (!state.warned) {
        REXLOG_WARN("PresenterStream: Failed to load the Vulkan entry points, streaming off");
        state.warned = true;
      }
      return;
    }
    if (!vulkan_device->extensions().ext_KHR_external_memory_fd) {
      if (!state.warned) {
        REXLOG_WARN("PresenterStream: VK_KHR_external_memory_fd is not enabled, streaming off");
        state.warned = true;
      }
      return;
    }
    if (!CheckExternalFormatSupport(vulkan_device)) {
      if (!state.warned) {
        REXLOG_WARN("PresenterStream: The guest output format is not exportable, streaming off");
        state.warned = true;
      }
      return;
    }
    if (!CreateStreamResources(vulkan_device)) {
      if (!state.warned) {
        REXLOG_WARN("PresenterStream: Resource creation failed, streaming off");
        state.warned = true;
      }
      return;
    }
    if (!EnsureServer()) {
      return;
    }
    state.active = true;
    REXLOG_INFO("PresenterStream: Streaming {}x{} x{} slots to {}", width, height,
                kStreamBufferCount, socket_path);
  }
  if (!state.active) {
    return;
  }
  if (width != state.width || height != state.height) {
    // Mode change: stop streaming rather than migrate resources mid-frame.
    if (!state.warned) {
      REXLOG_WARN("PresenterStream: Resolution changed {}x{} -> {}x{}, streaming off",
                  state.width, state.height, width, height);
      state.warned = true;
    }
    state.active = false;
    return;
  }

  state.last_guest_output_image = image;
  if (!state.consumer_connected.load(std::memory_order_acquire)) {
    return;  // nobody is watching; keep the newest image reference only
  }

  // Submit the copy for the next sequence, honoring slot lifecycle.
  const uint64_t next_sequence = state.head_sequence + 1;
  StreamSlot& slot = state.slots[next_sequence % kStreamBufferCount];
  if (slot.sequence) {
    if (slot.sequence > state.retired_sequence.load(std::memory_order_relaxed)) {
      return;  // not retired by the consumer yet
    }
  }
  if (SubmitCopyForSlot(vulkan_device, image, slot, next_sequence)) {
    state.head_sequence = next_sequence;
    if (next_sequence == 1 || next_sequence % 300 == 0) REXLOG_INFO("PresenterStream: Submitted frame {}", next_sequence);
    // Announce only submitted work; the menu must never wait for a future frame.
    if (send(state.client_fd, &next_sequence, sizeof(next_sequence), MSG_NOSIGNAL | MSG_DONTWAIT) != sizeof(next_sequence)) {
      state.active = false;
      shutdown(state.client_fd, SHUT_RDWR);
    }
  }
}

bool PresenterStreamHasConsumer() {
  StreamState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.active && state.consumer_connected.load(std::memory_order_acquire);
}

void PresenterStreamShutdown() {
  StreamState& state = State();
  std::unique_lock<std::mutex> lock(state.mutex);
  if (!state.attempted) {
    return;
  }
  state.active = false;
  state.server_running.store(false, std::memory_order_relaxed);
  if (state.listen_fd >= 0) {
    close(state.listen_fd);
    state.listen_fd = -1;
  }
  if (state.client_fd >= 0) {
    close(state.client_fd);
    state.client_fd = -1;
  }
  if (!state.socket_path.empty()) {
    unlink(state.socket_path.c_str());
  }
  lock.unlock();
  if (state.server_thread.joinable()) {
    state.server_thread.join();
  }
  lock.lock();
  if (state.device) {
    const VulkanDevice::Functions& dfn = state.device->functions();
    const VkDevice device = state.device->device();
    for (StreamSlot& slot : state.slots) {
      if (slot.copy_fence) {
        dfn.vkDestroyFence(device, slot.copy_fence, nullptr);
        slot.copy_fence = VK_NULL_HANDLE;
      }
      if (slot.released_semaphore) dfn.vkDestroySemaphore(device, slot.released_semaphore, nullptr);
      if (slot.released_fd >= 0) close(slot.released_fd);
      if (slot.copy_done_semaphore) {
        dfn.vkDestroySemaphore(device, slot.copy_done_semaphore, nullptr);
        slot.copy_done_semaphore = VK_NULL_HANDLE;
      }
      if (slot.memory_fd >= 0) {
        close(slot.memory_fd);
        slot.memory_fd = -1;
      }
      if (slot.semaphore_fd >= 0) {
        close(slot.semaphore_fd);
        slot.semaphore_fd = -1;
      }
      if (slot.image) {
        dfn.vkDestroyImage(device, slot.image, nullptr);
        slot.image = VK_NULL_HANDLE;
      }
      if (slot.memory) {
        dfn.vkFreeMemory(device, slot.memory, nullptr);
        slot.memory = VK_NULL_HANDLE;
      }
    }
    if (state.command_pool) {
      dfn.vkDestroyCommandPool(device, state.command_pool, nullptr);
      state.command_pool = VK_NULL_HANDLE;
    }
  }
  state.slots.clear();
  state.last_guest_output_image = VK_NULL_HANDLE;
  state.head_sequence = 0;
  state.attempted = false;
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
