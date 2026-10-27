/**
 ******************************************************************************
 * xenon-native - runtime/gfx/vulkan/shape_a.h
 ******************************************************************************
 *
 * "Shape A" D3D9 entry-point contract for the Vulkan renderer (docs/RENDERER.md).
 *
 * The runtime's primary feed is the PM4 ring (xn::IRenderer in gfx/renderer.h:
 * the command processor materialises the Xenos register file and reports
 * draws). Shape A is the second, hook-level feed: the game's statically linked
 * D3D9 keeps every register it is about to submit in one flat device struct,
 * so a hook on the few SUBMITTING entries (draw, clear, resolve, swap) can hand
 * the renderer a DeviceShadow - a pointer to that struct plus the verified
 * byte offsets for the title (titles/<title>/device_layout.toml) - and the
 * renderer decodes Xenos register state straight from it. Both feeds end in
 * the same register-file -> Vulkan path.
 */
#pragma once
#include "draw_batch_profile.h"
#include "edram_layout.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xn { struct GpuRegisterFile; }

namespace rex::graphics {
class RegisterFile;
}

namespace xn::gfx {

// ---------------------------------------------------------------------------
// Device struct layout (per title, from device_layout.toml).
// ---------------------------------------------------------------------------
struct DeviceLayout {
  // Register shadow blocks: a run of `count` Xenos registers starting at
  // `register_base` lives at `shadow_offset` inside the device struct, one
  // dword per register, in the register-file order.
  struct RegisterBlock {
    uint32_t register_base = 0;
    uint32_t shadow_offset = 0;
    uint32_t count = 0;
  };
  // Vertex ALU constants c0..c255 (256 float4, register 0x4000..) and pixel
  // ALU constants (register 0x4400..): contiguous float4 arrays.
  uint32_t vs_alu_constants = 1920;
  uint32_t ps_alu_constants = 6016;
  // 96 fetch slots x 2 dwords (register 0x4800..): vertex fetch slot n is
  // dwords 2n..2n+1, texture fetch constant t is slots 3t..3t+2.
  uint32_t fetch_constants = 1152;
  // 8 dwords of bool constants (0x4900) immediately followed by 32 loop
  // constants (0x4908).
  uint32_t bool_loop_constants = 10112;
  std::vector<RegisterBlock> register_blocks;
  // The device struct is guest memory: big-endian dwords.
  bool big_endian = true;
  // Total size, for bounds checks / debug dumps only.
  uint32_t size = 22288;

  // The PGR4 / Split/Second / RRU layout: the SDK build every one of those
  // titles carries. Geometry Wars is expected to match; the toml says so.
  static DeviceLayout Default();
  static bool LoadToml(const std::filesystem::path& path, DeviceLayout& out,
                       std::string* error);
};

// A view over the game's device struct. Cheap to construct per call.
struct DeviceShadow {
  const uint8_t* base = nullptr;
  const DeviceLayout* layout = nullptr;

  // Reads one dword at a byte offset, honouring layout->big_endian.
  uint32_t Dword(uint32_t byte_offset) const;
  // Reads a Xenos register through the shadow blocks; 0 when the register is
  // not shadowed (and *found = false when the pointer is given).
  uint32_t Register(uint32_t register_index, bool* found = nullptr) const;
  // Materialises the whole thing into a host-endian Xenos register file:
  // shadow blocks, ALU / bool / loop / fetch constants. The translator's
  // constant-buffer ABI is exactly this register file's layout.
  void ExportRegisters(rex::graphics::RegisterFile& regs) const;
};

// A guest shader: the Xenos microcode the D3D9 shader object points at.
struct ShaderRef {
  const uint32_t* ucode = nullptr;  // host pointer to the guest microcode
  uint32_t dword_count = 0;
  bool big_endian = true;  // guest memory: big-endian dwords
  // Optional. 0 = the renderer computes XXH3 over the guest (big-endian)
  // bytes, which is also how the --dump_shaders files are named.
  uint64_t hash = 0;
};

// What the D3D9 draw entry supplies beyond the register shadow: the
// primitive, the index source and the shaders.
struct DrawParams {
  uint32_t primitive_type = 4;  // xenos::PrimitiveType (4 = triangle list)
  uint32_t index_count = 0;     // number of indices (or vertices if !indexed)
  bool indexed = false;
  uint32_t index_base_guest = 0;  // physical byte address of the first index
  uint32_t index_format = 0;      // 0 = 16-bit, 1 = 32-bit (xenos::IndexFormat)
  uint32_t index_endian = 1;      // xenos::Endian; D3D9 index buffers: 8in16
  // Host pointer to index data overriding index_base_guest (immediate
  // indices in a PM4 DRAW_INDX_2); nullptr = read guest memory.
  const uint8_t* index_host_data = nullptr;
  ShaderRef vertex_shader;
  ShaderRef pixel_shader;  // ucode == nullptr for a depth-only draw
};

struct ClearParams {
  enum : uint32_t { kColor = 1, kDepth = 2, kStencil = 4 };
  uint32_t flags = kColor;
  uint32_t color_target_mask = 0b0001;  // which of the 4 colour targets
  float color[4] = {0.f, 0.f, 0.f, 0.f};
  float depth = 1.0f;
  uint32_t stencil = 0;
  // Sub-rectangle in render-target pixels; width == 0 means the whole target.
  uint32_t x = 0, y = 0, width = 0, height = 0;
};

// Surface description as the D3D9 SetRenderTarget / SetDepthStencilSurface
// entries see it. edram_base is the tile address the corresponding
// RB_COLOR_INFO.color_base / RB_DEPTH_INFO.depth_base will carry at draw time;
// with the width it is the key the renderer uses to pick the VkImage (the
// register shadow carries the pitch but not the height).
struct SurfaceDesc {
  uint32_t edram_base = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;  // xenos::ColorRenderTargetFormat or DepthRenderTargetFormat
  uint32_t msaa_samples = 0;  // xenos::MsaaSamples (0 = 1x)
};

struct ResolveParams {
  // Source: colour target index 0..3, or kDepth.
  static constexpr int32_t kDepth = -1;
  int32_t source = 0;
  // Source rectangle in render-target pixels; width == 0 = whole target.
  uint32_t src_x = 0, src_y = 0, src_width = 0, src_height = 0;
  // Destination guest texture (physical byte address of level 0) and its
  // shape. dest_format is a xenos::TextureFormat, dest_endian a xenos::Endian.
  uint32_t dest_guest_address = 0;
  uint32_t dest_width = 0, dest_height = 0;
  uint32_t dest_pitch = 0;  // in pixels; 0 = dest_width
  uint32_t dest_format = 6;  // k_8_8_8_8
  uint32_t dest_endian = 2;  // 8in32
  uint32_t dest_number = 0;  // xenos::SurfaceNumberFormat
  bool dest_tiled = true;
  uint32_t dest_x = 0, dest_y = 0;
  bool copy = true;  // false: clear only (CopyCommand kConstantOne / kNull)
  // When true, everything above (except an explicit source rect) is ignored
  // and RB_COPY_CONTROL / RB_COPY_DEST_* / RB_*_CLEAR in the register file are
  // used instead - the PM4 ring path.
  bool from_copy_registers = false;
  // D3D9 Resolve may clear the source after copying.
  bool clear_after = false;
  ClearParams clear;
};

struct PresentParams {
  // The front buffer the game swaps: physical address of the resolved texture
  // (VdSwap's frontbuffer pointer), and its shape.
  uint32_t frontbuffer_guest_address = 0;
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t format = 6;  // xenos::TextureFormat
};

// Optional title-owned, host-decoded texture data. Revisions let the upload
// cache refresh independently of guest writes, including repeated frame data.
class TextureUploadSource {
 public:
  virtual ~TextureUploadSource() = default;
  virtual void OnDraw(uint64_t pixel_shader, const xn::GpuRegisterFile& registers) = 0;
  virtual uint64_t Revision(uint32_t address, uint32_t width, uint32_t height, uint32_t format) const = 0;
  virtual bool Read(uint32_t address, uint32_t width, uint32_t height, uint32_t format,
                    std::vector<uint8_t>& pixels) const = 0;
};

struct VulkanRendererConfig {
  std::shared_ptr<TextureUploadSource> texture_source;
  // Optional capture seam: prepare physical guest bytes before a CPU read or
  // its write-token/cache validation. The callback runs on the renderer thread
  // and may populate an owned input image; it must not reenter the renderer.
  std::function<void(uint32_t address,uint32_t size)> prepare_guest_read;
  bool enable_validation = false;
  bool vsync = true;
  // Title opt-in after exact replay and live validation. The environment
  // override XN_VK_DIRTY_DRAW_PREPARATION=0/1 takes precedence.
  bool dirty_draw_preparation = false;
  // Virtual EDRAM capacity: 10 MiB by default; 20 or 40 MiB by title opt-in.
  // XN_VK_VIRTUAL_EDRAM=0/1/40 forces 10/20/40 MiB respectively.
  uint32_t virtual_edram_mib = 10;
  // Height assumed for a render target whose surface was never registered via
  // SetRenderTarget (the register shadow only carries the pitch).
  uint32_t fallback_render_target_height = 720;
  // Skip re-copying a guest range into the mirror when an earlier draw of the
  // same frame already copied it (docs/RENDERER.md, "guest memory mirror").
  bool mirror_dedupe_per_frame = true;
  std::filesystem::path shader_dump_dir;  // empty = no dump
  // Optional driver pipeline cache, loaded at startup and saved at shutdown.
  std::filesystem::path pipeline_cache_dir;
  // Write every frame_dump_every-th presented frame as PPM into this dir.
  std::filesystem::path frame_dump_dir;
  uint32_t frame_dump_every = 60;
  uint64_t frame_dump_start = 0;
};

struct RendererStats {
  uint32_t edram_tile_capacity = kDefaultEdramTileCapacity;
  uint64_t frames = 0, draws = 0, draws_skipped = 0, clears = 0, resolves = 0;
  uint64_t pipelines = 0, shaders_translated = 0, textures_loaded = 0, textures_reuploaded = 0;
  uint64_t mirror_bytes_copied_frame = 0;
  // Guest bytes fed through the mirror's change-detection hash (may exceed the
  // bytes copied: unchanged pages are hashed and skipped).
  uint64_t mirror_bytes_hashed = 0;
  // Draws whose vertex uploads were narrowed by the direct-fetch/index-range
  // proof (only the proven range was considered, not the advertised buffer).
  uint64_t vertex_ranges_bounded = 0;
  // Resolve storage is retained by guest address and complete texture layout.
  uint64_t resolve_images_created = 0;
  uint64_t resolve_images_reused = 0;
  uint64_t texture_sets_allocated = 0, texture_sets_reused = 0;
  uint64_t texture_dirty_hits = 0, texture_dirty_misses = 0;
  uint64_t gpu_constant_draws = 0, multi_draw_calls = 0, multi_draw_draws = 0;
  uint64_t multi_draw_calls_saved = 0;
  uint32_t constant_memory_flags = 0, index_memory_flags = 0, draw_record_memory_flags = 0;
  // Full-device waits (vkDeviceWaitIdle outside shutdown/readback); layout
  // changes that reuse a cached resolve must not add one.
  uint64_t device_idle_waits = 0;
  uint64_t depth_clear_plans_built = 0, depth_clear_plans_reused = 0;
  uint64_t pipelines_prewarmed = 0;
  uint64_t constant_upload_bytes = 0, constant_uploads_reused = 0;
  uint64_t float_constant_uploads_reused = 0, float_constant_bytes_avoided = 0;
  uint64_t index_cache_hits = 0, index_cache_misses = 0;
  uint64_t converted_bytes_avoided = 0, index_ring_bytes_avoided = 0;
  // Completed-batch measurements lag the current guest frame by the number
  // of batches in flight. GPU time includes the batch's queue execution span,
  // not CPU command recording or queue-idle time before it begins.
  uint64_t gpu_completed_ns = 0, gpu_completed_batches = 0;
  // CPU wall spans of batch fence waits and vkQueueSubmit only; dedicated
  // texture/screenshot readbacks and vkQueuePresent are separate operations.
  uint64_t cpu_fence_wait_ns = 0, cpu_queue_submit_ns = 0;
  uint32_t gpu_timestamp_valid_bits = 0;  // zero when disabled/unsupported
  uint64_t draw_state_commands_saved = 0;
  uint64_t fused_draw_callbacks = 0;
  uint64_t resident_index_hits = 0, resident_index_misses = 0;
  uint64_t resident_index_upload_bytes = 0, resident_index_bytes_reused = 0;
  uint64_t resident_index_fallbacks = 0;
  uint32_t resident_index_memory_flags = 0;
  uint64_t prepared_shader_lookups_avoided = 0, prepared_pipeline_lookups_avoided = 0;
  uint64_t preparation_shader_hits = 0, preparation_viewport_hits = 0;
  uint64_t preparation_system_hits = 0, preparation_pipeline_hits = 0;
  uint64_t dirty_constant_uploads_reused = 0, float_constant_packs_avoided = 0;
  uint64_t draw_state_pipeline_binds_saved = 0, draw_state_texture_binds_saved = 0;
  DrawBatchProfileStats draw_batch_profile;
  DrawBatchProfileStats instance_profile;  // profiler-only instance opportunity
};

}  // namespace xn::gfx
