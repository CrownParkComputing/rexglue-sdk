/**
 ******************************************************************************
 * xenon-native - guest memory mirror: changed-page snapshot upload
 *
 * The SG rendering fix, generalized to the common per-draw path: snapshot the
 * pages whose guest contents changed into a fenced upload ring, then copy
 * them into the device-local mirror buffer on the GPU timeline. Direct host
 * overwrites of the mirror could corrupt draws already queued this frame, and
 * address-only per-frame dedupe was stale when a page changed between draws.
 * Page hashes and coalesced copies keep the steady state cheap.
 ******************************************************************************
 */
#include "vulkan_renderer_impl.h"

#include <algorithm>
#include <cstdlib>

// Use the renderer's AVX2 baseline for the per-draw page validation.
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace xn::gfx {

bool VulkanRenderer::Impl::MirrorGuestRange(uint32_t guest_address, uint32_t size) {
  if (!size) return true;
  if (!guest_base) return false;
  const uint64_t end = uint64_t(guest_address) + size;
  // 64-bit arithmetic on purpose: a 32-bit address + size that wraps past
  // 2^32 must stay invalid, not wrap into a valid range.
  if (end > uint64_t(guest_size) || end > kGuestPhysicalSize) {
    static int logged = 0;
    if (logged++ < 8) {
      REXLOG_WARN("xn-vk: vertex/index range {:#x}+{:#x} outside guest memory", guest_address,
                  size);
    }
    return false;
  }
  auto& changed = scratch_changed_pages;
  auto& copies = scratch_copies;
  changed.clear();
  copies.clear();
  VkDeviceSize upload_size = 0;
  const uint32_t prepared_begin = guest_address/kMirrorPageSize*kMirrorPageSize;
  const uint64_t prepared_end = std::min<uint64_t>(
      (end+kMirrorPageSize-1)/kMirrorPageSize*kMirrorPageSize,guest_size);
  PrepareGuestRead(prepared_begin,uint32_t(prepared_end-prepared_begin));
  for (uint32_t page = guest_address / kMirrorPageSize;
       page < (end + kMirrorPageSize - 1) / kMirrorPageSize; ++page) {
    const uint32_t address = page * kMirrorPageSize;
    const uint32_t bytes = uint32_t(std::min<uint64_t>(kMirrorPageSize, guest_size - address));
    const uint64_t token = write_watch ? write_watch->Token(page) : 0;
    if (token && config.mirror_dedupe_per_frame && mirrored_pages.test(page) &&
        mirror_write_tokens[page] == token) continue;

    // Guest buffers can be rewritten anywhere in a page between draws. A
    // sample of its first cache line (especially XOR) cannot prove the rest
    // unchanged; it missed queued UI/geometry updates within the same frame.
    const uint64_t hash = XXH3_64bits(guest_base + address, bytes);
    stats.mirror_bytes_hashed += bytes;
    if (config.mirror_dedupe_per_frame && mirrored_pages.test(page) &&
        mirror_page_hashes[page] == hash) {
      mirror_write_tokens[page] = token;
      continue;
    }
    changed.push_back({page, hash, token});
    // Coalesce consecutive pages into one copy region.
    if (!copies.empty() && copies.back().dstOffset + copies.back().size == address)
      copies.back().size += bytes;
    else
      copies.push_back({upload_size, address, bytes});
    upload_size += bytes;
  }
  if (changed.empty()) return true;
  VkDeviceSize upload_offset = 0;
  uint8_t* upload = RingAllocate(mirror_upload_ring, upload_size, 4, upload_offset);
  VkBuffer upload_buffer = mirror_upload_ring.buf.buffer;
  if (!upload) {
    // Ring full: fall back to a one-shot staging buffer retired with the frame.
    HostBuffer extra;
    if (!CreateHostBuffer(upload_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, extra, false))
      return false;
    upload = extra.mapped;
    upload_buffer = extra.buffer;
    frames[frame_index].garbage.push_back(extra);
  }
  for (auto& copy : copies) {
    std::memcpy(upload + copy.srcOffset, guest_base + copy.dstOffset, copy.size);
    copy.srcOffset += upload_offset;
  }
  // The snapshot must not race the draw commands already recorded in this
  // command buffer, and the mirror copy must complete before the vertex
  // shaders read the mirror. This runs between render passes.
  EndRendering();
  GpuWorkScope profile(WorkProfile(), cb, "mirror_upload", guest_swap_number,
      0, 0, 0, 0, upload_size);
  VkMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cb, &dependency);
  vkCmdCopyBuffer(cb, upload_buffer, mirror.buffer, uint32_t(copies.size()), copies.data());
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
  vkCmdPipelineBarrier2(cb, &dependency);
  for (const auto& page : changed) {
    mirror_page_hashes[page.index] = page.hash;
    mirror_write_tokens[page.index] = page.write_token;
    mirrored_pages.set(page.index);
  }
  stats.mirror_bytes_copied_frame += upload_size;
  return true;
}

}  // namespace xn::gfx
