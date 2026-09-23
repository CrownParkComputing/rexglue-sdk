/**
 * @file        presenter_streamer.h
 * @brief       GPU-direct guest frame streaming to an external consumer.
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 *
 * @remarks     Enabled via the REX_PRESENT_STREAM environment variable (set to
 *              a unix socket path). Every refreshed guest output image is
 *              copied GPU-side into one of three stream images whose memory is
 *              exported as opaque fds (VK_KHR_external_memory_fd); a small
 *              socket server hands memory + semaphore descriptors to the
 *              consumer (the rexmenu launcher, importing via
 *              GL_EXT_memory_object_fd). Replaces the CaptureGuestOutput
 *              readback + shared-memory ring on the launcher display path.
 */

#ifndef REX_UI_VULKAN_PRESENTER_STREAMER_H_
#define REX_UI_VULKAN_PRESENTER_STREAMER_H_

#include <cstdint>

#include "rex/ui/vulkan/device.h"

namespace rex {
namespace ui {
namespace vulkan {

// Called after a guest output image refresh completes (the image contains a
// frame ready for consumption; reads of it at kGuestOutputInternalAccessMask
// have completed by submission order on the graphics/compute queue). Also
// performs lazy one-time initialization and submits the stream copy for the
// newest frame (the "tick"). Safe to call on every refresh.
void PresenterStreamNotifyImage(const VulkanDevice* vulkan_device, VkImage image, uint32_t width,
                                uint32_t height);

// True only while a working direct GPU consumer owns presentation.
// The shared-memory fallback must not read back frames in this state.
bool PresenterStreamHasConsumer();

// Called from the presenter destructor while the device is still alive.
void PresenterStreamShutdown();

}  // namespace vulkan
}  // namespace ui
}  // namespace rex

#endif  // REX_UI_VULKAN_PRESENTER_STREAMER_H_
