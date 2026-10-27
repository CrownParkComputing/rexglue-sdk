/**
 ******************************************************************************
 * xenon-native - VulkanRenderer private state (shared by the two .cpp files)
 ******************************************************************************
 */
#pragma once
#include "present_limiter.h"
#include "draw_preparation_dirty.h"
#include <rex/graphics/util/draw.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "gfx/command_processor.h"
#include "memory/physical_write_watch.h"
#include "gamma_ramp.h"
#include "palette_vertex_fetch.h"
#include "affine_vertex_fetch.h"
#include "resolve_bias.h"
#include "pipeline_warmup_file.h"
#include "vertex_index_range.h"
#include "constant_upload_cache.h"
#include "dma_index_cache.h"
#include "prepared_index_cache.h"
#include "resident_index_cache.h"
#include "vulkan_renderer.h"
#include "edram_layout.h"
#include "gpu_work_profile.h"

namespace xn::gfx {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
using rex::graphics::RegisterFile;
using rex::graphics::Shader;
using rex::graphics::SpirvShader;
using rex::graphics::SpirvShaderTranslator;

// 512 MB: the whole Xbox 360 physical address space the translator's shared
// memory SSBO aliases (address 0 of the buffer == guest physical 0).
constexpr VkDeviceSize kGuestPhysicalSize = VkDeviceSize(1) << 29;
constexpr uint32_t kFramesInFlight = 2;
constexpr VkDeviceSize kConstantRingSize = VkDeviceSize(32) << 20;
constexpr VkDeviceSize kIndexRingSize = VkDeviceSize(32) << 20;
// Render targets are views into the selected virtual EDRAM range, so targets
// at the same base share storage. Tracking arrays cover the maximum range;
// edram_tile_capacity bounds the range actually allocated for this renderer.

inline VkFormat SignedSamplingFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R16_UNORM: return VK_FORMAT_R16_SNORM;
    case VK_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_SNORM;
    case VK_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_BC4_SNORM_BLOCK;
    case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_BC5_SNORM_BLOCK;
    default: return format;
  }
}

#define XN_VK_CHECK(expr)                                                              \
  do {                                                                                 \
    VkResult xn_vk_result_ = (expr);                                                   \
    if (xn_vk_result_ != VK_SUCCESS) {                                                 \
      REXLOG_ERROR("xn-vk: {} failed: {}", #expr, int(xn_vk_result_));                 \
      return false;                                                                    \
    }                                                                                  \
  } while (0)

struct HostBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* mapped = nullptr;
  VkDeviceSize size = 0;
  VkDeviceAddress address = 0;
  VkMemoryPropertyFlags memory_flags = 0;
};

struct RingBuffer {
  HostBuffer buf;
  VkDeviceSize head = 0;  // next free byte
  // Per frame: [start, end) region in use.
  VkDeviceSize frame_start[kFramesInFlight] = {};
  VkDeviceSize frame_end[kFramesInFlight] = {};
};

struct GpuImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  VkImageView stencil_view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint32_t width = 0, height = 0;
  uint32_t depth = 1, layers = 1;
  uint32_t levels = 1;  // mip levels present in the image
};

struct RenderTargetImage {
  uint32_t id = 0;  // stable identity for the diagnostics
  GpuImage image;
  // Geometry Wars renders its main target at 2x MSAA. The multisample image is
  // what we rasterise into; `resolved` is the single-sample companion the
  // dynamic-rendering pass resolves into at every pass end, and is what every
  // reader (guest resolve, EDRAM store, readback, present) uses.
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  GpuImage resolved;
  xenos::MsaaSamples guest_msaa = xenos::MsaaSamples::k1X;
  bool is_depth = false;
  uint32_t edram_base = 0;   // in EDRAM tiles
  uint32_t xenos_format = 0;
  // The shape this target views EDRAM with. `pitch` is the row stride in
  // texels, which is what makes two targets at one base alias each other.
  uint32_t pitch = 0;
  uint32_t bytes_per_texel = 4;
  uint64_t depth_clear_discard_generation = 0;
  bool edram_resident = false;  // holds the live copy of its EDRAM range
  // Clear geometry depends on this immutable view and the tile mask, not on
  // the clear value. Reuse it for repeated SDK clears instead of walking and
  // allocating hundreds of rectangles for every cached depth view.
  struct DepthClearPlan {
    bool valid = false;
    std::bitset<kMaxVirtualEdramTileCapacity> tiles;
    std::vector<VkClearRect> rects;
  };
  std::array<DepthClearPlan, 16> depth_clear_plans;
  uint32_t next_depth_clear_plan = 0;
};

struct TextureKey {
  uint32_t base_address = 0;  // bytes
  uint32_t format = 0;
  uint32_t width = 0, height = 0;
  uint32_t pitch = 0;  // texels
  uint32_t tiled = 0;
  uint32_t endian = 0;
  uint32_t packed_mips = 0;
  // Mip chain: the guest keeps level 0 at base_address and levels 1.. at
  // mip_address, so both belong in the key - two fetches of the same base with
  // different mip data are different textures.
  uint32_t mip_address = 0;  // bytes; 0 = no mip chain
  uint32_t mip_levels = 1;   // levels to load, including the base
  uint32_t dimension = uint32_t(xenos::DataDimension::k2DOrStacked);
  uint32_t depth = 1;  // volume depth, array layers, or six cube faces
  bool operator==(const TextureKey& o) const {
    return std::memcmp(this, &o, sizeof(*this)) == 0;
  }
};
struct TextureKeyHash {
  size_t operator()(const TextureKey& k) const {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
    for (size_t i = 0; i < sizeof(k); ++i) h = (h ^ p[i]) * 1099511628211ull;
    return size_t(h);
  }
};

struct ResolvedKey {
  uint32_t address = 0;
  uint32_t format = 0;
  bool operator==(const ResolvedKey& o) const noexcept {
    return address == o.address && format == o.format;
  }
};
struct ResolvedKeyHash {
  size_t operator()(const ResolvedKey& k) const noexcept {
    return (uint64_t(k.address) << 32) | uint64_t(k.format);
  }
};

inline bool AreFormatsEquivalent(uint32_t f1, uint32_t f2) {
  if (f1 == f2) return true;
  using TF = xenos::TextureFormat;
  if ((f1 == uint32_t(TF::k_2_10_10_10) || f1 == uint32_t(TF::k_2_10_10_10_AS_16_16_16_16)) &&
      (f2 == uint32_t(TF::k_2_10_10_10) || f2 == uint32_t(TF::k_2_10_10_10_AS_16_16_16_16)))
    return true;
  if ((f1 == uint32_t(TF::k_8_8_8_8) || f1 == uint32_t(TF::k_8_8_8_8_AS_16_16_16_16) || f1 == uint32_t(TF::k_8_8_8_8_A)) &&
      (f2 == uint32_t(TF::k_8_8_8_8) || f2 == uint32_t(TF::k_8_8_8_8_AS_16_16_16_16) || f2 == uint32_t(TF::k_8_8_8_8_A))) {
    return true;
  }
  if ((f1 == uint32_t(TF::k_24_8) || f1 == uint32_t(TF::k_24_8_FLOAT)) &&
      (f2 == uint32_t(TF::k_24_8) || f2 == uint32_t(TF::k_24_8_FLOAT))) {
    return true;
  }
  if ((f1 == uint32_t(TF::k_16_16_16_16_FLOAT) || f1 == uint32_t(TF::k_16_16_16_16_EXPAND)) &&
      (f2 == uint32_t(TF::k_16_16_16_16_FLOAT) || f2 == uint32_t(TF::k_16_16_16_16_EXPAND))) {
    return true;
  }
  if ((f1 == uint32_t(TF::k_16_16_FLOAT) || f1 == uint32_t(TF::k_16_16_EXPAND)) &&
      (f2 == uint32_t(TF::k_16_16_FLOAT) || f2 == uint32_t(TF::k_16_16_EXPAND))) {
    return true;
  }
  if ((f1 == uint32_t(TF::k_16_FLOAT) || f1 == uint32_t(TF::k_16_EXPAND)) &&
      (f2 == uint32_t(TF::k_16_FLOAT) || f2 == uint32_t(TF::k_16_EXPAND))) {
    return true;
  }
  return false;
}

inline bool IsDisplayColorFormat(uint32_t fmt) {
  using TF = xenos::TextureFormat;
  return fmt == uint32_t(TF::k_8_8_8_8) ||
         fmt == uint32_t(TF::k_8_8_8_8_A) ||
         fmt == uint32_t(TF::k_8_8_8_8_AS_16_16_16_16) ||
         fmt == uint32_t(TF::k_2_10_10_10) ||
         fmt == uint32_t(TF::k_5_6_5);
}

struct TextureEntry {
  TextureKey key;
  std::array<uint64_t, 6> cube_face_resolve_serials = {};
  std::vector<uint64_t> mip_resolve_serials;  // includes level zero
  // Disjoint regions actually written by resolves. A partially resolved
  // texture still gets its untouched texels from the guest allocation.
  std::vector<VkRect2D> resolved_regions;
  bool fully_resolved = false;
  // Raw D24S8 bytes for a colour fetch of a depth resolve (for example MCLA's
  // per-object motion-blur index in stencil). Owned by resolve_texture_views.
  TextureEntry* packed_depth = nullptr;
  GpuImage image;
  uint32_t host_swizzle = 0;  // XE_GPU_TEXTURE_SWIZZLE of the host format
  // View per guest swizzle (composed with host_swizzle).
  std::unordered_map<uint32_t, VkImageView> views;
  bool resolved = false;   // written by Resolve, not loaded from guest memory
  bool unsupported = false;
  uint32_t guest_size = 0;
  uint64_t last_frame_used = 0;
  uint64_t resolve_serial = 0;  // refresh cropped sampling images after every copy
  // The guest rewrites textures in place (Geometry Wars redraws a 16x64
  // gradient/font strip ~2/s and composites its 512x512 atlas at load). The
  // cache therefore re-hashes each texture's guest bytes once per frame and
  // re-uploads when they change; loading once left every draw that samples
  // the strip multiplying by a stale all-zero texture, i.e. a black screen.
  uint64_t content_hash = 0;
  uint64_t source_revision = 0;
  std::vector<uint64_t> write_tokens;
  struct MipMemory {
    uint32_t address = 0, size = 0;
    uint64_t content_hash = 0;
    std::vector<uint64_t> write_tokens;
  };
  std::vector<MipMemory> mip_memory;  // levels 1..; base uses the fields above
  uint64_t last_hash_frame = UINT64_MAX;
  uint64_t guest_sync_serial = 0;  // last GPU version materialized for a CPU lock
};

struct ShaderEntry {
  std::unique_ptr<SpirvShader> shader;
  std::bitset<96> bounded_vertex_fetches;
  std::array<uint32_t, 96> bounded_vertex_extents = {};
  uint32_t scaled_vertex_fetch_slot = UINT32_MAX;
  uint32_t scaled_vertex_index_constant = 0;
  PaletteVertexFetch palette_vertex_fetch;
  AffineVertexFetch affine_vertex_fetch;
  struct VertexReadProfile {
    uint64_t calls = 0, requested_bytes = 0, mirrored_bytes = 0;
  };
  std::array<VertexReadProfile, 96> vertex_read_profile{};
  std::unordered_map<uint64_t, VkShaderModule> modules;  // by modification
};

struct PipelineKey {
  VkShaderModule vs = VK_NULL_HANDLE;
  VkShaderModule ps = VK_NULL_HANDLE;
  // One entry per bound colour target. A title that writes oC1 (Burnout
  // Revenge's scene pass writes colour to oC0 and motion vectors to oC1) needs
  // every attachment's format, blend state and write mask in the key, or the
  // second target silently keeps target 0's state.
  VkFormat color_formats[xenos::kMaxColorRenderTargets] = {
      VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  uint32_t topology = 0;
  uint32_t blend_controls[xenos::kMaxColorRenderTargets] = {};  // RB_BLENDCONTROL[i] & 0x1FFF1FFF
  uint32_t color_write_mask = 0;  // all four nibbles, as GetNormalizedColorMask returns them
  uint32_t depth_control = 0;   // normalized RB_DEPTHCONTROL
  uint32_t stencil_ref_mask = 0;  // RB_STENCILREFMASK & 0x00FFFF00
  uint32_t cull_mode = 0;
  uint32_t front_face = 0;
  uint32_t color_attachment_count = 0;
  uint32_t samples = 1;
  uint32_t depth_clamp = 0;
  uint32_t primitive_restart = 0;
  uint32_t reserved = 0;  // explicit padding for bytewise pipeline keys
  bool operator==(const PipelineKey& o) const {
    return std::memcmp(this, &o, sizeof(*this)) == 0;
  }
};
struct PipelineKeyHash {
  size_t operator()(const PipelineKey& k) const;
};

struct DescriptorKeyHash {
  using is_transparent = void;
  size_t operator()(std::span<const uint64_t> key) const {
    size_t hash = 0;
    for (uint64_t value : key)
      hash ^= std::hash<uint64_t>{}(value) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct DescriptorKeyEqual {
  using is_transparent = void;
  bool operator()(std::span<const uint64_t> a, std::span<const uint64_t> b) const {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
  }
};

struct ChangedPage {
  uint32_t index;
  uint64_t hash;
  uint64_t write_token;
};

struct DrawCommandState {
  bool valid = false;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSet vs_textures = VK_NULL_HANDLE, ps_textures = VK_NULL_HANDLE;
  VkViewport viewport{};
  VkRect2D scissor{};
  std::array<float, 3> depth_bias{};
  std::array<float, 4> blend_constants{};
  uint32_t stencil_reference = 0;
};
struct TextureDirtyCache {
  SpirvShader* shader = nullptr;
  VkShaderModule module = VK_NULL_HANDLE;
  uint32_t used_fetch_mask = 0;
  VkDescriptorSet set = VK_NULL_HANDLE;
  uint64_t frame_number = UINT64_MAX;
  uint32_t frame_index = UINT32_MAX;
  uint64_t resolves = UINT64_MAX;
  uint64_t epoch = UINT64_MAX;
};
static_assert(sizeof(VkViewport) == 6 * sizeof(float));
static_assert(sizeof(VkRect2D) == 4 * sizeof(uint32_t));

struct FrameContext {
  HostBuffer host_ui_geometry;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkSemaphore image_available = VK_NULL_HANDLE;
  // Helper passes allocate transient sets; immutable draw bindings survive
  // batch retirement in a separate pool until full or an image is destroyed.
  VkDescriptorPool texture_pool = VK_NULL_HANDLE;
  VkDescriptorPool persistent_texture_pool = VK_NULL_HANDLE;
  bool texture_pool_stale = false;
  std::unordered_map<std::vector<uint64_t>, VkDescriptorSet,
                     DescriptorKeyHash, DescriptorKeyEqual> texture_sets;
  uint32_t texture_set_count = 0, image_descriptor_count = 0, sampler_descriptor_count = 0;
  // Host buffers and GPU images to destroy once this frame's GPU work completed.
  std::vector<HostBuffer> garbage;
  std::vector<GpuImage> garbage_images;
  bool submitted = false;
  uint64_t submitted_serial = 0;
  bool timestamp_pending = false;
};

struct VulkanRenderer::Impl {
  // --- configuration / guest ---
  VulkanRendererConfig config;
  std::recursive_mutex mutex;
  SDL_Window* window = nullptr;
  // SDL window queries belong to the host thread. Publish the drawable size
  // without taking the command-thread mutex or delaying input polling.
  std::atomic<uint64_t> window_pixel_size{0};
  std::atomic<float> window_display_hz{0.f};
  uint8_t* guest_base = nullptr;
  size_t guest_size = 0;
  void PrepareGuestRead(uint32_t address,uint32_t size) {
    if (config.prepare_guest_read && guest_base && size &&
        uint64_t(address)+size <= std::min<uint64_t>(guest_size,kGuestPhysicalSize))
      config.prepare_guest_read(address,size);
  }
  RendererStats stats;
  // Launch diagnostics are immutable for this renderer. Environment lookup
  // and numeric parsing must not run on the ordinary per-draw path.
  struct LaunchDiagnostics {
    LaunchDiagnostics();
    uint32_t depth_mask = UINT32_MAX;
    uint8_t high_precision = 0; // 0 = normal, 1 = 10-bit, 2 = float
    bool depth_clear = false, resolve_rect = false, level_curve = false;
  } launch_diagnostics;
  bool cache_draw_state = false;
  bool cache_prepared_draw_state = false;  // opt-in XN_VK_PREPARED_DRAW_STATE=1
  bool cache_dirty_preparation = false;  // XN_VK_DIRTY_DRAW_PREPARATION
  DrawPreparationDirtyState preparation_dirty;
  // CPU-derived values only: resource checks, descriptors and target ownership
  // still run on each draw. Handles live until renderer shutdown.
  struct PreparedShaders {
    bool valid = false;
    ShaderEntry *input_vs = nullptr, *input_ps = nullptr, *selected_ps = nullptr;
    uint32_t primitive = 0, interpolator_mask = 0, param_gen_pos = UINT32_MAX;
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
  } prepared_shaders;
  struct PreparedViewport {
    bool valid = false, writes_depth = false, half_pixel = false;
    uint32_t width = 0, height = 0;
    rex::graphics::draw_util::ViewportInfo info{};
    VkViewport viewport{};
    VkRect2D scissor{};
  } prepared_viewport;
  struct PreparedSystem {
    bool valid = false;
    uint32_t texture_mask = 0, primitive = 0;
    SpirvShaderTranslator::SystemConstants values{};
  } prepared_system;
  struct PreparedPipeline {
    bool valid = false, polygonal = false;
    SpirvShader *vs_shader = nullptr, *ps_shader = nullptr;
    PipelineKey key{};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
  } prepared_pipeline;
  struct DirtyConstantBinding {
    const Shader* shader = nullptr;
    uint64_t batch = UINT64_MAX;
    uint32_t offset = 0;
  };
  uint64_t constant_batch_epoch = 0;
  std::array<DirtyConstantBinding, SpirvShaderTranslator::kConstantBufferCount>
      dirty_constant_bindings{};
  bool cache_texture_dirty = false;
  bool profile_draw_batches = false;
  bool profile_instance_batches = false;  // XN_VK_INSTANCE_BATCH_PROFILE=1
  bool gpu_draw_constants = false, multi_draw = false;
  bool cached_upload_fallback = false;
  bool reuse_float_constants = false;
  DrawBatchProfile draw_batch_profile;
  DrawBatchProfile instance_batch_profile;
  uint64_t draw_batch_epoch = 0;
  uint32_t pending_texture_dirty[2] = {UINT32_MAX, UINT32_MAX};
  uint64_t descriptor_prepare_epoch = 0;
  TextureDirtyCache texture_dirty_cache[2];
  // The snapshot is host order. Compare only texture/vertex fetch storage;
  // all other registers leave descriptor selection unchanged.
  void MarkTextureFetchChanges(const uint32_t* snapshot) {
    uint32_t dirty = 0;
    for (uint32_t i = 0; i < 192; ++i)
      if (regs.values[0x4800 + i] != snapshot[i]) dirty |= uint32_t(1) << (i / 6);
    pending_texture_dirty[0] |= dirty;
    pending_texture_dirty[1] |= dirty;
  }
  DrawCommandState draw_command_state;
  void InvalidateDrawState() {
    FlushNativeDraws();
    draw_command_state.valid = false;
    if (profile_draw_batches) draw_batch_profile.ExplicitFlush();
    if (profile_instance_batches) instance_batch_profile.ExplicitFlush();
    if (profile_draw_batches || profile_instance_batches) ++draw_batch_epoch;
  }

  // --- Vulkan core ---
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties = {};
  VkPhysicalDeviceMemoryProperties memory_properties = {};
  VkPhysicalDeviceFeatures features = {};
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  uint32_t queue_timestamp_bits = 0;
  VkQueue queue = VK_NULL_HANDLE;
  // Optional diagnostics. Timestamp queries are retired by existing fence
  // waits; collecting them must never introduce another GPU/CPU wait.
  bool measure_timings = false;
  bool readonly_barrier_elision = false;
  VkQueryPool batch_timestamps = VK_NULL_HANDLE;
  std::unique_ptr<GpuWorkProfile> work_profile;
  PFN_vkGetCalibratedTimestampsEXT work_calibrated_clock = nullptr;
  bool work_clock_supported = false;
  GpuWorkProfile::Token render_work_token;
  GpuWorkProfile* WorkProfile() {
    return frame_open && !in_readback ? work_profile.get() : nullptr;
  }
  VkFormat depth_stencil_format = VK_FORMAT_D24_UNORM_S8_UINT;
  VkFormat float_depth_stencil_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
  VkSampleCountFlags supported_sample_counts = VK_SAMPLE_COUNT_1_BIT;

  // --- swapchain ---
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swapchain_format = VK_FORMAT_B8G8R8A8_UNORM;
  VkExtent2D swapchain_extent = {};
  uint64_t swapchain_window_pixel_size = 0;
  std::vector<VkImage> swapchain_images;
  std::vector<VkImageView> swapchain_image_views;
  // Reacquiring an image guarantees its previous presentation wait completed.
  // A frame fence alone does not make a presentation semaphore reusable.
  std::vector<VkSemaphore> swapchain_present_ready;
  std::vector<VkImageLayout> swapchain_layouts;
  bool swapchain_dirty = false;
  uint32_t target_frame_rate = 60;
  PresentLimiter present_limiter;
  std::atomic<uint64_t> presented_frames{0};

  // --- frames ---
  FrameContext frames[kFramesInFlight];
  // Self-contained readback (CaptureFrame / frame dumps): its own pool,
  // command buffer and fence, so a readback never records into - or resets -
  // the command buffer the frame in flight is using. Sharing them was why
  // every capture after the first came back all zeros: whichever thread
  // called BeginFrameIfNeeded() next reset the buffer holding the pending
  // copy, so it never executed and the staging buffer kept its initial zeros.
  VkCommandPool readback_pool = VK_NULL_HANDLE;
  VkCommandBuffer readback_cb = VK_NULL_HANDLE;
  VkFence readback_fence = VK_NULL_HANDLE;
  uint32_t frame_index = 0;
  uint64_t frame_number = 0;
  uint64_t guest_swap_number = 0;
  bool frame_open = false;  // command buffer recording
  VkCommandBuffer cb = VK_NULL_HANDLE;

  // --- EDRAM ---
  HostBuffer edram;
  bool edram_aliasing = true;
  uint32_t edram_tile_capacity = kDefaultEdramTileCapacity;
  VkDeviceSize EdramSize() const { return VkDeviceSize(edram_tile_capacity) * kEdramTileBytes; }
  uint32_t RenderTargetBase(uint32_t register_value) const {
    return VirtualEdramBase(register_value, edram_tile_capacity);
  }

  // --- guest memory mirror (the translator's shared memory) ---
  HostBuffer mirror;
  uint32_t mirror_binding_count = 1;
  VkDeviceSize mirror_binding_size = kGuestPhysicalSize;
  // --- changed-page mirror upload (see MirrorGuestRange) ---
  static constexpr uint32_t kMirrorPageSize = 4096;
  static constexpr uint32_t kMirrorPageCount = kGuestPhysicalSize / kMirrorPageSize;
  // Guest pages copied to the GPU-side mirror at least once.
  std::bitset<kMirrorPageCount> mirrored_pages;
  std::vector<uint64_t> mirror_page_hashes;
  std::vector<uint64_t> mirror_write_tokens;
  PhysicalWriteWatch* write_watch = nullptr;
  // Staging ring for changed-page snapshots, then a vkCmdCopyBuffer into the
  // device-local mirror on the GPU timeline (the SG rendering fix, general).
  RingBuffer mirror_upload_ring;
  // --- rings ---
  RingBuffer constant_ring;
  std::array<ConstantUploadCache<4096>, SpirvShaderTranslator::kConstantBufferCount>
      constant_upload_cache;
  RingBuffer index_ring;
  // Five BDA constant addresses followed by an indirect command. A 64-byte
  // stride lets both shader stages select the record through firstInstance.
  struct alignas(8) NativeDrawRecord {
    uint64_t addresses[5];
    uint32_t command[5];
    uint32_t padding = 0;
  };
  static_assert(sizeof(NativeDrawRecord) == 64 && offsetof(NativeDrawRecord, command) == 40);
  RingBuffer draw_record_ring;
  uint32_t pending_native_draw_count = 0;
  VkDeviceSize pending_native_draw_offset = 0;
  bool pending_native_indexed = false;
  VkDrawIndexedIndirectCommand pending_native_first{};
  VkIndexType pending_native_index_type = VK_INDEX_TYPE_UINT16;
  void FlushNativeDraws();
  bool reuse_dma_indices = false; // opt-in XN_VK_INDEX_REUSE=1
  DmaIndexCache dma_index_cache;
  bool cache_prepared_indices = false; // opt-in XN_VK_PREPARED_INDEX_CACHE=1
  uint32_t prepared_index_min_count = 0; // XN_VK_PREPARED_INDEX_MIN_COUNT
  PreparedIndexCache prepared_index_cache;
  bool cache_resident_indices = false;
  uint32_t resident_index_min_count = 256;
  uint32_t resident_index_mib = 32;
  HostBuffer resident_index_buffer;
  ResidentIndexCache resident_index_cache;

  // --- descriptor layouts / sets ---
  VkDescriptorSetLayout layout_shared_memory = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_constants = VK_NULL_HANDLE;
  VkDescriptorPool static_pool = VK_NULL_HANDLE;
  VkDescriptorSet set_shared_memory = VK_NULL_HANDLE;
  VkDescriptorSet set_constants = VK_NULL_HANDLE;
  std::unordered_map<uint32_t, VkDescriptorSetLayout> texture_layouts;  // (tex<<16)|samp
  std::unordered_map<uint64_t, VkPipelineLayout> pipeline_layouts;
  VkDeviceSize ubo_align = 256;
  std::array<VkDeviceSize, SpirvShaderTranslator::kConstantBufferCount> ubo_sizes = {};

  // --- shaders / pipelines ---
  std::unique_ptr<SpirvShaderTranslator> translator;
  bool supports_clip_distance = false;
  bool supports_cull_distance = false;
  std::unordered_map<uint64_t, ShaderEntry> shaders;  // by ucode hash (both types)
  // Ring feed: the last IM_LOAD of each type is the bound shader.
  ShaderEntry* active_vertex_shader = nullptr;
  ShaderEntry* active_pixel_shader = nullptr;
  std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;
  PipelineKey last_pipeline_key{};
  VkPipeline last_pipeline = VK_NULL_HANDLE;
  bool last_pipeline_valid = false;
  VkPipelineCache vk_pipeline_cache = VK_NULL_HANDLE;
  std::filesystem::path pipeline_cache_file;
  using WarmupRecord = PipelineWarmupRecord<PipelineKey>;
  std::unordered_map<VkShaderModule, std::pair<ShaderEntry*, uint64_t>> module_sources;
  std::vector<WarmupRecord> pipeline_warmup;
  bool warming_pipelines = false;
  bool warmup_enabled = true;
  uint64_t texture_uploads_ring = 0, texture_uploads_dedicated = 0;
  VkShaderModule debug_vertex_module = VK_NULL_HANDLE;
  VkShaderModule debug_pixel_module = VK_NULL_HANDLE;

  // --- render targets ---
  uint32_t next_render_target_id = 1;
  std::unordered_map<uint32_t, RenderTargetImage> color_targets;  // by edram base
  std::unordered_map<uint64_t, RenderTargetImage> depth_targets;
  std::bitset<kMaxVirtualEdramTileCapacity> known_depth_clear_tiles;
  std::array<uint32_t, kMaxVirtualEdramTileCapacity> depth_clear_values{};
  uint64_t depth_clear_generation = 0;
  std::unordered_map<uint32_t, SurfaceDesc> registered_color;  // SetRenderTarget
  std::unordered_map<uint32_t, SurfaceDesc> registered_depth;
  GpuImage* last_presented = nullptr;  // for CaptureFrame
  GpuImage presented_image;  // snapshot owned until the next presentation
  bool rendering_active = false;
  uint64_t render_pass_starts_ = 0;  // diagnostic: render pass restarts per frame
  RenderTargetImage* active_color = nullptr;  // attachment 0; the present/pick heuristics use it
  std::array<RenderTargetImage*, xenos::kMaxColorRenderTargets> active_colors{};
  uint32_t active_color_count = 0;
  // EDRAM colour-tile ownership is per bound target: with several attachments
  // live at once each owns its own EDRAM range, so one owner slot is not enough.
  std::array<RenderTargetImage*, xenos::kMaxColorRenderTargets> edram_color_owners{};
  RenderTargetImage* active_depth = nullptr;

  // --- textures ---
  std::unordered_map<TextureKey, std::unique_ptr<TextureEntry>, TextureKeyHash> textures;
  // Keep each address/layout alive while queued descriptors can reference it.
  std::unordered_map<TextureKey, std::unique_ptr<TextureEntry>, TextureKeyHash> resolve_images;
  std::unordered_map<TextureKey, std::unique_ptr<TextureEntry>, TextureKeyHash> resolve_texture_views;
  std::unordered_map<ResolvedKey, TextureEntry*, ResolvedKeyHash> resolved_by_key;
  std::unordered_map<uint32_t, TextureEntry*> resolved_by_address;
  std::unordered_map<uint32_t, std::vector<TextureEntry*>> resolved_by_addr;
  // VdSwap publishes the real display allocation independently of cached
  // resolves, which may be rooted at an interior strip of that allocation.
  std::unordered_map<uint32_t, VkExtent2D> display_buffers;
  // Applies RB_COPY_DEST_INFO.copy_dest_exp_bias at resolve. Only built the
  // first time a resolve carries a nonzero bias, so a title that never uses
  // one (Geometry Wars 1: every resolve is exp_bias=0) never creates it.
  ResolveBiasPass resolve_bias;
  ResolveBiasPass resolve_depth_pack;
  std::unordered_map<uint64_t, VkSampler> samplers;
  GpuImage dummy_texture;  // 1x1 white
  VkImageView dummy_view = VK_NULL_HANDLE;
  GpuImage dummy_cube, dummy_volume;
  VkSampler dummy_sampler = VK_NULL_HANDLE;

  // --- display gamma ramp (DC_LUT_*) ---
  // The console keeps a display LUT that the guest can replace through the
  // DC_LUT registers (what D3D SetGammaRamp / SetPWLGamma drive) and that is
  // applied when scanning out. Observed here before deciding whether to
  // implement it.
  uint64_t dc_lut_writes = 0;
  uint32_t dc_lut_rw_index = 0;
  uint32_t dc_lut_rw_mode = 0;
  uint32_t dc_lut_seq_component = 0;   // SEQ_COLOR / PWL_DATA cycle R,G,B
  uint32_t dc_lut_pwl_component = 0;
  uint32_t dc_lut_write_mask = 0;
  // 256 entries x RGB, normalised to the UNORM16 upload format.
  std::array<std::array<uint16_t, 3>, 256> dc_lut = {};
  bool dc_lut_valid = false;
  // Highest entry the guest has written, +1. The second title writes 128, not
  // 256, and feeding the unwritten half to the sampler would map the top half
  // of every channel to black.
  uint32_t dc_lut_entries = 0;
  // Applied at present: see gamma_ramp.h. Allocated after the guest programs a ramp.
  GammaRampPass gamma_ramp;
  GpuImage gamma_out;      // the ramped frame; what CaptureFrame and present use
  GpuImage dc_lut_image;   // dc_lut_entries x 1, RGBA16 UNORM
  uint64_t dc_lut_uploaded_writes = 0;

  // --- per draw scratch ---
  std::vector<ChangedPage> scratch_changed_pages;
  std::vector<VkBufferCopy> scratch_copies;
  RegisterFile regs;
  bool ring_registers_current = false;
  // Repeated draws (including signed/unsigned bindings) commonly use the
  // same fetch. A new batch rechecks guest memory; a resolve can replace the
  // latest image at any address and therefore invalidates every slot.
  struct TextureBindingCache {
    xenos::xe_gpu_texture_fetch_t fetch{};
    uint64_t frame = UINT64_MAX;
    uint64_t resolves = UINT64_MAX;
    VkImageView view = VK_NULL_HANDLE;
    const char* kind = "dummy";
  };
  std::array<TextureBindingCache, 256> texture_binding_cache{};
  // XN_DEBUG_FRAME=<n>: log every draw (and its texture bindings) of frame n.
  int64_t debug_frame = -1;
  int64_t debug_swap = -1;
  int64_t debug_frame_count = 1;  // XN_DEBUG_FRAME_COUNT: latch over a range
  uint32_t cur_limit_step = 0;
  bool debug_frame_active = false;
  uint64_t debug_frame_first_draw = 0;
  bool debug_frame_prev = false;
  bool in_readback = false;  // keeps debug_frame_active latched across readbacks
  std::string last_texture_log;

  std::shared_ptr<const HostUiFrame> host_ui_frame;
  std::shared_ptr<const HostUiFont> host_ui_font_source;
  PresentationSettings presentation_settings;
  GpuImage host_ui_font;
  VkSampler host_ui_sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout host_ui_texture_layout = VK_NULL_HANDLE;
  VkPipelineLayout host_ui_pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule host_ui_vs = VK_NULL_HANDLE, host_ui_ps = VK_NULL_HANDLE;
  VkShaderModule host_sharpen_ps = VK_NULL_HANDLE;
  std::unordered_map<uint32_t, VkPipeline> host_ui_pipelines, host_sharpen_pipelines;
  PresentParams last_host_present{.width=0, .height=0};
  uint64_t last_host_present_ns = 0;
  bool InitHostUi();
  void DestroyHostUi();
  VkPipeline HostUiPipeline(bool sharpen);
  bool DrawHostUi(VkImage output, uint32_t width, uint32_t height, GpuImage* sharpen_source);

  // ---- vulkan_renderer.cpp ----
  bool InitInstance();
  bool PickDevice();
  bool InitDevice();
  bool InitSwapchain();
  void DestroySwapchain();
  bool InitFrames();
  bool InitMirror();
  void ObserveDcLutWrite(uint32_t index, uint32_t value);
  bool InitEdram();
  bool InitRings();
  bool InitDescriptors();
  bool InitDummyTexture();
  bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, HostBuffer& out,
                        bool prefer_device_local = false);
  void DestroyHostBuffer(HostBuffer& buf);
  bool CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                   VkImageAspectFlags aspect, GpuImage& out,
                   VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                   VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D,
                   uint32_t depth_or_layers = 1, uint32_t mip_levels = 1);
  // The single-sample image a reader should use for this target.
  GpuImage& ReadableImage(RenderTargetImage& rt) {
    return rt.samples != VK_SAMPLE_COUNT_1_BIT ? rt.resolved : rt.image;
  }
  void DestroyImage(GpuImage& img);
  uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred);
  void BeginFrameIfNeeded();
  void EndRendering();
  void Transition(GpuImage& img, VkImageLayout new_layout);
  void TransitionRaw(VkImage image, VkImageAspectFlags aspect, VkImageLayout old_layout,
                     VkImageLayout new_layout, uint32_t layers = 1, uint32_t levels = 1);
  bool SubmitFrame(bool present, uint32_t swap_image_index);
  void CollectBatchTiming(uint32_t index);  // caller already retired its fence
  void WaitIdleAndReset();
  uint8_t* RingAllocate(RingBuffer& ring, VkDeviceSize size, VkDeviceSize align,
                        VkDeviceSize& offset_out);
  void RingBeginFrame(RingBuffer& ring);
  bool ReadbackImage(GpuImage& img, std::vector<uint8_t>& rgba8_out,
                     const VkRect2D* region = nullptr, std::vector<float>* depth_values = nullptr,
                     uint32_t raw_bytes_per_pixel = 0);
  bool DumpFrame(GpuImage& source);
  void SnapshotPresentedImage(GpuImage* source);
  // Tests/diagnostics: a fixed cyan triangle with hand-written SPIR-V into the
  // colour target the current register file selects.
  bool DebugTriangleIntoCurrentTarget();

  // ---- vulkan_draw.cpp ----
  uint32_t GuestTargetHeight(uint32_t width) const;
  // Independent guard: no render target can exceed the console's 10 MiB of
  // EDRAM, whatever derived its height.
  static uint32_t HostBytesPerTexel(VkFormat f);
  uint32_t ClampHeightToEdram(uint32_t width, uint32_t height, uint32_t bytes_per_texel,
                              uint32_t samples, const char* what) const;
  RenderTargetImage* GetColorTarget(uint32_t edram_base, xenos::ColorRenderTargetFormat format,
                                    uint32_t surface_pitch, uint32_t height_needed = 0);
  RenderTargetImage* GetDepthTarget(uint32_t edram_base, uint32_t width, uint32_t height,
                                    VkSampleCountFlagBits samples);
  VkSampleCountFlagBits GuestSampleCount(xenos::MsaaSamples msaa) const;
  void EnsureRendering(RenderTargetImage* color, RenderTargetImage* depth);
  void EnsureRendering(RenderTargetImage* const* colors, uint32_t color_count,
                       RenderTargetImage* depth);
  void StoreAllEdramOwners(bool release = false);
  // EDRAM ownership transfer: a target's pixels live in EDRAM between passes,
  // so switching targets stores the outgoing view and loads the incoming one.
  bool EdramRange(const RenderTargetImage& rt, VkDeviceSize& offset_out,
                  VkDeviceSize& size_out) const;
  void StoreTargetToEdram(RenderTargetImage& rt);
  void LoadTargetFromEdram(RenderTargetImage& rt);
  ShaderEntry* GetShader(const ShaderRef& ref, xenos::ShaderType type);
  VkShaderModule GetShaderModule(ShaderEntry& entry, uint64_t modification, Shader::Translation* t);
  VkDescriptorSetLayout GetTextureLayout(uint32_t texture_count, uint32_t sampler_count);
  VkPipelineLayout GetPipelineLayout(uint32_t vs_tex, uint32_t vs_samp, uint32_t ps_tex,
                                     uint32_t ps_samp);
  VkPipeline GetPipeline(const PipelineKey& key, VkPipelineLayout layout);
  void RememberPipeline(const PipelineKey& key);
  bool WarmPipelines();
  TextureEntry* GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch);
  bool RefreshResolvedTextureLevels(TextureEntry& entry);
  bool LoadTexture(TextureEntry& entry);
  // Reads the guest bytes for `entry` and converts them to the host layout
  // (untile, endian swap, 16-bit expansion). Returns false if out of bounds.
  bool BuildTextureUpload(TextureEntry& entry, std::vector<uint8_t>& out, uint64_t& hash_out,
                          uint32_t mip = 0);
  void DumpTextureBytes(const TextureKey& k, const std::vector<uint8_t>& bytes);
  void UploadTexture(TextureEntry& entry, const std::vector<uint8_t>& bytes,
                     uint32_t mip = 0);
  // One staging allocation for a whole mip chain. Uploading level by level
  // takes a ring slot each, and a chain exhausts the ring - which drops every
  // later upload onto its own vkAllocateMemory, the exact cost the pacing work
  // removed by introducing ring reuse.
  struct LevelCopy {
    uint64_t offset = 0;
    uint32_t mip = 0, width = 0, height = 0;
  };
  void UploadTextureLevels(TextureEntry& entry, const std::vector<uint8_t>& blob,
                           const std::vector<LevelCopy>& levels);
  // Once per frame per texture: re-upload if the guest bytes changed.
  void RefreshTextureIfChanged(TextureEntry& entry);
  bool RefreshResolvedCubeFaces(TextureEntry& entry);
  VkImageView GetTextureView(TextureEntry& entry, uint32_t guest_swizzle,
                            bool is_signed = false,
                            VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_MAX_ENUM);
  VkSampler GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch,
                       const SpirvShader::SamplerBinding& binding);
  VkDescriptorSet AllocateTextureSet(SpirvShader* shader, const RegisterFile& regs,
                                    VkShaderModule module);
  bool MirrorGuestRange(uint32_t guest_address, uint32_t size);
  // All three read the register file in `regs` (already materialised).
  void DrawImpl(const DrawParams& params, ShaderEntry* prepared_vs = nullptr,
                ShaderEntry* prepared_ps = nullptr);
  bool ApplyDepthClearAliases(const DrawParams& params, const Shader& vs);
  void ClearDepthTiles(RenderTargetImage& target,
                      const std::bitset<kMaxVirtualEdramTileCapacity>& tiles,
                      uint32_t packed_clear);
  void ForgetDepthClearTiles(RenderTargetImage& target);
  void ClearImpl(const ClearParams& params);
  void ResolveImpl(const ResolveParams& params);
  // The PM4 ring resolve: a draw issued with RB_MODECONTROL.edram_mode == kCopy.
  void ResolveFromRegisters();
  void PresentImpl(const PresentParams& params);
  // Applies the guest display gamma ramp to `source`, returning the ramped
  // image, or leaves `source` untouched when no ramp has been programmed.
  GpuImage* ApplyGammaRamp(GpuImage* source);
  VkFormat ColorFormatToVk(xenos::ColorRenderTargetFormat f);
};

// Byte swap helpers shared by both files.
inline void SwapEndian(uint8_t* data, size_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i + 1 < size; i += 2) std::swap(data[i], data[i + 1]);
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

}  // namespace xn::gfx
