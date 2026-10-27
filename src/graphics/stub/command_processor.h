/**
 * @file        graphics/stub/command_processor.h
 * @brief       rexgpu-stub: PM4 consumer with no rendering backend
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/graphics/command_processor.h>

namespace rex::graphics::stub {

// Executes the Xenos PM4 stream for its guest-visible side effects only:
// the register file, EVENT_WRITE_* fence/timestamp writebacks,
// REG_TO_MEM/MEM_WRITE stores, ring read-pointer writeback, swap counters
// and interrupts. Draws, copies and resolves are counted and skipped;
// IssueSwap records the frontbuffer but presents nothing.
class StubCommandProcessor final : public CommandProcessor {
 public:
  StubCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state)
      : CommandProcessor(graphics_system, kernel_state) {}

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;
  rex::graphics::Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                    const uint32_t* host_address, uint32_t dword_count) override {
    // Draws are skipped; nothing dereferences the returned Shader*.
    return nullptr;
  }
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

  // The guest uses WAIT_REG_MEM to block the CP on values only real GPU
  // work would produce (draw/query completions). With nothing rendering,
  // those waits can never be satisfied; the base implementation would spin
  // forever and stall the ring. This bounds each wait at
  // stub_wait_reg_mem_max_ms, then proceeds and logs.
  bool ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet,
                                       uint32_t count) override;

 private:
  uint64_t draws_skipped_ = 0;
  uint64_t indices_skipped_ = 0;
  uint64_t copies_skipped_ = 0;
  uint64_t wait_reg_mem_timeouts_ = 0;
  uint64_t swaps_ = 0;
};

}  // namespace rex::graphics::stub
