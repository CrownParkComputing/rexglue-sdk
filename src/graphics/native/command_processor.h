/**
 * @file        graphics/native/command_processor.h
 * @brief       Native GPU renderer - PM4 -> native GPU command translation
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 *
 * @remarks     NativeCommandProcessor derives from the shared, game-agnostic
 *              rex::graphics::CommandProcessor. The base class does ALL PM4
 *              ring-buffer parsing (Type0/1/2/3 packets, DRAW_INDX, SET_CONSTANT,
 *              IM_LOAD shader loads, EVENT_WRITE, WAIT_REG_MEM, INDIRECT_BUFFER,
 *              XE_SWAP, ...) and hands us fully-decoded work through these
 *              virtual seams:
 *                - SetupContext / ShutdownContext : create/destroy host device
 *                - LoadShader                     : Xenos microcode -> host shader
 *                - IssueDraw                      : register state -> native draw
 *                - IssueCopy                      : EDRAM resolve -> native resolve
 *                - IssueSwap                      : present the front buffer
 *
 *              SCAFFOLD STATUS: every seam below currently LOGS and returns
 *              success without emitting host GPU work. This proves the plugin
 *              loads, the base PM4 parser runs, and the draw/shader/resolve/
 *              swap stream is observed for any title. The real renderer will be
 *              filled in incrementally behind these same signatures (see the
 *              phased plan). Nothing here is PGR3-specific.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::native {

class NativeGraphicsSystem;

class NativeCommandProcessor : public CommandProcessor {
 public:
  NativeCommandProcessor(NativeGraphicsSystem* graphics_system,
                         system::KernelState* kernel_state);
  ~NativeCommandProcessor() override;

  // Trace / save-state seams (no-ops until the native backend owns memory).
  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;
  void RestoreEdramSnapshot(const void* snapshot) override;

  // Present seam. Native renderer will hand a host image to the presenter here.
  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;

  bool IssueCopy() override;

 private:
  // Shader cache keyed by microcode hash. The base Shader retains the parsed
  // microcode; a native backend attaches a translated SPIR-V/DXIL/MSL module.
  std::vector<std::unique_ptr<Shader>> shader_storage_;
  std::unordered_map<uint64_t, Shader*> shader_map_;

  uint64_t draw_count_ = 0;
  uint64_t copy_count_ = 0;
  uint64_t swap_count_ = 0;
};

}  // namespace rex::graphics::native
