// Vd*: display facts, ring buffer hand-off, interrupts, VdSwap.
#include <cstring>

#include <rex/hook.h>

#include "gfx/command_processor.h"
#include "kernel.h"
#include "gfx/frame_rate.h"

using namespace xn;

void FillVideoMode(X_VIDEO_MODE* mode) {
  Kernel& k = K();
  std::memset(mode, 0, sizeof(*mode));
  mode->display_width = k.config.display_width;
  mode->display_height = k.config.display_height;
  mode->is_interlaced = 0;
  mode->is_widescreen = k.config.display_width * 3 >= k.config.display_height * 4;
  mode->is_hi_def = k.config.display_width >= 1280 || k.config.display_height >= 720;
  mode->refresh_rate = float(xn::FrameRate());
  mode->video_standard = 1;  // NTSC
  mode->unknown_0x8a = 0x4A;
  mode->unknown_0x01 = 0x01;
}

void VdQueryVideoMode_entry(ppc_ptr_t<X_VIDEO_MODE> mode) { FillVideoMode(mode); }
XN_IMPORT(__imp__VdQueryVideoMode, VdQueryVideoMode_entry)

u32 VdQueryVideoFlags_entry() {
  X_VIDEO_MODE m;
  FillVideoMode(&m);
  uint32_t flags = 0;
  if (m.is_widescreen) flags |= 1;
  if (uint32_t(m.display_width) >= 1024) flags |= 2;
  if (uint32_t(m.display_width) >= 1920) flags |= 4;
  return flags;
}
XN_IMPORT(__imp__VdQueryVideoFlags, VdQueryVideoFlags_entry)

void VdGetCurrentDisplayGamma_entry(mapped_u32 type_ptr, mapped_f32 power_ptr) {
  *type_ptr = 2;  // BT.709
  *power_ptr = 2.22222233f;
}
XN_IMPORT(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma_entry)

void VdGetCurrentDisplayInformation_entry(u32 info) {
  X_VIDEO_MODE m;
  FillVideoMode(&m);
  uint32_t w = m.display_width, h = m.display_height;
  std::memset(G(info), 0, 0x58);
  Store16(info + 0x00, uint16_t(w));
  Store16(info + 0x02, uint16_t(h));
  Store32(info + 0x08 + 0x08, w);  // scaler source rect x2
  Store32(info + 0x08 + 0x0C, h);
  Store32(info + 0x08 + 0x10, w);  // scaled output w/h
  Store32(info + 0x08 + 0x14, h);
  Store32(info + 0x08 + 0x18, 1);  // vertical filter
  Store32(info + 0x08 + 0x28, 1);  // horizontal filter
  Store16(info + 0x40, uint16_t(w / 4));
  Store16(info + 0x42, uint16_t(h / 4));
  Store16(info + 0x44, uint16_t(w / 4));
  Store16(info + 0x46, uint16_t(h / 4));
  Store16(info + 0x48, uint16_t(w));
  Store16(info + 0x4A, uint16_t(h));
  float refresh = float(xn::FrameRate());
  uint32_t bits;
  std::memcpy(&bits, &refresh, 4);
  Store32(info + 0x4C, bits);
  Store16(info + 0x56, uint16_t(w));
}
XN_IMPORT(__imp__VdGetCurrentDisplayInformation, VdGetCurrentDisplayInformation_entry)

u32 VdSetDisplayMode_entry(u32 flags) { REXKRNL_INFO("VdSetDisplayMode({:#x})", flags); return 0; }
XN_IMPORT(__imp__VdSetDisplayMode, VdSetDisplayMode_entry)
u32 VdInitializeEngines_entry(u32, u32, u32, u32, u32) { return 1; }
XN_IMPORT(__imp__VdInitializeEngines, VdInitializeEngines_entry)
void VdShutdownEngines_entry() {}
XN_IMPORT(__imp__VdShutdownEngines, VdShutdownEngines_entry)
u32 VdGetGraphicsAsicID_entry() { return 0x11; }
XN_IMPORT(__imp__VdGetGraphicsAsicID, VdGetGraphicsAsicID_entry)
u32 VdEnableDisableClockGating_entry(u32) { return 0; }
XN_IMPORT(__imp__VdEnableDisableClockGating, VdEnableDisableClockGating_entry)
void VdInitializeEDRAM_entry() {}
XN_IMPORT(__imp__VdInitializeEDRAM, VdInitializeEDRAM_entry)
u32 VdIsHSIOTrainingSucceeded_entry() { return 1; }
XN_IMPORT(__imp__VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceeded_entry)
u32 VdRetrainEDRAMWorker_entry(u32) { return 0; }
XN_IMPORT(__imp__VdRetrainEDRAMWorker, VdRetrainEDRAMWorker_entry)
u32 VdRetrainEDRAM_entry(u32, u32, u32, u32, u32, u32) { return 0; }
XN_IMPORT(__imp__VdRetrainEDRAM, VdRetrainEDRAM_entry)
u32 VdCallGraphicsNotificationRoutines_entry(u32, u32) { return 0; }
XN_IMPORT(__imp__VdCallGraphicsNotificationRoutines, VdCallGraphicsNotificationRoutines_entry)
void VdSetSystemCommandBufferGpuIdentifierAddress_entry(u32) {}
XN_IMPORT(__imp__VdSetSystemCommandBufferGpuIdentifierAddress, VdSetSystemCommandBufferGpuIdentifierAddress_entry)

void VdSetGraphicsInterruptCallback_entry(u32 callback, u32 user_data) {
  K().gfx_interrupt_callback = callback;
  K().gfx_interrupt_user_data = user_data;
  if (K().gpu) K().gpu->SetInterruptCallback(callback, user_data);
}
XN_IMPORT(__imp__VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback_entry)

void VdInitializeRingBuffer_entry(u32 ptr, u32 size_log2) {
  if (K().gpu) K().gpu->InitializeRingBuffer(ptr, size_log2);
}
XN_IMPORT(__imp__VdInitializeRingBuffer, VdInitializeRingBuffer_entry)

void VdEnableRingBufferRPtrWriteBack_entry(u32 ptr, u32 block_size_log2) {
  if (K().gpu) K().gpu->EnableReadPointerWriteBack(ptr, block_size_log2);
}
XN_IMPORT(__imp__VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack_entry)

void VdGetSystemCommandBuffer_entry(u32 p0, u32 p1) {
  std::memset(G(p0), 0, 0x94);
  Store32(p0, 0xBEEF0000);
  Store32(p1, 0xBEEF0001);
}
XN_IMPORT(__imp__VdGetSystemCommandBuffer, VdGetSystemCommandBuffer_entry)

u32 VdInitializeScalerCommandBuffer_entry(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32,
                                          u32 dest_ptr, u32 dest_count) {
  for (uint32_t i = 0; i < dest_count; i++) Store32(dest_ptr + i * 4, 0x80000000);  // type-2 NOP
  return dest_count;
}
XN_IMPORT(__imp__VdInitializeScalerCommandBuffer, VdInitializeScalerCommandBuffer_entry)

u32 VdPersistDisplay_entry(u32, mapped_u32 out_ptr) {
  if (out_ptr) *out_ptr = K().arena.AllocPhysical(64, 0x10000, 32, 0, 0xFFFFFFFF);
  return 1;
}
XN_IMPORT(__imp__VdPersistDisplay, VdPersistDisplay_entry)

// VdSwap: the caller reserved 64 dwords in the ring for us. Emit the front
// buffer fetch constant (base rewritten from the CPU alias to the physical
// page: xe_gpu_texture_fetch dword 1 bits 12..31) and a swap packet the
// command processor understands.
void VdSwap_entry(u32 buffer_ptr, u32 fetch_ptr, u32, u32, u32, mapped_u32 frontbuffer_ptr,
                  mapped_u32 texture_format_ptr, mapped_u32 color_space_ptr, mapped_u32 width,
                  mapped_u32 height) {
  uint32_t fetch[6];
  for (int i = 0; i < 6; i++) fetch[i] = Load32(fetch_ptr + i * 4);
  uint32_t front = frontbuffer_ptr ? uint32_t(*frontbuffer_ptr) : 0;
  if (!front) front = fetch[1] & 0xFFFFF000u;
  uint32_t physical = K().arena.GetPhysicalAddress(front);
  if (physical == UINT32_MAX) physical = front & 0x1FFFFFFF;
  fetch[1] = (fetch[1] & 0xFFFu) | (physical & 0xFFFFF000u);
  std::memset(G(buffer_ptr), 0, 64 * 4);
  uint32_t o = 0;
  auto put = [&](uint32_t v) { Store32(buffer_ptr + (o++) * 4, v); };
  put((0u << 30) | ((6 - 1) << 16) | 0x4800);  // type-0: SHADER_CONSTANT_FETCH_00_0, 6 dwords
  for (int i = 0; i < 6; i++) put(fetch[i]);
  put((3u << 30) | ((4 - 1) << 16) | (0x64 << 8));  // type-3 XE_SWAP
  put(0x53574150);
  put(physical);
  put(width ? uint32_t(*width) : 0);
  put(height ? uint32_t(*height) : 0);
  while (o < 64) put(0x80000000);  // type-2 NOPs
  static int logged = 0;
  if (logged++ < 3)
    REXKRNL_INFO("VdSwap frontbuffer {:#010x} (physical {:#010x}) {}x{} format {:#x}", front,
                 physical, width ? uint32_t(*width) : 0, height ? uint32_t(*height) : 0,
                 texture_format_ptr ? uint32_t(*texture_format_ptr) : 0);
}
XN_IMPORT(__imp__VdSwap, VdSwap_entry)
