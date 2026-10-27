/**
 * @file        graphics/stub/command_processor.cpp
 * @brief       rexgpu-stub: PM4 consumer with no rendering backend
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 */

#include "stub/command_processor.h"

#include <chrono>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/perf/counter.h>
#include <rex/thread.h>

REXCVAR_DEFINE_UINT32(stub_wait_reg_mem_max_ms, 16, "GPU",
                      "rexgpu-stub: maximum time a PM4 WAIT_REG_MEM packet may block the command "
                      "processor before proceeding unmatched. With no GPU behind the ring, waits "
                      "on draw/query completions can never be satisfied; 0 makes every unmatched "
                      "wait time out on its first poll.")
    .range(0, 10000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::stub {

void StubCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                     uint32_t frontbuffer_height) {
  // No presenter - just keep counters useful for headless benchmarking.
  if (++swaps_ % 256 == 0) {
    REXLOG_INFO("rexgpu-stub: {} swaps, {} draws ({} indices) and {} copies skipped, "
                "{} WAIT_REG_MEM timeouts; last frontbuffer {:08X} {}x{}",
                swaps_, draws_skipped_, indices_skipped_, copies_skipped_,
                wait_reg_mem_timeouts_, frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
  }
}

bool StubCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                     IndexBufferInfo* index_buffer_info,
                                     bool major_mode_explicit) {
  ++draws_skipped_;
  indices_skipped_ += index_count;
  return true;
}

bool StubCommandProcessor::IssueCopy() {
  ++copies_skipped_;
  return true;
}

bool StubCommandProcessor::ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader,
                                                           uint32_t packet, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t wait = reader->ReadAndSwap<uint32_t>();

  bool is_memory = (wait_info & 0x10) != 0;

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(REXCVAR_GET(stub_wait_reg_mem_max_ms));

  bool matched = false;
  do {
    uint32_t value = 0;
    if (is_memory) {
      value =
          *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));
      value = xenos::GpuSwap(value, static_cast<xenos::Endian>(poll_reg_addr & 0x3));
    } else {
      value = ReadRegisterValue(poll_reg_addr);
      if (poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST) {
        MakeCoherent();
        value = ReadRegisterValue(poll_reg_addr);
      }
    }
    switch (wait_info & 0x7) {
      case 0x1:
        matched = (value & mask) < ref;
        break;
      case 0x2:
        matched = (value & mask) <= ref;
        break;
      case 0x3:
        matched = (value & mask) == ref;
        break;
      case 0x4:
        matched = (value & mask) != ref;
        break;
      case 0x5:
        matched = (value & mask) >= ref;
        break;
      case 0x6:
        matched = (value & mask) > ref;
        break;
      default:
        matched = wait_info & 0x7;
        break;
    }
    if (!matched) {
      if (std::chrono::steady_clock::now() >= deadline) {
        ++wait_reg_mem_timeouts_;
        // Titles that gate frames on GPU completions poll this ~60x/s; log the
        // first few then a sparse sample, the running total is in the swap stats.
        if (wait_reg_mem_timeouts_ <= 5 || wait_reg_mem_timeouts_ % 1000 == 0) {
          REXGPU_WARN("rexgpu-stub: WAIT_REG_MEM #{} timed out ({} addr {:08X} ref {:08X} "
                      "mask {:08X}); proceeding unmatched",
                      wait_reg_mem_timeouts_, is_memory ? "mem" : "reg", poll_reg_addr, ref, mask);
        }
        return true;
      }
      PrepareForWait();
      rex::thread::MaybeYield();
      rex::thread::SyncMemory();
      ReturnFromWait();
      if (!worker_running_) {
        return false;
      }
    }
  } while (!matched);

  return true;
}

}  // namespace rex::graphics::stub
