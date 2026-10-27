#pragma once

#include <algorithm>
#include <span>
#include <vulkan/vulkan.h>

namespace xn::gfx {
inline VkRect2D FitPresentationRect(VkExtent2D source, VkExtent2D output) {
  if (!source.width || !source.height || !output.width || !output.height) return {};
  VkExtent2D size = output;
  if (uint64_t(output.width)*source.height > uint64_t(output.height)*source.width)
    size.width = uint32_t(uint64_t(output.height)*source.width/source.height);
  else
    size.height = uint32_t(uint64_t(output.width)*source.height/source.width);
  size.width = std::max(1u,size.width);
  size.height = std::max(1u,size.height);
  return {{int32_t((output.width-size.width)/2),int32_t((output.height-size.height)/2)},size};
}

inline bool PresentModeAvailable(VkPresentModeKHR mode,
                                 std::span<const VkPresentModeKHR> available) {
  return std::find(available.begin(), available.end(), mode) != available.end();
}

// `force_fifo` restores strict FIFO (XN_VK_PRESENT_FIFO=1).
inline VkPresentModeKHR ChoosePresentMode(bool vsync, uint32_t target_fps,
                                         float display_hz,
                                         std::span<const VkPresentModeKHR> available,
                                         bool force_fifo = false) {
  if (vsync && display_hz > 0.f && float(target_fps) <= display_hz + .5f) {
    // FIFO blocks the producer until the next vblank, so a frame that misses
    // its deadline by any margin costs a whole refresh: the rate snaps from 60
    // to 30 rather than easing to 55, and a frame-paced title's simulation
    // halves with it. Mailbox keeps the same tear-free vblank presentation but
    // does not stall the producer, so an occasional long frame costs only
    // itself. FIFO remains the fallback and is always supported.
    if (!force_fifo && PresentModeAvailable(VK_PRESENT_MODE_MAILBOX_KHR, available))
      return VK_PRESENT_MODE_MAILBOX_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
  }
  if (force_fifo) return VK_PRESENT_MODE_FIFO_KHR;
  if (!vsync && PresentModeAvailable(VK_PRESENT_MODE_IMMEDIATE_KHR,available))
    return VK_PRESENT_MODE_IMMEDIATE_KHR;
  // Mailbox permits a faster producer without tearing. Unknown display rates
  // must not silently cap the requested rate either. FIFO is always supported.
  for (auto mode : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
    if (PresentModeAvailable(mode, available)) return mode;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}
}  // namespace xn::gfx
