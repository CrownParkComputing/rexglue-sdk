/**
 * @file        graphics/native/command_processor.cpp
 * @brief       Native GPU renderer - PM4 -> native GPU command translation
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 */

#include "native/command_processor.h"

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "native/graphics_system.h"

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
  // Phase 0 scaffold: no host device objects created yet. Returning true lets
  // the base command-processor worker thread start and drive the PM4 parser so
  // the draw/shader/resolve/swap stream flows through the seams below.
  REXLOG_INFO("rexgpu-native: SetupContext (scaffold - no host device yet)");
  return true;
}

void NativeCommandProcessor::ShutdownContext() {
  REXLOG_INFO("rexgpu-native: ShutdownContext (draws={} copies={} swaps={})", draw_count_,
              copy_count_, swap_count_);
  shader_map_.clear();
  shader_storage_.clear();
}

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
  // Return true: the draw is "consumed". Nothing is rendered yet.
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
  REXLOG_INFO(
      "rexgpu-native: IssueSwap #{} frontbuffer=0x{:08X} {}x{} (draws this run={} copies={})",
      swap_count_, frontbuffer_ptr, frontbuffer_width, frontbuffer_height, draw_count_,
      copy_count_);
}

void NativeCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) {
  (void)base_ptr;
  (void)length;
}

void NativeCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  (void)snapshot;
}

}  // namespace rex::graphics::native
