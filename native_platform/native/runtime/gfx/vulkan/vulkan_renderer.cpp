/**
 ******************************************************************************
 * xenon-native - VulkanRenderer: device, swapchain, frames, memory, present
 ******************************************************************************
 */
#include "vulkan_renderer_impl.h"
#include "presentation_mode.h"
#include "pipeline_cache_file.h"
#include "gpu_timestamp.h"
#include "render_capture.h"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <sys/resource.h>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/spirv_translator_features.h>

extern "C" void xn_cpu_api_audit_checkpoint(const char*, uint64_t) __attribute__((weak));

namespace xn::gfx {

namespace {

uint64_t DiagnosticNanoseconds() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint32_t EnvironmentUnsigned(const char* name, uint32_t fallback, uint32_t maximum) {
  if (const char* value = std::getenv(name)) {
    uint32_t result = 0;
    const char* end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, result);
    if (parsed.ec == std::errc{} && parsed.ptr == end && result <= maximum) return result;
  }
  return fallback;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    REXLOG_WARN("vk-validation: {}", data->pMessage ? data->pMessage : "");
  }
  return VK_FALSE;
}

}  // namespace

// ---------------------------------------------------------------------------
// Factory / entry points
// ---------------------------------------------------------------------------
VulkanRenderer::VulkanRenderer(const VulkanRendererConfig& config)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = config;
  impl_->edram_tile_capacity = VirtualEdramTileCapacity(
      SelectVirtualEdramMiB(config.virtual_edram_mib, std::getenv("XN_VK_VIRTUAL_EDRAM")));
  impl_->stats.edram_tile_capacity = impl_->edram_tile_capacity;
  const char* timings = std::getenv("XN_GPU_TIMES");
  impl_->measure_timings = (timings && timings[0] == '1') ||
                           std::getenv("XN_FRAME_TIMES") != nullptr;
  const char* draw_cache = std::getenv("XN_VK_DRAW_STATE_CACHE");
  impl_->cache_draw_state = !draw_cache || draw_cache[0] == '1';
  const char* prepared_draw_state = std::getenv("XN_VK_PREPARED_DRAW_STATE");
  impl_->cache_prepared_draw_state =
      prepared_draw_state && std::strcmp(prepared_draw_state, "1") == 0;
  const char* dirty_preparation = std::getenv("XN_VK_DIRTY_DRAW_PREPARATION");
  impl_->cache_dirty_preparation = dirty_preparation
      ? std::strcmp(dirty_preparation, "1") == 0 : config.dirty_draw_preparation;
  const char* index_cache = std::getenv("XN_VK_INDEX_REUSE");
  impl_->reuse_dma_indices = index_cache && std::strcmp(index_cache, "1") == 0;
  const char* resident_indices = std::getenv("XN_VK_RESIDENT_INDEX_CACHE");
  impl_->cache_resident_indices = resident_indices && std::strcmp(resident_indices, "1") == 0;
  impl_->resident_index_min_count = EnvironmentUnsigned("XN_VK_RESIDENT_INDEX_MIN_COUNT", 256, 1048576);
  impl_->resident_index_mib = EnvironmentUnsigned("XN_VK_RESIDENT_INDEX_MIB", 32, 256);
  const char* prepared_indices = std::getenv("XN_VK_PREPARED_INDEX_CACHE");
  impl_->cache_prepared_indices = prepared_indices && std::strcmp(prepared_indices, "1") == 0;
  if (const char* minimum = std::getenv("XN_VK_PREPARED_INDEX_MIN_COUNT")) {
    uint32_t count = 0;
    const char* end = minimum + std::strlen(minimum);
    const auto parsed = std::from_chars(minimum, end, count);
    if (parsed.ec == std::errc{} && parsed.ptr == end && count <= 1048576)
      impl_->prepared_index_min_count = count;
  }
  const char* readonly_barriers = std::getenv("XN_VK_READONLY_BARRIER_ELISION");
  impl_->readonly_barrier_elision = readonly_barriers && std::strcmp(readonly_barriers, "1") == 0;
  const char* texture_dirty = std::getenv("XN_VK_TEXTURE_DIRTY_CACHE");
  // Host texture revisions may change independently of guest registers.
  // Those providers retain the ordinary preparation path.
  impl_->cache_texture_dirty = !config.texture_source &&
      (!texture_dirty || std::strcmp(texture_dirty, "1") == 0);
  const char* batch_profile = std::getenv("XN_VK_DRAW_BATCH_PROFILE");
  impl_->profile_draw_batches = batch_profile && std::strcmp(batch_profile, "1") == 0;
  const char* instance_profile = std::getenv("XN_VK_INSTANCE_BATCH_PROFILE");
  impl_->profile_instance_batches = impl_->cache_resident_indices &&
      instance_profile && std::strcmp(instance_profile, "1") == 0;
  const char* gpu_constants = std::getenv("XN_VK_GPU_CONSTANTS");
  const char* multi_draw = std::getenv("XN_VK_MULTI_DRAW");
  impl_->multi_draw = multi_draw && std::strcmp(multi_draw, "1") == 0;
  impl_->gpu_draw_constants = gpu_constants ? std::strcmp(gpu_constants, "1") == 0
                                           : impl_->multi_draw;
  impl_->multi_draw &= impl_->gpu_draw_constants;
  const char* cached_fallback = std::getenv("XN_VK_CACHED_UPLOAD_FALLBACK");
  impl_->cached_upload_fallback = cached_fallback && std::strcmp(cached_fallback, "1") == 0;
  const char* float_reuse = std::getenv("XN_VK_FLOAT_CONSTANT_REUSE");
  impl_->reuse_float_constants = !float_reuse || std::strcmp(float_reuse, "1") == 0;
  if (impl_->reuse_float_constants) REXLOG_INFO("xn-vk: immutable float-constant upload reuse enabled");
  if (impl_->cache_texture_dirty) REXLOG_INFO("xn-vk: dirty-tracked texture preparation cache enabled");
  if (impl_->cache_draw_state) REXLOG_INFO("xn-vk: normal draw command-state cache enabled");
  if (impl_->cache_dirty_preparation)
    REXLOG_INFO("xn-vk: dirty-tracked draw preparation enabled");
  if (impl_->cache_prepared_draw_state)
    REXLOG_INFO("xn-vk: prepared draw-state lookup cache enabled");
}
VulkanRenderer::~VulkanRenderer() { Shutdown(); }

#define XN_LOCK() std::lock_guard<std::recursive_mutex> xn_lock_(impl_->mutex)

// --- xn::IRenderer: ring feed ---
bool VulkanRenderer::Initialize(SDL_Window* window, uint8_t* guest_base, uint8_t* physical_base) {
  (void)guest_base;
  XN_LOCK();
  SetGuestMemory(physical_base, size_t(kGuestPhysicalSize));
  return Init(window);
}

void VulkanRenderer::OnRegisterWrite(uint32_t index, uint32_t value) {
  XN_LOCK();
  if (impl_->cache_texture_dirty && index >= 0x4800 && index < 0x48C0 &&
      impl_->regs.values[index] != value) {
    const uint32_t dirty = uint32_t(1) << ((index - 0x4800) / 6);
    impl_->pending_texture_dirty[0] |= dirty;
    impl_->pending_texture_dirty[1] |= dirty;
  }
  if (index < RegisterFile::kRegisterCount) {
    if (impl_->cache_dirty_preparation)
      impl_->preparation_dirty.MarkWrite(index, impl_->regs.values[index], value);
    impl_->regs.values[index] = value;
  }
  // Display gamma ramp. 0x1921..0x1936 is the DC_LUT block; the guest walks it
  // with an index register and writes components in sequence.
  if (index >= 0x1921 && index <= 0x1936) impl_->ObserveDcLutWrite(index, value);
}

void VulkanRenderer::OnConstants(uint32_t register_index, const uint32_t* dwords, uint32_t count) {
  XN_LOCK();
  if (register_index >= RegisterFile::kRegisterCount) return;
  count = std::min(count, uint32_t(RegisterFile::kRegisterCount - register_index));
  // Ranges with write side effects are delivered through OnRegisterWrite so
  // side effects and normalized register values remain authoritative.
  if (!((register_index >= 0x2000 && register_index + count <= 0x2800) ||
        (register_index >= 0x4000 && register_index + count <= 0x4928))) return;
  if (impl_->cache_texture_dirty && register_index < 0x48C0 && register_index + count > 0x4800) {
    uint32_t dirty = 0;
    const uint32_t first = std::max(register_index, 0x4800u);
    const uint32_t last = std::min(register_index + count, 0x48C0u);
    for (uint32_t index = first; index < last; ++index)
      if (impl_->regs.values[index] != __builtin_bswap32(dwords[index - register_index]))
        dirty |= uint32_t(1) << ((index - 0x4800) / 6);
    impl_->pending_texture_dirty[0] |= dirty;
    impl_->pending_texture_dirty[1] |= dirty;
  }
  if (impl_->cache_dirty_preparation && register_index >= 0x4000)
    impl_->preparation_dirty.MarkRange(register_index, count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t value = __builtin_bswap32(dwords[i]);
    if (impl_->cache_dirty_preparation && register_index < 0x2800)
      impl_->preparation_dirty.MarkWrite(register_index + i, impl_->regs.values[register_index + i], value);
    impl_->regs.values[register_index + i] = value;
  }
}

void VulkanRenderer::OnRegisterBatch(const GpuRegisterFile& registers,
                                     std::span<const RegisterRange> ranges) {
  XN_LOCK();
  ApplyRegisterBatchUnlocked(registers, ranges);
}

void VulkanRenderer::ApplyRegisterBatchUnlocked(const GpuRegisterFile& registers,
                                                 std::span<const RegisterRange> ranges) {
  for (const auto& range : ranges) {
    if (!((range.index >= 0x2000 && range.index < 0x2800 && range.count <= 0x2800 - range.index) ||
          (range.index >= 0x4000 && range.index < 0x4928 && range.count <= 0x4928 - range.index))) continue;
    if (impl_->cache_texture_dirty && range.index < 0x48C0 && range.index + range.count > 0x4800) {
      uint32_t dirty = 0;
      const uint32_t first = std::max(range.index, 0x4800u);
      const uint32_t last = std::min(range.index + range.count, 0x48C0u);
      for (uint32_t index = first; index < last; ++index)
        if (impl_->regs.values[index] != registers.values[index])
          dirty |= uint32_t(1) << ((index - 0x4800) / 6);
      impl_->pending_texture_dirty[0] |= dirty;
      impl_->pending_texture_dirty[1] |= dirty;
    }
    if (impl_->cache_dirty_preparation) {
      if (range.index < 0x2800) {
        for (uint32_t i = range.index; i < range.index + range.count; ++i)
          impl_->preparation_dirty.MarkWrite(i, impl_->regs.values[i], registers.values[i]);
      } else impl_->preparation_dirty.MarkRange(range.index, range.count);
    }
    std::memcpy(impl_->regs.values + range.index, registers.values + range.index,
                size_t(range.count) * sizeof(uint32_t));
  }
}

void VulkanRenderer::OnShaderLoad(uint32_t shader_type, uint32_t physical_address,
                                  uint32_t size_dwords, const uint32_t* dwords) {
  XN_LOCK();
  Impl& im = *impl_;
  if (!im.device || !dwords || !size_dwords) return;
  ShaderRef ref;
  ref.ucode = dwords;
  ref.dword_count = size_dwords;
  ref.big_endian = true;
  ShaderEntry* entry = im.GetShader(
      ref, shader_type == 0 ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel);
  (void)physical_address;
  if (shader_type == 0) {
    im.active_vertex_shader = entry;
    if (entry->shader->memexport_eM_written()) {
      REXLOG_INFO("xn-vk: vertex memory export mask={:#x} streams={}",
          entry->shader->memexport_eM_written(), entry->shader->memexport_stream_constants().size());
    }
  } else {
    im.active_pixel_shader = entry;
  }
}

void VulkanRenderer::OnDraw(const DrawPacket& draw, const GpuRegisterFile& gpu_regs) {
  XN_LOCK();
  DrawUnlocked(draw, gpu_regs);
}

void VulkanRenderer::OnDrawWithRegisterBatch(const DrawPacket& draw,
                                              const GpuRegisterFile& registers,
                                              std::span<const RegisterRange> ranges) {
  XN_LOCK();
  ++impl_->stats.fused_draw_callbacks;
  ApplyRegisterBatchUnlocked(registers, ranges);
  DrawUnlocked(draw, registers);
}

void VulkanRenderer::DrawUnlocked(const DrawPacket& draw, const GpuRegisterFile& gpu_regs) {
  Impl& im = *impl_;
  if (!im.device) return;
  // The command processor's register file is the truth at this point.
  static_assert(GpuRegisterFile::kCount == RegisterFile::kRegisterCount);
  // The CP already sent each change. Copying all 80 KB for every city draw
  // moved hundreds of MB per frame, mostly unchanged zeros and constants.
  // Standalone snapshots and returning from the Shape A feed still resync.
  if (!draw.registers_notified || !im.ring_registers_current) {
    if (im.cache_dirty_preparation) im.preparation_dirty.InvalidateAll();
    if (im.cache_texture_dirty) im.MarkTextureFetchChanges(gpu_regs.values + 0x4800);
    std::memcpy(im.regs.values, gpu_regs.values, sizeof(im.regs.values));
  }
  im.ring_registers_current = draw.registers_notified;
  if (im.config.texture_source)
    im.config.texture_source->OnDraw(im.active_pixel_shader ?
        im.active_pixel_shader->shader->ucode_data_hash() : 0, gpu_regs);
  if (im.regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    im.ResolveFromRegisters();
    return;
  }
  DrawParams p;
  p.primitive_type = draw.prim_type;
  p.index_count = draw.index_count;
  p.index_format = draw.index_32bit ? 1 : 0;
  p.index_endian = draw.index_endian;
  if (draw.source == 0) {
    p.indexed = true;
    p.index_base_guest = draw.index_base;
  } else if (draw.source == 1) {
    p.indexed = true;
    p.index_host_data = reinterpret_cast<const uint8_t*>(draw.immediate);
  }
  if (!im.active_vertex_shader) {
    ++im.stats.draws;
    ++im.stats.draws_skipped;
    return;
  }
  // Keep ShaderRef populated for the ordinary path. The opt-in path can use
  // the analysed entries from the ring's most recent shader loads directly.
  p.vertex_shader.hash = im.active_vertex_shader->shader->ucode_data_hash();
  p.vertex_shader.ucode = im.active_vertex_shader->shader->ucode_data().data();
  p.vertex_shader.dword_count = uint32_t(im.active_vertex_shader->shader->ucode_dword_count());
  p.vertex_shader.big_endian = false;  // Shader keeps ucode in host order
  if (im.active_pixel_shader) {
    p.pixel_shader.hash = im.active_pixel_shader->shader->ucode_data_hash();
    p.pixel_shader.ucode = im.active_pixel_shader->shader->ucode_data().data();
    p.pixel_shader.dword_count = uint32_t(im.active_pixel_shader->shader->ucode_dword_count());
    p.pixel_shader.big_endian = false;
  }
  if (im.cache_prepared_draw_state || im.cache_dirty_preparation)
    im.DrawImpl(p, im.active_vertex_shader, im.active_pixel_shader);
  else
    im.DrawImpl(p);
}

void VulkanRenderer::OnEvent(uint32_t initiator, const GpuRegisterFile& gpu_regs) {
  (void)initiator;
  (void)gpu_regs;
}

void VulkanRenderer::OnSwap(uint32_t physical_address, uint32_t width, uint32_t height) {
  XN_LOCK();
  PresentParams p;
  p.frontbuffer_guest_address = physical_address;
  p.width = width;
  p.height = height;
  impl_->PresentImpl(p);
  if (xn_cpu_api_audit_checkpoint && !std::getenv("XN_CPU_API_AUDIT_REPLAY"))
    xn_cpu_api_audit_checkpoint("guest_swap", impl_->guest_swap_number ? impl_->guest_swap_number-1 : 0);
  if (impl_->work_profile && impl_->work_calibrated_clock && impl_->guest_swap_number % 120 == 0)
    impl_->work_profile->Calibrate(impl_->work_calibrated_clock, VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT);
  static FILE* timing = []() -> FILE* {
    const char* path = std::getenv("XN_FRAME_TIMES");
    if (!path) return nullptr;
    FILE* file = std::fopen(path, "w");
    if (file) {
      std::setvbuf(file, nullptr, _IOLBF, 0);  // titles may exit without unwinding
      std::fprintf(file, "elapsed_ms,interval_ms,cp_cpu_ms,draws,mirror_checked_bytes,"
                         "pipelines,shaders_translated,textures_loaded,textures_reuploaded,"
                         "submissions,write_faults,depth_clear_plans_built,depth_clear_plans_reused,"
                         "constant_upload_bytes,constant_uploads_reused,gpu_completed_ms,"
                         "gpu_completed_batches,cpu_batch_fence_wait_ms,cpu_queue_submit_ms,"
                         "gpu_timestamp_valid_bits,draw_state_commands_saved,"
                         "draw_state_pipeline_binds_saved,draw_state_texture_binds_saved,monotonic_ns,guest_frame,cp_user_ms,cp_system_ms,"
                         "resident_index_hits,resident_index_misses,resident_index_bytes_reused,resident_index_fallbacks,"
                         "prepared_shader_lookups_avoided,prepared_pipeline_lookups_avoided,resident_index_memory_flags,preparation_shader_hits,preparation_viewport_hits,preparation_system_hits,preparation_pipeline_hits,dirty_constant_uploads_reused,float_constant_packs_avoided,edram_tile_capacity\n");
    }
    return file;
  }();
  if (timing) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    static const auto start = now;
    static auto previous = now;
    double cpu_ms = 0;
#ifdef CLOCK_THREAD_CPUTIME_ID
    // CPU time excludes waits for guest commands, fences and presentation.
    // This distinguishes an expensive frame from a pacing stall at one
    // clock read per swap, rather than instrumenting every draw.
    timespec cpu_time{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_time);
    const double cpu_now = double(cpu_time.tv_sec) * 1000 + double(cpu_time.tv_nsec) / 1e6;
    static double previous_cpu = cpu_now;
    cpu_ms = cpu_now - previous_cpu;
    previous_cpu = cpu_now;
#endif
    double user_ms = -1, system_ms = -1;
    rusage usage{};
    if (getrusage(RUSAGE_THREAD, &usage) == 0) {
      const double user_now = double(usage.ru_utime.tv_sec) * 1000 + double(usage.ru_utime.tv_usec) / 1000;
      const double system_now = double(usage.ru_stime.tv_sec) * 1000 + double(usage.ru_stime.tv_usec) / 1000;
      static double previous_user = user_now, previous_system = system_now;
      user_ms = user_now - previous_user;
      system_ms = system_now - previous_system;
      previous_user = user_now;
      previous_system = system_now;
    }
    static uint64_t previous_draws = impl_->stats.draws;
    static uint64_t previous_hashed = impl_->stats.mirror_bytes_hashed;
    static auto previous_stats = impl_->stats;
    const uint64_t faults = impl_->write_watch ? impl_->write_watch->fault_count() : 0;
    static uint64_t previous_faults = faults;
    timespec host_time{};
    clock_gettime(CLOCK_MONOTONIC, &host_time);
    const uint64_t host_ns = uint64_t(host_time.tv_sec) * UINT64_C(1000000000) + uint64_t(host_time.tv_nsec);
    // Completed GPU batches lag this guest swap. Aggregate over a steady
    // window; these deltas are not the GPU duration of this particular frame.
    std::fprintf(timing, "%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.3f,%llu,%.3f,%.3f,%u,%llu,%llu,%llu,%llu,%llu,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%u\n",
        std::chrono::duration<double, std::milli>(now - start).count(),
        std::chrono::duration<double, std::milli>(now - previous).count(), cpu_ms,
        static_cast<unsigned long long>(impl_->stats.draws - previous_draws),
        static_cast<unsigned long long>(impl_->stats.mirror_bytes_hashed - previous_hashed),
        static_cast<unsigned long long>(impl_->stats.pipelines - previous_stats.pipelines),
        static_cast<unsigned long long>(impl_->stats.shaders_translated - previous_stats.shaders_translated),
        static_cast<unsigned long long>(impl_->stats.textures_loaded - previous_stats.textures_loaded),
        static_cast<unsigned long long>(impl_->stats.textures_reuploaded - previous_stats.textures_reuploaded),
        static_cast<unsigned long long>(impl_->stats.frames - previous_stats.frames),
        static_cast<unsigned long long>(faults - previous_faults),
        static_cast<unsigned long long>(impl_->stats.depth_clear_plans_built - previous_stats.depth_clear_plans_built),
        static_cast<unsigned long long>(impl_->stats.depth_clear_plans_reused - previous_stats.depth_clear_plans_reused),
        static_cast<unsigned long long>(impl_->stats.constant_upload_bytes - previous_stats.constant_upload_bytes),
        static_cast<unsigned long long>(impl_->stats.constant_uploads_reused - previous_stats.constant_uploads_reused),
        double(impl_->stats.gpu_completed_ns - previous_stats.gpu_completed_ns) / 1e6,
        static_cast<unsigned long long>(impl_->stats.gpu_completed_batches - previous_stats.gpu_completed_batches),
        double(impl_->stats.cpu_fence_wait_ns - previous_stats.cpu_fence_wait_ns) / 1e6,
        double(impl_->stats.cpu_queue_submit_ns - previous_stats.cpu_queue_submit_ns) / 1e6,
        impl_->stats.gpu_timestamp_valid_bits,
        static_cast<unsigned long long>(impl_->stats.draw_state_commands_saved - previous_stats.draw_state_commands_saved),
        static_cast<unsigned long long>(impl_->stats.draw_state_pipeline_binds_saved - previous_stats.draw_state_pipeline_binds_saved),
        static_cast<unsigned long long>(impl_->stats.draw_state_texture_binds_saved - previous_stats.draw_state_texture_binds_saved),
        static_cast<unsigned long long>(host_ns),
        static_cast<unsigned long long>(impl_->guest_swap_number ? impl_->guest_swap_number - 1 : 0),
        user_ms, system_ms,
        static_cast<unsigned long long>(impl_->stats.resident_index_hits - previous_stats.resident_index_hits),
        static_cast<unsigned long long>(impl_->stats.resident_index_misses - previous_stats.resident_index_misses),
        static_cast<unsigned long long>(impl_->stats.resident_index_bytes_reused - previous_stats.resident_index_bytes_reused),
        static_cast<unsigned long long>(impl_->stats.resident_index_fallbacks - previous_stats.resident_index_fallbacks),
        static_cast<unsigned long long>(impl_->stats.prepared_shader_lookups_avoided - previous_stats.prepared_shader_lookups_avoided),
        static_cast<unsigned long long>(impl_->stats.prepared_pipeline_lookups_avoided - previous_stats.prepared_pipeline_lookups_avoided),
        impl_->stats.resident_index_memory_flags,
        static_cast<unsigned long long>(impl_->stats.preparation_shader_hits - previous_stats.preparation_shader_hits),
        static_cast<unsigned long long>(impl_->stats.preparation_viewport_hits - previous_stats.preparation_viewport_hits),
        static_cast<unsigned long long>(impl_->stats.preparation_system_hits - previous_stats.preparation_system_hits),
        static_cast<unsigned long long>(impl_->stats.preparation_pipeline_hits - previous_stats.preparation_pipeline_hits),
        static_cast<unsigned long long>(impl_->stats.dirty_constant_uploads_reused - previous_stats.dirty_constant_uploads_reused),
        static_cast<unsigned long long>(impl_->stats.float_constant_packs_avoided - previous_stats.float_constant_packs_avoided),
        impl_->stats.edram_tile_capacity);
    previous = now;
    previous_draws = impl_->stats.draws;
    previous_hashed = impl_->stats.mirror_bytes_hashed;
    previous_stats = impl_->stats;
    previous_faults = faults;
  }
}

void VulkanRenderer::Present() {
  // On Wayland, a resized surface may continue accepting the old swapchain
  // indefinitely and stretch it. Publish SDL's actual drawable dimensions;
  // OnSwap recreates the swapchain on the command thread when they change.
  Impl& im = *impl_;
  if (!im.window) return;
  int width = 0, height = 0;
  if (SDL_GetWindowSizeInPixels(im.window,&width,&height) && width > 0 && height > 0)
    im.window_pixel_size.store((uint64_t(uint32_t(width)) << 32) | uint32_t(height),
                               std::memory_order_relaxed);
  const SDL_DisplayMode* display = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(im.window));
  im.window_display_hz.store(display ? display->refresh_rate : 0.f,std::memory_order_relaxed);
}

bool VulkanRenderer::CaptureFrame(std::vector<uint8_t>& rgba8, uint32_t& width,
                                  uint32_t& height) {
  XN_LOCK();
  Impl& im = *impl_;
  if (!im.device || !im.last_presented) return false;
  width = im.last_presented->width;
  height = im.last_presented->height;
  const bool ok = im.ReadbackImage(*im.last_presented, rgba8);
  static int logged = 0;
  if (ok && logged < 4) {
    ++logged;
    uint8_t max_value = 0;
    for (uint8_t v : rgba8) max_value = std::max(max_value, v);
    REXLOG_INFO("xn-vk: CaptureFrame {}x{} ok={} max_channel={}", width, height, ok, max_value);
  }
  return ok;
}

bool VulkanRenderer::OnD3DCall(const D3DCall& call) {
  (void)call;
  return false;  // the ring stays authoritative
}

// --- Shape A ---
void VulkanRenderer::SetGuestMemory(uint8_t* base, size_t size) {
  XN_LOCK();
  impl_->FlushNativeDraws();
  ++impl_->descriptor_prepare_epoch;
  impl_->mirrored_pages.reset();
  impl_->dma_index_cache.Reset();
  impl_->prepared_index_cache.Reset();
  impl_->resident_index_cache.InvalidateAll();
  impl_->guest_base = base;
  impl_->guest_size = size;
  impl_->write_watch = PhysicalWriteWatch::Find(base,size);
}
void VulkanRenderer::SetRenderTarget(uint32_t index, const SurfaceDesc& surface) {
  (void)index;
  XN_LOCK();
  impl_->registered_color[surface.edram_base] = surface;
}
void VulkanRenderer::SetDepthStencil(const SurfaceDesc& surface) {
  XN_LOCK();
  impl_->registered_depth[surface.edram_base] = surface;
}
void VulkanRenderer::Draw(const DeviceShadow& d, const DrawParams& p) {
  XN_LOCK();
  if (impl_->cache_dirty_preparation) impl_->preparation_dirty.InvalidateAll();
  if (impl_->cache_texture_dirty) {
    std::array<uint32_t, 192> previous;
    std::memcpy(previous.data(), impl_->regs.values + 0x4800, sizeof(previous));
    d.ExportRegisters(impl_->regs);
    impl_->MarkTextureFetchChanges(previous.data());
  } else d.ExportRegisters(impl_->regs);
  impl_->ring_registers_current = false;
  impl_->DrawImpl(p);
}
void VulkanRenderer::Clear(const DeviceShadow& d, const ClearParams& p) {
  XN_LOCK();
  if (impl_->cache_dirty_preparation) impl_->preparation_dirty.InvalidateAll();
  if (impl_->cache_texture_dirty) {
    std::array<uint32_t, 192> previous;
    std::memcpy(previous.data(), impl_->regs.values + 0x4800, sizeof(previous));
    d.ExportRegisters(impl_->regs);
    impl_->MarkTextureFetchChanges(previous.data());
  } else d.ExportRegisters(impl_->regs);
  impl_->ring_registers_current = false;
  impl_->ClearImpl(p);
}
void VulkanRenderer::Resolve(const DeviceShadow& d, const ResolveParams& p) {
  XN_LOCK();
  if (impl_->cache_dirty_preparation) impl_->preparation_dirty.InvalidateAll();
  if (impl_->cache_texture_dirty) {
    std::array<uint32_t, 192> previous;
    std::memcpy(previous.data(), impl_->regs.values + 0x4800, sizeof(previous));
    d.ExportRegisters(impl_->regs);
    impl_->MarkTextureFetchChanges(previous.data());
  } else d.ExportRegisters(impl_->regs);
  impl_->ring_registers_current = false;
  impl_->ResolveImpl(p);
}
void VulkanRenderer::Present(const PresentParams& p) {
  XN_LOCK();
  impl_->PresentImpl(p);
}
RendererStats VulkanRenderer::GetStats() const {
  XN_LOCK();
  auto result = impl_->stats;
  if (impl_->profile_draw_batches) result.draw_batch_profile = impl_->draw_batch_profile.GetStats();
  if (impl_->profile_instance_batches && impl_->cache_resident_indices)
    result.instance_profile = impl_->instance_batch_profile.GetStats();
  return result;
}

void VulkanRenderer::SetFrameRateTarget(uint32_t fps) {
  XN_LOCK();
  if (fps == impl_->target_frame_rate) return;
  impl_->target_frame_rate = fps;
  impl_->swapchain_dirty = true;
}

void VulkanRenderer::InvalidateGuestRange(uint32_t guest_address, uint32_t size) {
  XN_LOCK();
  Impl& im = *impl_;
  im.FlushNativeDraws();
  const uint64_t lo = guest_address, hi = uint64_t(guest_address) + size;
  ++im.descriptor_prepare_epoch;
  im.texture_binding_cache = {};
  for (uint64_t page = lo / Impl::kMirrorPageSize;
       page < (hi + Impl::kMirrorPageSize - 1) / Impl::kMirrorPageSize && page < Impl::kMirrorPageCount;
       ++page) im.mirrored_pages.reset(page);
  // Textures loaded from that range are stale: recreate on next use.
  for (auto it = im.textures.begin(); it != im.textures.end();) {
    TextureEntry& t = *it->second;
    const uint64_t tlo = t.key.base_address, thi = tlo + t.guest_size;
    bool overlaps = tlo < hi && thi > lo;
    for (const auto& mip : t.mip_memory)
      overlaps |= mip.address < hi && uint64_t(mip.address) + mip.size > lo;
    if (!t.resolved && overlaps) {
      im.WaitIdleAndReset();
      for (auto& frame : im.frames) frame.texture_pool_stale = true;
      for (auto& v : t.views) vkDestroyImageView(im.device, v.second, nullptr);
      im.DestroyImage(t.image);
      it = im.textures.erase(it);
    } else {
      ++it;
    }
  }
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------
bool VulkanRenderer::Init(SDL_Window* window) {
  XN_LOCK();
  Impl& im = *impl_;
  const VulkanRendererConfig& config = im.config;
  im.window = window;
  Present();
  if (im.device) return true;
  if (!config.shader_dump_dir.empty()) {
    REXCVAR_SET(dump_shaders, config.shader_dump_dir.string());
  }
  if (const char* v = std::getenv("XN_DEBUG_FRAME")) im.debug_frame = std::atoll(v);
  if (const char* v = std::getenv("XN_DEBUG_FRAME_COUNT")) im.debug_frame_count = std::atoll(v);
  if (const char* v = std::getenv("XN_DEBUG_SWAP")) im.debug_swap = std::atoll(v);
  if (!im.InitInstance()) return false;
  if (window) {
    if (!SDL_Vulkan_CreateSurface(window, im.instance, nullptr, &im.surface)) {
      REXLOG_ERROR("xn-vk: SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
      return false;
    }
  }
  if (!im.PickDevice()) return false;
  if (!im.InitDevice()) return false;
  if (window && !im.InitSwapchain()) return false;
  if (!im.InitFrames()) return false;
  if (!im.InitMirror()) return false;
  if (!im.InitEdram()) return false;
  if (!im.InitRings()) return false;
  if (const char* path = std::getenv("XN_VK_WORK_PROFILE")) {
    auto profiler = std::make_unique<GpuWorkProfile>();
    if (profiler->Init(im.device, im.queue_timestamp_bits, im.properties.limits.timestampPeriod, path,
                       im.features.pipelineStatisticsQuery && std::getenv("XN_VK_WORK_PIPELINE_STATS"))) {
      im.work_profile = std::move(profiler);
      if (im.work_calibrated_clock)
        im.work_profile->Calibrate(im.work_calibrated_clock, VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT);
    } else REXLOG_WARN("xn-vk: GPU work profiling unavailable");
  }
  if (!im.InitDescriptors()) return false;
  if (!im.InitDummyTexture()) return false;

  // Translator features from what we actually enabled on the device.
  using namespace rex::graphics;
  SpirvTranslatorDeviceInfo info = QuerySpirvTranslatorDeviceInfo(
      im.physical_device, vkGetPhysicalDeviceProperties, vkGetPhysicalDeviceFeatures,
      vkGetPhysicalDeviceProperties2, vkGetPhysicalDeviceFeatures2,
      std::min(im.properties.apiVersion, uint32_t(VK_API_VERSION_1_3)), true, true, false,
      true, true);
  info.fragment_shader_sample_interlock = false;  // not enabled
  im.supports_clip_distance = info.shader_clip_distance;
  im.supports_cull_distance = info.shader_cull_distance;
  im.translator = std::make_unique<SpirvShaderTranslator>(
      MakeSpirvTranslatorFeatures(info), /*native_2x_msaa_with_attachments=*/true,
      /*native_2x_msaa_no_attachments=*/false, /*edram_fragment_shader_interlock=*/false,
      1, 1, im.gpu_draw_constants);
  if (!im.WarmPipelines()) return false;
  REXLOG_INFO("xn-vk: initialised on {} (api {}.{}.{}), mirror {} MB in {} binding(s)",
              im.properties.deviceName, VK_API_VERSION_MAJOR(im.properties.apiVersion),
              VK_API_VERSION_MINOR(im.properties.apiVersion),
              VK_API_VERSION_PATCH(im.properties.apiVersion), kGuestPhysicalSize >> 20,
              im.mirror_binding_count);
  return true;
}

void VulkanRenderer::FlushCaches() {
  XN_LOCK();
  Impl& im = *impl_;
  if (!im.device) return;
  REXLOG_INFO("xn-vk: texture upload staging: {} ring / {} dedicated allocations",
              im.texture_uploads_ring,im.texture_uploads_dedicated);
  if (im.warmup_enabled && !im.pipeline_cache_file.empty() && !im.pipeline_warmup.empty()) {
    if (WritePipelineWarmup(im.pipeline_cache_file.string() + ".warmup",
                           std::span<const Impl::WarmupRecord>(im.pipeline_warmup)))
      REXLOG_INFO("xn-vk: saved {} pipeline warm-up states", im.pipeline_warmup.size());
    else
      REXLOG_WARN("xn-vk: could not save pipeline warm-up states");
  }
  if (!im.pipeline_cache_file.empty() && im.vk_pipeline_cache) {
    size_t bytes = 0;
    if (vkGetPipelineCacheData(im.device,im.vk_pipeline_cache,&bytes,nullptr) == VK_SUCCESS &&
        bytes <= kMaxPipelineCacheBytes) {
      std::vector<uint8_t> cache(bytes);
      if (vkGetPipelineCacheData(im.device,im.vk_pipeline_cache,&bytes,cache.data()) == VK_SUCCESS &&
          WritePipelineCache(im.pipeline_cache_file,std::span(cache).first(bytes),im.properties))
        REXLOG_INFO("xn-vk: saved pipeline cache ({} bytes)",bytes);
    }
  }
}

void VulkanRenderer::Shutdown() {
  XN_LOCK();
  Impl& im = *impl_;
  if (im.dc_lut_writes) {
    REXLOG_INFO("xn-vk: DC_LUT (display gamma ramp) written {} times, table {}",
                im.dc_lut_writes, im.dc_lut_valid ? "captured" : "not captured");
    if (im.dc_lut_valid) {
      std::string row;
      for (uint32_t i = 0; i < 256; i += 32) {
        row += fmt::format(" [{}]=({},{},{})", i, im.dc_lut[i][0], im.dc_lut[i][1],
                           im.dc_lut[i][2]);
      }
      REXLOG_INFO("xn-vk: DC_LUT samples:{}", row);
      // XN_DEBUG_DUMP_LUT=<path>: all 256 entries as CSV. Eight samples is not
      // enough to tell a gamma curve from a near-identity one, and the black
      // end - where a display ramp does the most to a dark game - falls between
      // them entirely.
      if (const char* p = std::getenv("XN_DEBUG_DUMP_LUT")) {
        if (FILE* f = std::fopen(p, "w")) {
          std::fprintf(f, "index,r,g,b\n");
          for (uint32_t i = 0; i < 256; ++i) {
            std::fprintf(f, "%u,%u,%u,%u\n", i, im.dc_lut[i][0], im.dc_lut[i][1],
                         im.dc_lut[i][2]);
          }
          std::fclose(f);
          REXLOG_INFO("xn-vk: DC_LUT written to {}", p);
        }
      }
    }
  } else {
    REXLOG_INFO("xn-vk: DC_LUT never written (no guest gamma ramp)");
  }
  if (!im.device) {
    if (im.instance) {
      if (im.surface) vkDestroySurfaceKHR(im.instance, im.surface, nullptr);
      vkDestroyInstance(im.instance, nullptr);
      im.instance = VK_NULL_HANDLE;
    }
    return;
  }
  im.WaitIdleAndReset();
  FlushCaches();
  VkDevice dev = im.device;
  im.DestroyHostUi();
  im.resolve_bias.Destroy();
  im.resolve_depth_pack.Destroy();
  im.gamma_ramp.Destroy();
  im.DestroyImage(im.gamma_out);
  im.DestroyImage(im.dc_lut_image);
  im.DestroyImage(im.presented_image);
  im.last_presented = nullptr;
  for (auto& p : im.pipelines) vkDestroyPipeline(dev, p.second, nullptr);
  im.pipelines.clear();
  im.preparation_dirty.InvalidateAll();
  im.prepared_shaders = {};
  im.prepared_viewport = {};
  im.prepared_system = {};
  im.prepared_pipeline = {};
  im.dirty_constant_bindings = {};
  im.last_pipeline_valid = false;
  im.last_pipeline = VK_NULL_HANDLE;
  im.last_pipeline_key = {};
  if (im.debug_vertex_module) vkDestroyShaderModule(dev, im.debug_vertex_module, nullptr);
  if (im.debug_pixel_module) vkDestroyShaderModule(dev, im.debug_pixel_module, nullptr);
  im.debug_vertex_module = im.debug_pixel_module = VK_NULL_HANDLE;
  if (im.vk_pipeline_cache) vkDestroyPipelineCache(dev, im.vk_pipeline_cache, nullptr);
  for (auto& s : im.shaders)
    for (auto& m : s.second.modules) vkDestroyShaderModule(dev, m.second, nullptr);
  im.shaders.clear();
  im.active_vertex_shader = nullptr;
  im.active_pixel_shader = nullptr;
  im.module_sources.clear();
  im.pipeline_warmup.clear();
  im.translator.reset();
  for (auto& l : im.pipeline_layouts) vkDestroyPipelineLayout(dev, l.second, nullptr);
  for (auto& l : im.texture_layouts) vkDestroyDescriptorSetLayout(dev, l.second, nullptr);
  if (im.layout_shared_memory) vkDestroyDescriptorSetLayout(dev, im.layout_shared_memory, nullptr);
  if (im.layout_constants) vkDestroyDescriptorSetLayout(dev, im.layout_constants, nullptr);
  if (im.static_pool) vkDestroyDescriptorPool(dev, im.static_pool, nullptr);
  for (auto& s : im.samplers) vkDestroySampler(dev, s.second, nullptr);
  if (im.dummy_sampler) vkDestroySampler(dev, im.dummy_sampler, nullptr);
  if (im.dummy_view) vkDestroyImageView(dev, im.dummy_view, nullptr);
  im.DestroyImage(im.dummy_texture);
  im.DestroyImage(im.dummy_cube);
  im.DestroyImage(im.dummy_volume);
  for (auto& t : im.textures) {
    for (auto& v : t.second->views) vkDestroyImageView(dev, v.second, nullptr);
    im.DestroyImage(t.second->image);
  }
  im.textures.clear();
  im.texture_binding_cache = {};
  for (auto& t : im.resolve_images) {
    for (auto& v : t.second->views) vkDestroyImageView(dev, v.second, nullptr);
    im.DestroyImage(t.second->image);
  }
  im.resolve_images.clear();
  for (auto& [key, t] : im.resolve_texture_views) {
    for (auto& v : t->views) vkDestroyImageView(dev, v.second, nullptr);
    im.DestroyImage(t->image);
  }
  im.resolve_texture_views.clear();
  im.resolved_by_key.clear();
  im.resolved_by_address.clear();
  im.resolved_by_addr.clear();
  im.display_buffers.clear();
  for (auto& rt : im.color_targets) {
    im.DestroyImage(rt.second.image);
    im.DestroyImage(rt.second.resolved);
  }
  for (auto& rt : im.depth_targets) {
    im.DestroyImage(rt.second.image);
    im.DestroyImage(rt.second.resolved);
  }
  im.color_targets.clear();
  im.depth_targets.clear();
  im.known_depth_clear_tiles.reset();
  im.depth_clear_generation = 0;
  im.DestroyHostBuffer(im.constant_ring.buf);
  im.DestroyHostBuffer(im.index_ring.buf);
  im.resident_index_cache.Reset();
  im.DestroyHostBuffer(im.resident_index_buffer);
  im.stats.resident_index_memory_flags = 0;
  im.DestroyHostBuffer(im.draw_record_ring.buf);
  im.DestroyHostBuffer(im.mirror_upload_ring.buf);
  im.DestroyHostBuffer(im.mirror);
  im.DestroyHostBuffer(im.edram);
  if (im.readback_fence) vkDestroyFence(dev, im.readback_fence, nullptr);
  if (im.readback_pool) vkDestroyCommandPool(dev, im.readback_pool, nullptr);
  im.readback_fence = VK_NULL_HANDLE;
  im.readback_pool = VK_NULL_HANDLE;
  for (FrameContext& f : im.frames) {
    for (HostBuffer& g : f.garbage) im.DestroyHostBuffer(g);
    f.garbage.clear();
    if (f.texture_pool) vkDestroyDescriptorPool(dev, f.texture_pool, nullptr);
    if (f.persistent_texture_pool) vkDestroyDescriptorPool(dev, f.persistent_texture_pool, nullptr);
    if (f.fence) vkDestroyFence(dev, f.fence, nullptr);
    if (f.image_available) vkDestroySemaphore(dev, f.image_available, nullptr);
    if (f.pool) vkDestroyCommandPool(dev, f.pool, nullptr);
    f = FrameContext();
  }
  im.DestroySwapchain();
  im.work_profile.reset();
  if (im.batch_timestamps) vkDestroyQueryPool(dev, im.batch_timestamps, nullptr);
  im.batch_timestamps = VK_NULL_HANDLE;
  vkDestroyDevice(dev, nullptr);
  im.device = VK_NULL_HANDLE;
  if (im.surface) vkDestroySurfaceKHR(im.instance, im.surface, nullptr);
  im.surface = VK_NULL_HANDLE;
  if (im.messenger) {
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(im.instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy) destroy(im.instance, im.messenger, nullptr);
  }
  vkDestroyInstance(im.instance, nullptr);
  im.instance = VK_NULL_HANDLE;
}

bool VulkanRenderer::Impl::InitInstance() {
  std::vector<const char*> extensions;
  if (window) {
    Uint32 count = 0;
    const char* const* sdl_ext = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!sdl_ext) {
      REXLOG_ERROR("xn-vk: SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError());
      return false;
    }
    for (Uint32 i = 0; i < count; ++i) extensions.push_back(sdl_ext[i]);
  }
  std::vector<const char*> layers;
  if (config.enable_validation) {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    layers.push_back("VK_LAYER_KHRONOS_validation");
  }
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "xenon-native";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = uint32_t(extensions.size());
  ci.ppEnabledExtensionNames = extensions.data();
  ci.enabledLayerCount = uint32_t(layers.size());
  ci.ppEnabledLayerNames = layers.data();
  XN_VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));
  if (config.enable_validation) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (create) {
      VkDebugUtilsMessengerCreateInfoEXT mi = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
      mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      mi.pfnUserCallback = DebugCallback;
      create(instance, &mi, nullptr, &messenger);
    }
  }
  return true;
}

bool VulkanRenderer::Impl::PickDevice() {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  VkPhysicalDevice best = VK_NULL_HANDLE;
  int best_score = -1;
  for (VkPhysicalDevice pd : devices) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) continue;
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qprops.data());
    int family = -1;
    for (uint32_t i = 0; i < qcount; ++i) {
      if (!(qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
      if (surface) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
        if (!present) continue;
      }
      family = int(i);
      break;
    }
    if (family < 0) continue;
    int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
    if (score > best_score) {
      best_score = score;
      best = pd;
      queue_family = uint32_t(family);
      queue_timestamp_bits = qprops[family].timestampValidBits;
    }
  }
  if (!best) {
    REXLOG_ERROR("xn-vk: no Vulkan 1.3 device with a graphics{} queue",
                 surface ? "+present" : "");
    return false;
  }
  physical_device = best;
  vkGetPhysicalDeviceProperties(physical_device, &properties);
  vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
  vkGetPhysicalDeviceFeatures(physical_device, &features);
  return true;
}

bool VulkanRenderer::Impl::InitDevice() {
  VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  supported.pNext = &supported12;
  vkGetPhysicalDeviceFeatures2(physical_device, &supported);
  const uint32_t storage_bindings = 1 + (1u << SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
      properties.limits.maxStorageBufferRange));
  if (gpu_draw_constants && (!supported12.bufferDeviceAddress || !features.shaderInt64 ||
      properties.limits.maxPerStageDescriptorStorageBuffers < storage_bindings ||
      properties.limits.maxDescriptorSetStorageBuffers < storage_bindings ||
      properties.limits.maxVertexOutputComponents < 76 ||
      properties.limits.maxFragmentInputComponents < 76)) {
    REXLOG_WARN("xn-vk: GPU draw constants unavailable; using dynamic uniform buffers");
    gpu_draw_constants = multi_draw = false;
  }
  if (multi_draw && (!features.multiDrawIndirect || !features.drawIndirectFirstInstance ||
                     properties.limits.maxDrawIndirectCount < 2)) {
    REXLOG_WARN("xn-vk: multi-draw unavailable; using direct GPU-constant draws");
    multi_draw = false;
  }
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queue_family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;

  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  f13.shaderDemoteToHelperInvocation = VK_TRUE;
  VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f12.pNext = &f13;
  f12.bufferDeviceAddress = gpu_draw_constants;
  // The translator's XeSystemConstants block uses a scalar-style layout.
  f12.uniformBufferStandardLayout = VK_TRUE;
  // Bisect switch: the translator's XeSystemConstants block is scalar-laid-out
  // (user_clip_planes at offset 28), which spirv-val only accepts with this.
  f12.scalarBlockLayout = std::getenv("XN_VK_NO_SCALAR") ? VK_FALSE : VK_TRUE;
  f12.samplerMirrorClampToEdge = VK_TRUE;
  VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f12;
  f2.features.shaderClipDistance = features.shaderClipDistance;
  f2.features.shaderCullDistance = features.shaderCullDistance;
  f2.features.depthClamp = features.depthClamp;
  f2.features.robustBufferAccess = features.robustBufferAccess;
  f2.features.fullDrawIndexUint32 = features.fullDrawIndexUint32;
  f2.features.vertexPipelineStoresAndAtomics = features.vertexPipelineStoresAndAtomics;
  f2.features.fragmentStoresAndAtomics = features.fragmentStoresAndAtomics;
  f2.features.samplerAnisotropy = features.samplerAnisotropy;
  f2.features.independentBlend = features.independentBlend;
  f2.features.textureCompressionBC = features.textureCompressionBC;
  f2.features.sampleRateShading = features.sampleRateShading;
  f2.features.fillModeNonSolid = features.fillModeNonSolid;
  f2.features.pipelineStatisticsQuery = std::getenv("XN_VK_WORK_PROFILE") && std::getenv("XN_VK_WORK_PIPELINE_STATS") && features.pipelineStatisticsQuery;
  f2.features.shaderInt64 = gpu_draw_constants;
  f2.features.multiDrawIndirect = multi_draw;
  f2.features.drawIndirectFirstInstance = multi_draw;

  std::vector<const char*> extensions;
  if (surface) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  if (std::getenv("XN_VK_WORK_PROFILE")) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, available.data());
    const bool advertised = std::any_of(available.begin(), available.end(), [](const auto& item) {
      return std::strcmp(item.extensionName, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0;
    });
    auto domains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
    if (advertised && domains) {
      count = 0;
      if (domains(physical_device, &count, nullptr) == VK_SUCCESS) {
        std::vector<VkTimeDomainEXT> values(count);
        if (domains(physical_device, &count, values.data()) == VK_SUCCESS &&
            std::find(values.begin(), values.end(), VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT) != values.end()) {
          extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
          work_clock_supported = true;
        }
      }
    }
  }

  VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  ci.pNext = &f2;
  ci.queueCreateInfoCount = 1;
  ci.pQueueCreateInfos = &qci;
  ci.enabledExtensionCount = uint32_t(extensions.size());
  ci.ppEnabledExtensionNames = extensions.data();
  XN_VK_CHECK(vkCreateDevice(physical_device, &ci, nullptr, &device));
  vkGetDeviceQueue(device, queue_family, 0, &queue);
  if (work_clock_supported) work_calibrated_clock = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
      vkGetDeviceProcAddr(device, "vkGetCalibratedTimestampsEXT"));
  if (gpu_draw_constants) REXLOG_INFO("xn-vk: GPU-addressed draw constants enabled, multi-draw={}", multi_draw);
  stats.gpu_timestamp_valid_bits = 0;
  if (measure_timings && queue_timestamp_bits && properties.limits.timestampPeriod > 0) {
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = kFramesInFlight * 2;
    if (vkCreateQueryPool(device, &qi, nullptr, &batch_timestamps) == VK_SUCCESS) {
      stats.gpu_timestamp_valid_bits = queue_timestamp_bits;
      REXLOG_INFO("xn-vk: asynchronous GPU batch timing enabled ({} bits, {} ns/tick)",
                  queue_timestamp_bits, properties.limits.timestampPeriod);
    } else {
      REXLOG_WARN("xn-vk: GPU timestamp pool unavailable; CPU timing remains enabled");
    }
  }

  // Depth/stencil format: prefer D24S8, else D32S8.
  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
  depth_stencil_format = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                             ? VK_FORMAT_D24_UNORM_S8_UINT
                             : VK_FORMAT_D32_SFLOAT_S8_UINT;
  vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
  if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
    REXLOG_ERROR("xn-vk: D32S8 is required for guest floating depth targets");
    return false;
  }
  ubo_align = std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 16);
  supported_sample_counts = properties.limits.framebufferColorSampleCounts &
                            properties.limits.framebufferDepthSampleCounts;

  VkPipelineCacheCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  std::vector<uint8_t> cached_pipelines;
  if (!config.pipeline_cache_dir.empty()) {
    std::string uuid;
    for (uint8_t byte : properties.pipelineCacheUUID) uuid += fmt::format("{:02x}",byte);
    pipeline_cache_file = config.pipeline_cache_dir / fmt::format("{}{:04x}-{:04x}-{:08x}-{}.bin",
        gpu_draw_constants ? "gpu-draw-v1-" : "",
        properties.vendorID,properties.deviceID,properties.driverVersion,uuid);
    cached_pipelines = ReadPipelineCache(pipeline_cache_file,properties);
    pci.initialDataSize = cached_pipelines.size();
    pci.pInitialData = cached_pipelines.data();
  }
  VkResult cache_result = vkCreatePipelineCache(device, &pci, nullptr, &vk_pipeline_cache);
  if (cache_result != VK_SUCCESS && !cached_pipelines.empty()) {
    cached_pipelines.clear();
    pci.initialDataSize = 0;
    pci.pInitialData = nullptr;
    vkCreatePipelineCache(device, &pci, nullptr, &vk_pipeline_cache);
  }
  if (!cached_pipelines.empty() && vk_pipeline_cache)
    REXLOG_INFO("xn-vk: loaded pipeline cache ({} bytes)",cached_pipelines.size());
  return true;
}

bool VulkanRenderer::Impl::InitSwapchain() {
  VkSurfaceCapabilitiesKHR caps;
  XN_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &caps));
  uint32_t fcount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &fcount, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(fcount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &fcount, formats.data());
  VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                                   VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                              : formats[0];
  for (const VkSurfaceFormatKHR& f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
      chosen = f;
      break;
    }
  }
  swapchain_format = chosen.format;
  const uint64_t pixel_size = window_pixel_size.load(std::memory_order_relaxed);
  const uint32_t w = uint32_t(pixel_size >> 32), h = uint32_t(pixel_size);
  if (caps.currentExtent.width != 0xFFFFFFFFu) {
    swapchain_extent = caps.currentExtent;
  } else {
    swapchain_extent.width = std::clamp(uint32_t(w), caps.minImageExtent.width,
                                        caps.maxImageExtent.width);
    swapchain_extent.height = std::clamp(uint32_t(h), caps.minImageExtent.height,
                                         caps.maxImageExtent.height);
  }
  const float display_hz = window_display_hz.load(std::memory_order_relaxed);
  uint32_t pcount = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &pcount, nullptr);
  std::vector<VkPresentModeKHR> modes(pcount);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &pcount, modes.data());
  static const bool force_fifo = std::getenv("XN_VK_PRESENT_FIFO") != nullptr;
  const VkPresentModeKHR chosen_mode =
      ChoosePresentMode(config.vsync, target_frame_rate, display_hz, modes, force_fifo);
  // Mailbox needs a spare image to hold the pending frame; with two it
  // degenerates back into a producer stall.
  uint32_t image_count =
      std::max(caps.minImageCount + 1, chosen_mode == VK_PRESENT_MODE_MAILBOX_KHR ? 3u : 2u);
  if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
  VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface;
  ci.minImageCount = image_count;
  ci.imageFormat = chosen.format;
  ci.imageColorSpace = chosen.colorSpace;
  ci.imageExtent = swapchain_extent;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = chosen_mode;
  REXLOG_INFO("xn-vk: presentation target={} display={:.1f}Hz mode={} extent={}x{}",
              target_frame_rate, display_hz, uint32_t(ci.presentMode),
              swapchain_extent.width,swapchain_extent.height);
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = swapchain;
  VkSwapchainKHR new_swapchain;
  XN_VK_CHECK(vkCreateSwapchainKHR(device, &ci, nullptr, &new_swapchain));
  DestroySwapchain();
  swapchain = new_swapchain;
  uint32_t count = 0;
  vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
  swapchain_images.resize(count);
  vkGetSwapchainImagesKHR(device, swapchain, &count, swapchain_images.data());
  swapchain_image_views.resize(count, VK_NULL_HANDLE);
  for (uint32_t i=0;i<count;++i) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image=swapchain_images[i]; view.viewType=VK_IMAGE_VIEW_TYPE_2D;
    view.format=swapchain_format;
    view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    XN_VK_CHECK(vkCreateImageView(device,&view,nullptr,&swapchain_image_views[i]));
  }
  swapchain_present_ready.resize(count, VK_NULL_HANDLE);
  VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (auto& semaphore : swapchain_present_ready)
    XN_VK_CHECK(vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore));
  swapchain_layouts.assign(count, VK_IMAGE_LAYOUT_UNDEFINED);
  swapchain_window_pixel_size = pixel_size;
  swapchain_dirty = false;
  return true;
}

void VulkanRenderer::Impl::DestroySwapchain() {
  for (auto view:swapchain_image_views) if (view) vkDestroyImageView(device,view,nullptr);
  swapchain_image_views.clear();
  for (auto semaphore : swapchain_present_ready)
    if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
  swapchain_present_ready.clear();
  if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
  swapchain = VK_NULL_HANDLE;
  swapchain_images.clear();
  swapchain_layouts.clear();
}

bool VulkanRenderer::Impl::InitFrames() {
  for (FrameContext& f : frames) {
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family;
    XN_VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &f.pool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = f.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    XN_VK_CHECK(vkAllocateCommandBuffers(device, &cai, &f.cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    XN_VK_CHECK(vkCreateFence(device, &fci, nullptr, &f.fence));
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    XN_VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &f.image_available));
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, 16384},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 8192;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    XN_VK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &f.texture_pool));
    XN_VK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &f.persistent_texture_pool));
  }
  {
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family;
    XN_VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &readback_pool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = readback_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    XN_VK_CHECK(vkAllocateCommandBuffers(device, &cai, &readback_cb));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    XN_VK_CHECK(vkCreateFence(device, &fci, nullptr, &readback_fence));
  }
  return true;
}

uint32_t VulkanRenderer::Impl::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required,
                                              VkMemoryPropertyFlags preferred) {
  for (int pass = 0; pass < 2; ++pass) {
    VkMemoryPropertyFlags want = pass == 0 ? (required | preferred) : required;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      if (!(type_bits & (1u << i))) continue;
      if ((memory_properties.memoryTypes[i].propertyFlags & want) == want) return i;
    }
  }
  return UINT32_MAX;
}

bool VulkanRenderer::Impl::CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                            HostBuffer& out, bool prefer_device_local) {
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  XN_VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &out.buffer));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device, out.buffer, &req);
  const VkMemoryPropertyFlags required =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  uint32_t type = FindMemoryType(req.memoryTypeBits, required,
                                 prefer_device_local ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0);
  if (type == UINT32_MAX) {
    REXLOG_ERROR("xn-vk: no host-visible coherent memory type for a {} byte buffer", size);
    return false;
  }
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  VkMemoryAllocateFlagsInfo address_flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    address_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    mai.pNext = &address_flags;
  }
  VkResult alloc_result = vkAllocateMemory(device, &mai, nullptr, &out.memory);
  if (alloc_result != VK_SUCCESS && prefer_device_local) {
    // DEVICE_LOCAL | HOST_VISIBLE is a separate, often small heap (the PCIe BAR
    // window - 256 MB on a card without resizable BAR), shared with every other
    // application on the machine. Preferring it is a performance win when it
    // fits and a hard init failure when it does not, and whether it fits
    // depends on what else is running: this failed with 10 GB of VRAM free
    // because a couple of desktop apps were holding most of the BAR. So when
    // the preferred heap refuses, fall back to ordinary host-visible memory
    // rather than failing to start.
    const uint32_t fallback = FindMemoryType(req.memoryTypeBits, required,
        cached_upload_fallback ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
    if (fallback != UINT32_MAX && fallback != type) {
      REXLOG_WARN("xn-vk: device-local host-visible allocation of {} bytes failed ({}); "
                  "falling back to ordinary host-visible memory",
                  req.size, int(alloc_result));
      mai.memoryTypeIndex = fallback;
      alloc_result = vkAllocateMemory(device, &mai, nullptr, &out.memory);
    }
  }
  XN_VK_CHECK(alloc_result);
  XN_VK_CHECK(vkBindBufferMemory(device, out.buffer, out.memory, 0));
  void* mapped = nullptr;
  XN_VK_CHECK(vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
  out.mapped = static_cast<uint8_t*>(mapped);
  out.size = size;
  out.memory_flags = memory_properties.memoryTypes[mai.memoryTypeIndex].propertyFlags;
  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = out.buffer;
    out.address = vkGetBufferDeviceAddress(device, &info);
    if (!out.address) return false;
  }
  return true;
}

void VulkanRenderer::Impl::DestroyHostBuffer(HostBuffer& buf) {
  if (buf.mapped) vkUnmapMemory(device, buf.memory);
  if (buf.buffer) vkDestroyBuffer(device, buf.buffer, nullptr);
  if (buf.memory) vkFreeMemory(device, buf.memory, nullptr);
  buf = HostBuffer();
}

bool VulkanRenderer::Impl::CreateImage(uint32_t width, uint32_t height, VkFormat format,
                                       VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                       GpuImage& out, VkSampleCountFlagBits samples,
                                       VkImageViewType view_type, uint32_t depth_or_layers,
                                       uint32_t mip_levels) {
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = view_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  if (view_type == VK_IMAGE_VIEW_TYPE_CUBE) ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  // The translator samples signed and unsigned channels through separate
  // bindings of the same guest bytes. Compatible normalized views preserve
  // two's-complement interpretation and filtering before the shader uses them.
  if (SignedSamplingFormat(format) != format) ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  ici.format = format;
  ici.extent = {std::max(width, 1u), std::max(height, 1u),
                ici.imageType == VK_IMAGE_TYPE_3D ? depth_or_layers : 1u};
  ici.mipLevels = std::max(mip_levels, 1u);
  ici.arrayLayers = ici.imageType == VK_IMAGE_TYPE_3D ? 1u : depth_or_layers;
  ici.samples = samples;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  // TRANSFER_DST unconditionally: every image is cleared once below, and a
  // clear is a transfer.
  ici.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  XN_VK_CHECK(vkCreateImage(device, &ici, nullptr, &out.image));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, out.image, &req);
  uint32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
  if (type == UINT32_MAX) type = FindMemoryType(req.memoryTypeBits, 0, 0);
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  XN_VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &out.memory));
  XN_VK_CHECK(vkBindImageMemory(device, out.image, out.memory, 0));
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = out.image;
  vci.viewType = view_type;
  vci.format = format;
  vci.subresourceRange = {aspect, 0, ici.mipLevels, 0, ici.arrayLayers};
  XN_VK_CHECK(vkCreateImageView(device, &vci, nullptr, &out.view));
  if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
    VkImageViewCreateInfo dvci = vci;
    dvci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    XN_VK_CHECK(vkCreateImageView(device, &dvci, nullptr, &out.depth_view));
    dvci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    XN_VK_CHECK(vkCreateImageView(device, &dvci, nullptr, &out.stencil_view));
  }
  out.format = format;
  out.aspect = aspect;
  out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  out.width = ici.extent.width;
  out.height = ici.extent.height;
  out.depth = ici.extent.depth;
  out.layers = ici.arrayLayers;
  out.levels = ici.mipLevels;

  // A fresh Vulkan image has UNDEFINED contents, and this renderer hands images
  // to the guest that may be sampled before every texel has been written - a
  // resolve whose copied rectangle is smaller than its destination is the
  // obvious way in, but it will not be the only one. Sampling undefined memory
  // is never acceptable and its symptoms are driver-dependent, so every image
  // starts at a defined value.
  //
  // XN_VK_GARISH_CLEAR=1 starts them at saturated magenta instead. Undefined
  // texels then announce themselves in the frame instead of hiding behind a
  // plausible black, and the regions that are undefined can be read off the
  // picture rather than inferred from rectangle arithmetic. Keep it: this is
  // how the second title's bloom haze was attributed, and the class recurs.
  // Not for block-compressed formats: vkCmdClearColorImage forbids them, and a
  // compressed image is always fully written by its upload anyway. Not during a
  // readback either, which drives its own command buffer.
  const bool compressed = format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK &&
                          format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
  if (!compressed && !in_readback && frames[frame_index].cb != VK_NULL_HANDLE) {
    static const bool garish = std::getenv("XN_VK_GARISH_CLEAR") != nullptr;
    BeginFrameIfNeeded();
    EndRendering();  // a clear cannot be recorded inside a render pass
    GpuWorkScope profile(WorkProfile(), cb, "image_initial_clear", guest_swap_number,
        0, width, height, 1);
    Transition(out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageSubresourceRange range = {aspect, 0, out.levels, 0, out.layers};
    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
      VkClearColorValue c = {};
      if (garish) {
        c.float32[0] = 1.0f;
        c.float32[1] = 0.0f;
        c.float32[2] = 1.0f;
        c.float32[3] = 1.0f;
      }
      vkCmdClearColorImage(cb, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    } else {
      const VkClearDepthStencilValue d = {1.0f, 0};
      vkCmdClearDepthStencilImage(cb, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &d, 1,
                                  &range);
    }
  }
  return true;
}

void VulkanRenderer::Impl::DestroyImage(GpuImage& img) {
  FlushNativeDraws();
  if (img.image) {
    ++descriptor_prepare_epoch;
    // Vulkan handles may be recycled. Forget every lookup before destroying
    // a view, but keep the sets allocated until their owning fence retires.
    texture_binding_cache = {};
    for (auto& frame : frames) {
      frame.texture_sets.clear();
      frame.texture_pool_stale = true;
    }
  }
  if (img.stencil_view) vkDestroyImageView(device, img.stencil_view, nullptr);
  if (img.depth_view) vkDestroyImageView(device, img.depth_view, nullptr);
  if (img.view) vkDestroyImageView(device, img.view, nullptr);
  if (img.image) vkDestroyImage(device, img.image, nullptr);
  if (img.memory) vkFreeMemory(device, img.memory, nullptr);
  img = GpuImage();
}

bool VulkanRenderer::Impl::InitMirror() {
  mirrored_pages.reset();
  mirror_page_hashes.assign(kMirrorPageCount, 0);
  mirror_write_tokens.assign(kMirrorPageCount, 0);
  // Vertex fetches read device-local storage. Immutable upload snapshots feed
  // it with ordered GPU copies; the CPU never overwrites an in-flight draw.
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = kGuestPhysicalSize;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
              VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  XN_VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &mirror.buffer));
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(device, mirror.buffer, &requirements);
  const uint32_t type = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
  if (type == UINT32_MAX) return false;
  VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = type;
  XN_VK_CHECK(vkAllocateMemory(device, &allocation, nullptr, &mirror.memory));
  XN_VK_CHECK(vkBindBufferMemory(device, mirror.buffer, mirror.memory, 0));
  mirror.size = kGuestPhysicalSize;
  if (!CreateHostBuffer(VkDeviceSize(64) << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        mirror_upload_ring.buf, false)) return false;
  uint32_t log2 = SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
      properties.limits.maxStorageBufferRange);
  mirror_binding_count = 1u << log2;
  mirror_binding_size = kGuestPhysicalSize >> log2;
  return true;
}

void VulkanRenderer::Impl::ObserveDcLutWrite(uint32_t index, uint32_t value) {
  ++dc_lut_writes;
  auto unorm10_to_16 = [](uint32_t v) -> uint16_t {
    return uint16_t(((v & 0x3FF) * 65535u + 511u) / 1023u);
  };
  // XN_DEBUG_GAMMA=<n>: log the first n raw writes (default 48). The whole
  // ramp is ~1700 writes, and 48 reaches only the first six segments - enough
  // to decode the format (low 16 bits base, high 16 bits delta, with
  // base[n+1] == base[n] + delta[n]) but not to see where the curve ends up.
  static const int gamma_log_limit = [] {
    const char* e = std::getenv("XN_DEBUG_GAMMA");
    if (!e) return 0;
    const int n = std::atoi(e);
    return n > 1 ? n : 48;
  }();
  static int logged = 0;
  if (logged < gamma_log_limit) {
    ++logged;
    static const char* const kNames[] = {"RW_MODE", "RW_INDEX", "SEQ_COLOR", "PWL_DATA",
                                         "30_COLOR", "?", "WRITE_EN_MASK"};
    const char* name = (index >= 0x1921 && index <= 0x1927) ? kNames[index - 0x1921] : "OTHER";
    REXLOG_INFO("xn-vk: DC_LUT write {:#x} ({}) = {:#010x}", index, name, value);
  }
  switch (index) {
    case 0x1921:  // DC_LUT_RW_MODE
      dc_lut_rw_mode = value;
      dc_lut_seq_component = 0;
      dc_lut_pwl_component = 0;
      break;
    case 0x1922:  // DC_LUT_RW_INDEX
      dc_lut_rw_index = value & 0xFF;
      dc_lut_seq_component = 0;
      dc_lut_pwl_component = 0;
      break;
    case 0x1923: {  // DC_LUT_SEQ_COLOR - R, G, B in turn, bits 6:15 are the value
      if (dc_lut_write_mask & (1u << (2 - dc_lut_seq_component))) {
        dc_lut[dc_lut_rw_index][dc_lut_seq_component] = unorm10_to_16(value >> 6);
        dc_lut_entries = std::max(dc_lut_entries, dc_lut_rw_index + 1);
        dc_lut_valid = true;
      }
      if (++dc_lut_seq_component >= 3) {
        dc_lut_seq_component = 0;
        dc_lut_rw_index = (dc_lut_rw_index + 1) & 0xFF;
      }
      break;
    }
    case 0x1925: {  // DC_LUT_30_COLOR - 10:10:10 in one write
      for (uint32_t c = 0; c < 3; ++c) {
        if (!(dc_lut_write_mask & (1u << (2 - c)))) continue;
        dc_lut[dc_lut_rw_index][c] = unorm10_to_16(value >> ((2 - c) * 10));
        dc_lut_entries = std::max(dc_lut_entries, dc_lut_rw_index + 1);
        dc_lut_valid = true;
      }
      dc_lut_rw_index = (dc_lut_rw_index + 1) & 0xFF;
      dc_lut_seq_component = 0;
      break;
    }
    case 0x1924: {
      // DC_LUT_PWL_DATA - piecewise linear, three writes per entry (R, G, B).
      // Low 16 bits are the segment's BASE (its output value), high 16 bits the
      // DELTA across the segment. Decoded from the guest's own writes, which
      // satisfy base[n+1] == base[n] + delta[n] exactly - so the base values
      // ARE the curve, sampled once per entry, and the deltas are redundant.
      //
      // This previously stored nothing while still setting dc_lut_valid, so the
      // table read back as all zeros and the diagnostic reported "table
      // captured" for a table it had never filled. The second title drives the
      // ramp exclusively through this path.
      const uint32_t base = value & 0xFFFF;
      const uint32_t entry = dc_lut_rw_index & 0x7F;
      if (dc_lut_write_mask & (1u << (2 - dc_lut_pwl_component))) {
        dc_lut[entry][dc_lut_pwl_component] = uint16_t(base);
        dc_lut_entries = std::max(dc_lut_entries, entry + 1);
        dc_lut_valid = true;
      }
      if (++dc_lut_pwl_component >= 3) {
        dc_lut_pwl_component = 0;
        dc_lut_rw_index = (dc_lut_rw_index & 0x80) | ((entry + 1) & 0x7F);
      }
      break;
    }
    case 0x1927:  // DC_LUT_WRITE_EN_MASK (B, G, R)
      dc_lut_write_mask = value & 7;
      break;
    default:
      break;
  }
}

bool VulkanRenderer::Impl::InitEdram() {
  if (!CreateHostBuffer(EdramSize(),
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, edram,
                        /*prefer_device_local=*/true)) {
    return false;
  }
  std::memset(edram.mapped, 0, size_t(EdramSize()));
  return true;
}

bool VulkanRenderer::Impl::InitRings() {
  if (!CreateHostBuffer(kConstantRingSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
      (gpu_draw_constants ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0), constant_ring.buf,
                        true)) {
    return false;
  }
  if (!CreateHostBuffer(kIndexRingSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_ring.buf, true)) {
    return false;
  }
  // Preserve allocation priority for the larger existing uploads. A record
  // segment holds 16,384 draws and is retired by the same fence as constants.
  if (gpu_draw_constants && !CreateHostBuffer(VkDeviceSize(2) << 20,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      draw_record_ring.buf, true)) return false;
  stats.constant_memory_flags = constant_ring.buf.memory_flags;
  stats.index_memory_flags = index_ring.buf.memory_flags;
  stats.draw_record_memory_flags = draw_record_ring.buf.memory_flags;
  stats.resident_index_memory_flags = 0;
  if (cache_resident_indices) {
    if (gpu_draw_constants || !resident_index_mib) {
      cache_resident_indices = false;
    } else {
      const bool allocated = CreateHostBuffer(VkDeviceSize(resident_index_mib) << 20,
          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, resident_index_buffer, true);
      if (!allocated || !(resident_index_buffer.memory_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        DestroyHostBuffer(resident_index_buffer);
        cache_resident_indices = false;
        REXLOG_WARN("xn-vk: resident index arena unavailable; retaining ordinary index uploads");
      } else {
        resident_index_cache.Reset(resident_index_buffer.size);
        stats.resident_index_memory_flags = resident_index_buffer.memory_flags;
        REXLOG_INFO("xn-vk: immutable resident index arena {} MiB, minimum {} indices",
                    resident_index_mib, resident_index_min_count);
      }
    }
  }
  REXLOG_INFO("xn-vk: upload memory flags: constants={:#x} indices={:#x} draw-records={:#x}",
      stats.constant_memory_flags, stats.index_memory_flags, stats.draw_record_memory_flags);
  return true;
}

bool VulkanRenderer::Impl::InitDescriptors() {
  const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  // Set 0: shared memory (the mirror), split into equal storage buffer bindings.
  {
    VkDescriptorSetLayoutBinding b = {};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b.descriptorCount = mirror_binding_count;
    b.stageFlags = stages;
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &b;
    XN_VK_CHECK(vkCreateDescriptorSetLayout(device, &ci, nullptr, &layout_shared_memory));
  }
  const uint32_t constant_binding_count = gpu_draw_constants ? 1 : SpirvShaderTranslator::kConstantBufferCount;
  // Set 1: either the immutable draw-address table or the original dynamic UBOs.
  {
    VkDescriptorSetLayoutBinding b[SpirvShaderTranslator::kConstantBufferCount] = {};
    for (uint32_t i = 0; i < constant_binding_count; ++i) {
      b[i].binding = i;
      b[i].descriptorType = gpu_draw_constants ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      b[i].descriptorCount = 1;
      b[i].stageFlags = stages;
    }
    if (!gpu_draw_constants) b[SpirvShaderTranslator::kConstantBufferFloatVertex].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    if (!gpu_draw_constants) b[SpirvShaderTranslator::kConstantBufferFloatPixel].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = constant_binding_count;
    ci.pBindings = b;
    XN_VK_CHECK(vkCreateDescriptorSetLayout(device, &ci, nullptr, &layout_constants));
  }
  ubo_sizes[SpirvShaderTranslator::kConstantBufferSystem] =
      sizeof(SpirvShaderTranslator::SystemConstants);
  ubo_sizes[SpirvShaderTranslator::kConstantBufferFloatVertex] = 256 * 16;
  ubo_sizes[SpirvShaderTranslator::kConstantBufferFloatPixel] = 256 * 16;
  ubo_sizes[SpirvShaderTranslator::kConstantBufferBoolLoop] = (8 + 32) * 4;
  ubo_sizes[SpirvShaderTranslator::kConstantBufferFetch] = 32 * 6 * 4;

  VkDescriptorPoolSize sizes[2] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mirror_binding_count + uint32_t(gpu_draw_constants)},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, SpirvShaderTranslator::kConstantBufferCount}};
  VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 2;
  dpi.poolSizeCount = gpu_draw_constants ? 1 : 2;
  dpi.pPoolSizes = sizes;
  XN_VK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &static_pool));
  VkDescriptorSetLayout layouts[2] = {layout_shared_memory, layout_constants};
  VkDescriptorSet sets[2];
  VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = static_pool;
  dai.descriptorSetCount = 2;
  dai.pSetLayouts = layouts;
  XN_VK_CHECK(vkAllocateDescriptorSets(device, &dai, sets));
  set_shared_memory = sets[0];
  set_constants = sets[1];

  std::vector<VkDescriptorBufferInfo> mirror_infos(mirror_binding_count);
  for (uint32_t i = 0; i < mirror_binding_count; ++i) {
    mirror_infos[i] = {mirror.buffer, VkDeviceSize(i) * mirror_binding_size, mirror_binding_size};
  }
  VkDescriptorBufferInfo ubo_infos[SpirvShaderTranslator::kConstantBufferCount];
  VkWriteDescriptorSet writes[1 + SpirvShaderTranslator::kConstantBufferCount] = {};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = set_shared_memory;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = mirror_binding_count;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = mirror_infos.data();
  for (uint32_t i = 0; i < constant_binding_count; ++i) {
    ubo_infos[i] = gpu_draw_constants
        ? VkDescriptorBufferInfo{draw_record_ring.buf.buffer, 0, draw_record_ring.buf.size}
        : VkDescriptorBufferInfo{constant_ring.buf.buffer, 0, ubo_sizes[i]};
    VkWriteDescriptorSet& w = writes[1 + i];
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set_constants;
    w.dstBinding = i;
    w.descriptorCount = 1;
    w.descriptorType = gpu_draw_constants ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w.pBufferInfo = &ubo_infos[i];
  }
  vkUpdateDescriptorSets(device, 1 + constant_binding_count, writes, 0,
                         nullptr);
  return true;
}

bool VulkanRenderer::Impl::InitDummyTexture() {
  if (!CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, dummy_texture)) {
    return false;
  }
  {
    // The translator declares 2D textures as arrayed images.
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = dummy_texture.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = dummy_texture.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    XN_VK_CHECK(vkCreateImageView(device, &vci, nullptr, &dummy_view));
  }
  BeginFrameIfNeeded();
  Transition(dummy_texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  // XN_VK_BLACK_DUMMY=1: makes it obvious when a draw is sampling the dummy
  // rather than the texture it asked for.
  VkClearColorValue white = std::getenv("XN_VK_BLACK_DUMMY") ? VkClearColorValue{{0.f, 0.f, 0.f, 0.f}}
                                                             : VkClearColorValue{{1.f, 1.f, 1.f, 1.f}};
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cb, dummy_texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1,
                       &range);
  Transition(dummy_texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  // A shader may declare both the stacked and volume interpretation of a
  // fetch slot. Even an unused branch needs a dimension-compatible binding.
  for (bool cube : {false, true}) {
    GpuImage& image = cube ? dummy_cube : dummy_volume;
    if (!CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, image, VK_SAMPLE_COUNT_1_BIT,
        cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_3D, cube ? 6 : 1)) return false;
    Transition(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image.layers};
    vkCmdClearColorImage(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &all);
    Transition(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sci.maxLod = 0.0f;
  XN_VK_CHECK(vkCreateSampler(device, &sci, nullptr, &dummy_sampler));
  return true;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
void VulkanRenderer::Impl::RingBeginFrame(RingBuffer& ring) {
  // Each fence owns a fixed segment. Starting after the previous batch's end
  // can wrap into its still-live range when that batch was only partly filled.
  const VkDeviceSize start = frame_index * (ring.buf.size / kFramesInFlight);
  ring.frame_start[frame_index] = start;
  ring.frame_end[frame_index] = start;
  ring.head = start;
}

uint8_t* VulkanRenderer::Impl::RingAllocate(RingBuffer& ring, VkDeviceSize size,
                                            VkDeviceSize align, VkDeviceSize& offset_out) {
  VkDeviceSize offset = (ring.head + align - 1) & ~(align - 1);
  const VkDeviceSize limit = ring.frame_start[frame_index] + ring.buf.size / kFramesInFlight;
  if (offset + size > std::min(limit, ring.buf.size)) {
    return nullptr;
  }
  ring.head = offset + size;
  ring.frame_end[frame_index] = ring.head;
  offset_out = offset;
  return ring.buf.mapped + offset;
}

void VulkanRenderer::Impl::SnapshotPresentedImage(GpuImage* source) {
  last_presented = nullptr;
  if (!source) return;
  // Captures run on the main thread after OnSwap releases the renderer lock.
  // The next guest frame may already have overwritten or destroyed the resolve
  // allocation (MCLA reuses a front buffer for several differently sized passes).
  // Preserve the image at the swap boundary, including its dimensions/format.
  if (presented_image.image && (presented_image.width != source->width ||
      presented_image.height != source->height || presented_image.format != source->format)) {
    frames[frame_index].garbage_images.push_back(presented_image);
    presented_image = {};
  }
  if (!presented_image.image && !CreateImage(source->width, source->height, source->format,
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
          VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, presented_image)) return;
  EndRendering();
  GpuWorkScope profile(WorkProfile(), cb, "present_snapshot", guest_swap_number,
      0, source->width, source->height, 1);
  Transition(*source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  Transition(presented_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageCopy copy = {};
  copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.extent = {source->width, source->height, 1};
  vkCmdCopyImage(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                presented_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  // A resolved front buffer can immediately become a sampled input again.
  Transition(*source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  last_presented = &presented_image;
}

void VulkanRenderer::Impl::CollectBatchTiming(uint32_t index) {
  auto& f = frames[index];
  if (!batch_timestamps || !f.timestamp_pending) return;
  // Every caller already waited for this batch's fence, or for device idle.
  // Deliberately omit WAIT_BIT: diagnostics must never add a synchronization
  // dependency. Consume once, before these query slots are reset/re-recorded.
  f.timestamp_pending = false;
  uint64_t ticks[2]{};
  const VkResult result = vkGetQueryPoolResults(device, batch_timestamps,
      index * 2, 2, sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
  if (result != VK_SUCCESS) {
    REXLOG_WARN("xn-vk: completed batch timestamp results unavailable ({})", int(result));
    return;
  }
  stats.gpu_completed_ns += uint64_t(double(GpuTimestampTicks(
      ticks[0], ticks[1], queue_timestamp_bits)) * properties.limits.timestampPeriod);
  ++stats.gpu_completed_batches;
}

void VulkanRenderer::Impl::BeginFrameIfNeeded() {
  if (frame_open) return;
  ++descriptor_prepare_epoch;
  InvalidateDrawState();
  FrameContext& f = frames[frame_index];
  const uint64_t wait_started = measure_timings ? DiagnosticNanoseconds() : 0;
  vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
  resident_index_cache.Retire(f.submitted_serial);
  if (measure_timings) stats.cpu_fence_wait_ns += DiagnosticNanoseconds() - wait_started;
  CollectBatchTiming(frame_index);
  if (work_profile) work_profile->Collect(frame_index, guest_swap_number);
  dma_index_cache.Reset();
  vkResetFences(device, 1, &f.fence);
  for (HostBuffer& g : f.garbage) DestroyHostBuffer(g);
  f.garbage.clear();
  for (GpuImage& img : f.garbage_images) DestroyImage(img);
  f.garbage_images.clear();

  if (f.texture_pool_stale || f.texture_set_count + 2 > 4096 ||
      f.image_descriptor_count + 64 > 16384 || f.sampler_descriptor_count + 64 > 16384) {
    f.texture_sets.clear();
    f.texture_set_count = f.image_descriptor_count = f.sampler_descriptor_count = 0;
    vkResetDescriptorPool(device, f.persistent_texture_pool, 0);
    f.texture_pool_stale = false;
  }
  vkResetDescriptorPool(device, f.texture_pool, 0);
  vkResetCommandBuffer(f.cb, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(f.cb, &bi);
  cb = f.cb;
  if (batch_timestamps) {
    vkCmdResetQueryPool(cb, batch_timestamps, frame_index * 2, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        batch_timestamps, frame_index * 2);
  }
  frame_open = true;
  if (work_profile) work_profile->BeginBatch(cb, frame_index, frame_number, guest_swap_number);
  if (!in_readback) {
    debug_frame_prev = debug_frame_active;
    debug_frame_active = debug_swap >= 0
        ? uint64_t(debug_swap) == guest_swap_number
        : debug_frame >= 0 && uint64_t(debug_frame) <= frame_number &&
              frame_number < uint64_t(debug_frame) +
                                 uint64_t(debug_frame_count > 0 ? debug_frame_count : 1);
  }
  if (debug_frame_active) debug_frame_first_draw = stats.draws;
  f.submitted = false;
  RingBeginFrame(constant_ring);
  for (auto& cached : constant_upload_cache) cached.Reset();
  ++constant_batch_epoch;
  RingBeginFrame(index_ring);
  if (gpu_draw_constants) RingBeginFrame(draw_record_ring);
  RingBeginFrame(mirror_upload_ring);
  stats.mirror_bytes_copied_frame = 0;
}

void VulkanRenderer::Impl::EndRendering() {
  // Helpers can record their own graphics state even while our normal pass
  // is inactive. Every call is a boundary, including an otherwise empty one.
  InvalidateDrawState();
  if (!rendering_active) return;
  {
    GpuWorkScope tail(WorkProfile(), cb, "attachment_end_and_resolve", guest_swap_number,
        active_color ? active_color->id : 0,
        active_color ? active_color->image.width : 0,
        active_color ? active_color->image.height : 0,
        active_color ? uint32_t(active_color->samples) : 0);
    vkCmdEndRendering(cb);
  }
  if (work_profile && render_work_token) work_profile->End(cb, render_work_token);
  render_work_token = {};
  rendering_active = false;
  // active_color/active_depth are deliberately NOT cleared: they track which
  // target currently holds live pixels, which outlives the render pass. A
  // resolve or a texture upload ends the pass without changing the bound
  // target, and treating that as a target switch would reload the target from
  // EDRAM over content it had just produced.
}

void VulkanRenderer::Impl::TransitionRaw(VkImage image, VkImageAspectFlags aspect,
                                         VkImageLayout old_layout, VkImageLayout new_layout,
                                         uint32_t layers, uint32_t levels) {
  FlushNativeDraws();
  // A repeated read-only layout cannot have an intervening write to this
  // image: every attachment, clear, and copy destination first changes layout.
  // Keep the draw flush above so deferred multi-draw ordering is unchanged.
  if (readonly_barrier_elision && old_layout == new_layout &&
      (new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
       new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) return;
  GpuWorkScope profile(WorkProfile(), cb, "image_barrier", guest_swap_number);
  VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
  b.oldLayout = old_layout;
  b.newLayout = new_layout;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  // GpuImage tracks one layout for the entire image. Uploads and sampled views
  // include the mip tail, so it must participate in every transition too.
  b.subresourceRange = {aspect, 0, levels, 0, layers};
  VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1;
  di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
}

void VulkanRenderer::Impl::Transition(GpuImage& img, VkImageLayout new_layout) {
  if (img.layout == new_layout) {
    // Same layout: still a full barrier so writes become visible.
    TransitionRaw(img.image, img.aspect, img.layout, new_layout, img.layers, img.levels);
    return;
  }
  TransitionRaw(img.image, img.aspect, img.layout, new_layout, img.layers, img.levels);
  img.layout = new_layout;
}

bool VulkanRenderer::Impl::SubmitFrame(bool present, uint32_t swap_image_index) {
  FrameContext& f = frames[frame_index];
  EndRendering();
  if (work_profile) work_profile->EndBatch(cb);
  if (batch_timestamps)
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        batch_timestamps, frame_index * 2 + 1);
  vkEndCommandBuffer(cb);
  frame_open = false;
  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &f.cb;
  if (present) {
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.image_available;
    si.pWaitDstStageMask = &wait_stage;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &swapchain_present_ready[swap_image_index];
  }
  const uint64_t submit_started = measure_timings ? DiagnosticNanoseconds() : 0;
  VkResult r = vkQueueSubmit(queue, 1, &si, f.fence);
  if (measure_timings) stats.cpu_queue_submit_ns += DiagnosticNanoseconds() - submit_started;
  if (r != VK_SUCCESS) {
    REXLOG_ERROR("xn-vk: vkQueueSubmit failed: {}", int(r));
    return false;
  }
  f.submitted = true;
  f.submitted_serial = frame_number + 1;
  f.timestamp_pending = batch_timestamps != VK_NULL_HANDLE;
  if (present) {
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &swapchain_present_ready[swap_image_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &swap_image_index;
    r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
      swapchain_dirty = true;
    } else if (r != VK_SUCCESS) {
      REXLOG_ERROR("xn-vk: vkQueuePresentKHR failed: {}", int(r));
    }
  }
  frame_index = (frame_index + 1) % kFramesInFlight;
  ++frame_number;
  ++stats.frames;
  return true;
}

void VulkanRenderer::Impl::WaitIdleAndReset() {
  if (frame_open) {
    SubmitFrame(false, 0);
  }
  ++stats.device_idle_waits;
  vkDeviceWaitIdle(device);
  resident_index_cache.Retire(frame_number);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) CollectBatchTiming(i);
  if (work_profile) for (uint32_t i = 0; i < kFramesInFlight; ++i) work_profile->Collect(i, guest_swap_number);
  if (work_profile && work_calibrated_clock)
    work_profile->Calibrate(work_calibrated_clock, VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT);
  for (FrameContext& f : frames) {
    for (HostBuffer& g : f.garbage) DestroyHostBuffer(g);
    f.garbage.clear();
    // Images retired by resize/alias replacement may still belong to the
    // final submitted batch, whose fence segment will never be reused during
    // shutdown. Device idle makes their deferred destruction safe now.
    for (GpuImage& image : f.garbage_images) DestroyImage(image);
    f.garbage_images.clear();
  }
}


bool VulkanRenderer::Impl::ReadbackImage(GpuImage& img, std::vector<uint8_t>& rgba8_out,
                                        const VkRect2D* area, std::vector<float>* depth_values,
                                        uint32_t raw_bytes_per_pixel) {
  // Read raw native storage or blit into RGBA8 for a screenshot, copy to a
  // host buffer, wait. Recorded into the renderer's private readback command
  // buffer, NOT the frame in flight: the caller may be the host main thread
  // (CaptureFrame) while the command processor thread is recording draws, and
  // sharing the frame's command buffer meant the pending copy was reset away
  // before it ever executed.
  if (!readback_cb) return false;
  if (depth_values) depth_values->clear();
  const bool is_depth = (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
  if (raw_bytes_per_pixel && (is_depth || raw_bytes_per_pixel > 16)) return false;
  const bool convert_color = !is_depth && !raw_bytes_per_pixel;
  const uint32_t x = area ? uint32_t(std::max(0, area->offset.x)) : 0;
  const uint32_t y = area ? uint32_t(std::max(0, area->offset.y)) : 0;
  if (x >= img.width || y >= img.height) return false;
  const uint32_t width = area ? std::min(area->extent.width, img.width - x) : img.width;
  const uint32_t height = area ? std::min(area->extent.height, img.height - y) : img.height;
  if (!width || !height) return false;
  in_readback = true;
  struct ReadbackGuard {
    bool& flag;
    ~ReadbackGuard() { flag = false; }
  } readback_guard{in_readback};
  // Flush whatever the current frame has recorded so the source image holds
  // everything drawn so far, and let the GPU finish it.
  if (frame_open) SubmitFrame(false, 0);
  vkDeviceWaitIdle(device);
  resident_index_cache.Retire(frame_number);
  if (work_profile) for (uint32_t i = 0; i < kFramesInFlight; ++i) work_profile->Collect(i, guest_swap_number);
  if (work_profile && work_calibrated_clock)
    work_profile->Calibrate(work_calibrated_clock, VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT);

  GpuImage tmp;
  if (convert_color && !CreateImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, tmp)) {
    return false;
  }
  HostBuffer staging;
  const VkDeviceSize size = VkDeviceSize(width) * height * (raw_bytes_per_pixel ? raw_bytes_per_pixel : 4);
  if (!CreateHostBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, staging)) {
    DestroyImage(tmp);
    return false;
  }
  std::memset(staging.mapped, 0, size_t(size));

  // Record on the private command buffer. Transition() and friends record
  // into `cb`, so point it at the readback buffer for the duration.
  VkCommandBuffer saved_cb = cb;
  cb = readback_cb;
  vkResetCommandBuffer(readback_cb, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(readback_cb, &bi);

  const VkImageLayout old_layout = img.layout;
  Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  if (convert_color) {
    Transition(tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {};
    blit.srcSubresource = {img.aspect, 0, 0, 1};
    blit.srcOffsets[0] = {int32_t(x), int32_t(y), 0};
    blit.srcOffsets[1] = {int32_t(x + width), int32_t(y + height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(width), int32_t(height), 1};
    vkCmdBlitImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
  }
  if (convert_color) Transition(tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy region = {};
  region.imageSubresource = {is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  if (!convert_color) region.imageOffset = {int32_t(x), int32_t(y), 0};
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(cb, convert_color ? tmp.image : img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1,
                         &region);
  if (old_layout != VK_IMAGE_LAYOUT_UNDEFINED) Transition(img, old_layout);
  vkEndCommandBuffer(readback_cb);
  cb = saved_cb;

  vkResetFences(device, 1, &readback_fence);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &readback_cb;
  VkResult r = vkQueueSubmit(queue, 1, &si, readback_fence);
  if (r == VK_SUCCESS) {
    vkWaitForFences(device, 1, &readback_fence, VK_TRUE, UINT64_MAX);
    if (is_depth) {
      rgba8_out.resize(size);
      for (size_t i = 0; i < size_t(width) * height; ++i) {
        uint32_t raw;
        std::memcpy(&raw, staging.mapped + i * 4, 4);
        float value;
        if (img.format == VK_FORMAT_D24_UNORM_S8_UINT)
          value = float(raw & 0xFFFFFF) / 16777215.0f;
        else std::memcpy(&value, &raw, 4); // D32_SFLOAT_S8_UINT depth aspect
        if (depth_values) depth_values->push_back(value);
        const uint8_t grey = uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        rgba8_out[i * 4] = rgba8_out[i * 4 + 1] = rgba8_out[i * 4 + 2] = grey;
        rgba8_out[i * 4 + 3] = 255;
      }
    } else rgba8_out.assign(staging.mapped, staging.mapped + size);
  } else {
    REXLOG_ERROR("xn-vk: readback submit failed: {}", int(r));
  }
  DestroyHostBuffer(staging);
  DestroyImage(tmp);
  return r == VK_SUCCESS;
}

bool VulkanRenderer::DebugReadbackDepthPixel(uint32_t edram_base, uint32_t x, uint32_t y,
                                           float& depth_out) {
  XN_LOCK();
  Impl& im = *impl_;
  RenderTargetImage* target = im.active_depth;
  if (!target || target->edram_base != edram_base) {
    target = nullptr;
    for (auto& [key, candidate] : im.depth_targets)
      if (candidate.edram_base == edram_base && x < candidate.image.width &&
          y < candidate.image.height) target = &candidate;
  }
  if (!target || x >= target->image.width || y >= target->image.height) return false;
  const VkRect2D point{{int32_t(x), int32_t(y)}, {1, 1}};
  std::vector<uint8_t> rgba;
  std::vector<float> values;
  if (!im.ReadbackImage(im.ReadableImage(*target), rgba, &point, &values) || values.empty())
    return false;
  depth_out = values[0];
  return true;
}

bool VulkanRenderer::DebugReadbackGuestTexture(const GpuRegisterFile& registers,
                                               uint32_t fetch_slot, std::vector<uint8_t>& out,
                                               bool signed_view) {
  XN_LOCK();
  if (fetch_slot >= 32) return false;
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, registers.values + 0x4800 + fetch_slot * 6, sizeof(fetch));
  Impl& im = *impl_;
  im.BeginFrameIfNeeded();
  TextureEntry* entry = im.GetTexture(fetch);
  if (!entry || entry->unsupported) return false;
  if (entry->image.layers != 1 || !im.resolve_bias.Init(im.device)) return false;
  const auto view = im.GetTextureView(*entry, fetch.swizzle, signed_view, VK_IMAGE_VIEW_TYPE_2D);
  GpuImage sampled;
  if (!im.CreateImage(entry->image.width, entry->image.height, VK_FORMAT_R8G8B8A8_UNORM,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT, sampled)) return false;
  im.EndRendering();
  im.Transition(entry->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  im.Transition(sampled, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  const bool drew = im.resolve_bias.Apply(im.cb, im.frames[im.frame_index].texture_pool,
      view, sampled.view, sampled.format, 0, 0, 0, 0, sampled.width, sampled.height, 1.f);
  const bool ok = drew && im.ReadbackImage(sampled, out);
  im.DestroyImage(sampled);
  return ok;
}

bool VulkanRenderer::DebugReadbackColorTarget(uint32_t edram_base, std::vector<uint8_t>& out,
                                              uint32_t& w, uint32_t& h) {
  XN_LOCK();
  Impl& im = *impl_;
  // Preserve the largest-view convention, preferring the active view on ties.
  // Equal-sized 1x and MSAA views must not depend on unordered-map iteration.
  RenderTargetImage* best = nullptr;
  for (auto& rt : im.color_targets) {
    if (rt.second.edram_base != edram_base) continue;
    const uint32_t area = rt.second.image.width * rt.second.image.height;
    if (!best || area > best->image.width * best->image.height ||
        (area == best->image.width * best->image.height && &rt.second == im.active_color)) {
      best = &rt.second;
    }
  }
  if (!best) return false;
  GpuImage& readable = im.ReadableImage(*best);
  w = readable.width;
  h = readable.height;
  return im.ReadbackImage(readable, out);
}

bool VulkanRenderer::DebugReadbackResolvedTexture(uint32_t guest_address,
                                                  std::vector<uint8_t>& out, uint32_t& w,
                                                  uint32_t& h) {
  XN_LOCK();
  Impl& im = *impl_;
  TextureEntry* target = nullptr;
  for (const auto& [rkey, entry] : im.resolved_by_key) {
    if (rkey.address == guest_address) {
      const bool is_depth = (rkey.format == 22 || rkey.format == 23);
      if (!is_depth) {
        target = entry;
        break;
      }
      if (!target) target = entry;
    }
  }
  if (!target) {
    auto it = im.resolved_by_address.find(guest_address);
    if (it != im.resolved_by_address.end()) target = it->second;
  }
  if (!target) return false;
  w = target->image.width;
  h = target->image.height;
  return im.ReadbackImage(target->image, out);
}

}  // namespace xn::gfx

namespace xn::gfx {
namespace {
void (*g_forward_sink)(int, const char*) = nullptr;
void ForwardingSink(xn::log::Level level, const std::string& message) {
  if (g_forward_sink) g_forward_sink(int(level), message.c_str());
}
}  // namespace
void SetRendererLogSink(void (*sink)(int level, const char* message)) {
  g_forward_sink = sink;
  xn::log::SetSink(sink ? ForwardingSink : nullptr);
}

bool VulkanRenderer::Impl::DumpFrame(GpuImage& source) {
  if (std::getenv("XN_DEBUG_READBACK")) {
    // Which image actually holds the frame: the resolved front buffer we
    // present, or the colour target it was resolved from?
    std::vector<uint8_t> probe;
    auto max_of = [&](GpuImage& img, const char* what) {
      if (!ReadbackImage(img, probe)) {
        REXLOG_INFO("xn-vk: probe {} readback FAILED", what);
        return;
      }
      uint8_t m = 0;
      for (uint8_t v : probe) m = std::max(m, v);
      // Per-channel means including ALPHA: shader EC8F's composite blends
      // src + dst * (1 - srcAlpha), so the alpha the resolved bloom level
      // carries decides how much of the destination survives.
      double ch[4] = {0, 0, 0, 0};
      // pow() is concave, so the mean of the composite's output is NOT a
      // function of the level's mean: a level with a low floor everywhere and
      // one with the same mean concentrated in bright spots add very different
      // amounts. "tone" is the composite's own arithmetic - the mean of
      // pow(c, 0.65) * 0.5 - which is the number that decides the lift, and
      // "black" is the fraction of pixels that are essentially off.
      double tone[3] = {0, 0, 0};
      uint64_t px_n = 0, black_n = 0;
      for (size_t i = 0; i + 3 < probe.size(); i += 4) {
        for (int c = 0; c < 4; ++c) ch[c] += probe[i + c];
        for (int c = 0; c < 3; ++c) {
          tone[c] += std::pow(double(probe[i + c]) / 255.0, 0.65) * 0.5 * 255.0;
        }
        if (probe[i] < 8 && probe[i + 1] < 8 && probe[i + 2] < 8) ++black_n;
        ++px_n;
      }
      REXLOG_INFO(
          "xn-vk: probe {} {}x{} layout={} max={} mean rgba {:.2f} {:.2f} {:.2f} {:.2f} "
          "tone {:.2f} {:.2f} {:.2f} black {:.3f}",
          what, img.width, img.height, int(img.layout), m, px_n ? ch[0] / px_n : 0.0,
          px_n ? ch[1] / px_n : 0.0, px_n ? ch[2] / px_n : 0.0, px_n ? ch[3] / px_n : 0.0,
          px_n ? tone[0] / px_n : 0.0, px_n ? tone[1] / px_n : 0.0, px_n ? tone[2] / px_n : 0.0,
          px_n ? double(black_n) / double(px_n) : 0.0);
      if (!config.frame_dump_dir.empty()) {
        std::filesystem::create_directories(config.frame_dump_dir);
        char name[96];
        // RAW BYTE ORDER WARNING, in the filename so it cannot be missed.
        // Render targets are created as R8G8B8A8_UNORM and their bytes are RGB.
        // RESOLVED targets carry the guest copy format's host_swizzle, applied
        // by the image view the shader samples through - so their raw bytes are
        // NOT in the same order, and dumping byte 0,1,2 transposes R and B
        // relative to a colour target. Comparing the two dumps directly reads
        // as a channel swap that does not exist in the rendering.
        std::snprintf(name, sizeof(name), "rt_f%06llu_%s_%ux%u%s.ppm",
                      (unsigned long long)frame_number, what, img.width, img.height,
                      std::strstr(what, "resolved") ? "_RAWBYTES_swizzle_not_applied" : "");
        if (FILE* f = std::fopen((config.frame_dump_dir / name).string().c_str(), "wb")) {
          std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
          for (size_t i = 0; i + 3 < probe.size(); i += 4) std::fwrite(probe.data() + i, 1, 3, f);
          std::fclose(f);
        }
      }
    };
    REXLOG_INFO("xn-vk: stats frame={} draws={} skipped={} pipelines={} textures={} resolves={} "
                "clears={} frame_open={} frame_index={} render_pass_starts={}",
                frame_number, stats.draws, stats.draws_skipped, stats.pipelines,
                stats.textures_loaded, stats.resolves, stats.clears, frame_open, frame_index,
                render_pass_starts_);
    if (std::getenv("XN_DEBUG_TRI")) {
      // Does ANY rendering land in the colour target at this point?
      DebugTriangleIntoCurrentTarget();
    }
    max_of(source, "present-source");
    for (auto& rt : color_targets) {
      max_of(ReadableImage(rt.second), "colour-target");
    }
    for (auto& r : resolved_by_address) {
      max_of(r.second->image, fmt::format("resolved_{:08X}", r.first).c_str());
    }
    for (auto& t : textures) {
      TextureEntry& e = *t.second;
      if (e.resolved || !e.image.image) continue;
      uint8_t guest_max = 0;
      if (guest_base && e.guest_size) {
        const uint8_t* p = guest_base + e.key.base_address;
        for (uint32_t i = 0; i < e.guest_size; ++i) guest_max = std::max(guest_max, p[i]);
      }
      std::vector<uint8_t> px;
      uint8_t img_max = 0;
      if (ReadbackImage(e.image, px)) {
        for (uint8_t v : px) img_max = std::max(img_max, v);
      }
      REXLOG_INFO("xn-vk: probe texture {:#x} {}x{} guest_max={} image_max={} reuploads={}",
                  e.key.base_address, e.key.width, e.key.height, guest_max, img_max,
                  stats.textures_reuploaded);
      if (!config.frame_dump_dir.empty() && guest_base && e.guest_size &&
          std::getenv("XN_DEBUG_DUMP_RAW")) {
        std::filesystem::create_directories(config.frame_dump_dir);
        char rawname[96];
        std::snprintf(rawname, sizeof(rawname), "raw_%08X_%ux%u_p%u_t%u_e%u.bin",
                      e.key.base_address, e.key.width, e.key.height, e.key.pitch, e.key.tiled,
                      e.key.endian);
        if (FILE* f = std::fopen((config.frame_dump_dir / rawname).string().c_str(), "wb")) {
          std::fwrite(guest_base + e.key.base_address, 1, e.guest_size, f);
          std::fclose(f);
        }
      }
      if (!px.empty() && !config.frame_dump_dir.empty() &&
          std::getenv("XN_DEBUG_DUMP_TEX_PPM")) {
        std::filesystem::create_directories(config.frame_dump_dir);
        char name[96];
        std::snprintf(name, sizeof(name), "tex_f%06llu_%08X_%ux%u.ppm",
                      (unsigned long long)frame_number, e.key.base_address, e.image.width,
                      e.image.height);
        if (FILE* f = std::fopen((config.frame_dump_dir / name).string().c_str(), "wb")) {
          std::fprintf(f, "P6\n%u %u\n255\n", e.image.width, e.image.height);
          for (size_t i = 0; i < size_t(e.image.width) * e.image.height; ++i) {
            std::fwrite(px.data() + i * 4, 1, 3, f);
          }
          std::fclose(f);
        }
      }
    }
  }
  std::vector<uint8_t> rgba;
  const uint32_t w = source.width, h = source.height;
  if (!ReadbackImage(source, rgba)) return false;
  std::filesystem::create_directories(config.frame_dump_dir);
  char name[64];
  std::snprintf(name, sizeof(name), "frame_%06llu.ppm", (unsigned long long)frame_number);
  FILE* f = std::fopen((config.frame_dump_dir / name).string().c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%u %u\n255\n", w, h);
  std::vector<uint8_t> rgb(size_t(w) * h * 3);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
  }
  std::fwrite(rgb.data(), 1, rgb.size(), f);
  std::fclose(f);
  REXLOG_INFO("xn-vk: wrote {} ({}x{})", name, w, h);
  return true;
}
}  // namespace xn::gfx

namespace xn {
IRenderer* CreateVulkanRenderer() {
  gfx::VulkanRendererConfig config;
  if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache && *cache)
    config.pipeline_cache_dir = std::filesystem::path(cache) / "xenon-native" / "vulkan";
  else if (const char* user_home = std::getenv("HOME"); user_home && *user_home)
    config.pipeline_cache_dir = std::filesystem::path(user_home) / ".cache" / "xenon-native" / "vulkan";
  return CreateVulkanRenderer(config);
}
IRenderer* CreateVulkanRenderer(gfx::VulkanRendererConfig config) {
  if (const char* v = std::getenv("XN_VK_PIPELINE_CACHE"))
    config.pipeline_cache_dir = std::strcmp(v,"0") ? v : "";
  if (const char* v = std::getenv("XN_VK_VALIDATION")) config.enable_validation = v[0] == '1';
  if (const char* v = std::getenv("XN_VK_NO_VSYNC")) config.vsync = v[0] != '1';
  if (std::getenv("XN_VK_NO_MIRROR_DEDUPE")) config.mirror_dedupe_per_frame = false;
  if (const char* v = std::getenv("XN_FRAME_DUMP")) config.frame_dump_dir = v;
  if (const char* v = std::getenv("XN_FRAME_DUMP_EVERY")) {
    config.frame_dump_every = std::max(1, std::atoi(v));
  }
  if (const char* v = std::getenv("XN_FRAME_DUMP_START")) {
    config.frame_dump_start = std::strtoull(v, nullptr, 10);
  }
  if (const char* v = std::getenv("XN_SHADER_DUMP")) config.shader_dump_dir = v;
  if (const char* trace = std::getenv("XN_RENDER_TRACE"); trace && *trace)
    return CreateRenderCapture(std::move(config));
  return new gfx::VulkanRenderer(config);
}
}  // namespace xn
