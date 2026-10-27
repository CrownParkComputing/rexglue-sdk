// xenon-native: the GPU front end. Owns the register file and walks the PM4
// ring the title's D3D writes: fences, waits, indirect buffers, swaps and
// interrupts are handled here; draws/state are handed to IRenderer.
#pragma once
#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include "register_file.h"

namespace xn {

class IRenderer;
class GuestThread;
struct DrawPacket;

class CommandProcessor {
 public:
  explicit CommandProcessor(IRenderer* renderer);
  ~CommandProcessor();

  void Start();   // spawns the CP thread and the vsync thread
  void Stop();

  // Vd* imports.
  void InitializeRingBuffer(uint32_t physical_address, uint32_t size_log2);
  void EnableReadPointerWriteBack(uint32_t physical_address, uint32_t block_size_log2);
  void SetInterruptCallback(uint32_t callback, uint32_t user_data);

  // MMIO (0x7FC80000 register window).
  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  uint64_t swaps() const { return swaps_.load(); }
  uint64_t draws() const { return draws_.load(); }
  const GpuRegisterFile& regs() const { return regs_; }

 private:
  friend struct CommandProcessorTestAccess;
  void WorkerMain(GuestThread& self);
  void VsyncMain(GuestThread& self);
  uint32_t ExecuteRange(uint32_t start_index, uint32_t end_index);  // returns next unread dword
  // Executes packets from a physical address; returns false on a malformed stream.
  bool ExecuteBuffer(uint32_t physical_address, uint32_t dword_count, int depth);
  uint32_t ExecutePacket(const uint32_t* words, uint32_t available, int depth);  // returns dwords consumed
  void ExecuteType3(uint32_t opcode, const uint32_t* data, uint32_t count, int depth);
  void SetRegister(uint32_t index, uint32_t value);
  const char* const_src_ = "?";
  uint32_t const_src_addr_ = 0;
  void SetConstants(uint32_t index, const uint32_t* big_endian_words, uint32_t count);
  void MarkRegisterBatch(uint32_t index, uint32_t count);
  void FlushRegisterBatch();
  void FlushRegisterBatchLocked();  // state mutex already held when batching
  void DeliverRegisterBatchLocked(const DrawPacket* draw);  // optional fused draw
  void DrawLocked(const DrawPacket& draw);
  std::unique_lock<std::recursive_mutex> LockRegisterBatch();
  void WriteBackReadPointer();
  void DispatchInterrupt(uint32_t source, uint32_t cpu);
  uint32_t ReadPhysical32(uint32_t physical) const;
  void WritePhysical32(uint32_t physical, uint32_t value);
  bool EvaluateWait(uint32_t wait_info, uint32_t poll_addr, uint32_t ref, uint32_t mask);

  IRenderer* renderer_;
  GpuRegisterFile regs_;
  // Only 0x2000..0x2800 and 0x4000..0x4928 are eligible. Each bit names a
  // 16-dword block of side-effect-free storage. CP truth remains immediate.
  bool batch_registers_ = false, register_batch_pending_ = false;
  bool draw_state_batch_ = false;
  bool shader_load_requires_registers_ = true;
  std::recursive_mutex register_batch_mutex_;
  std::array<uint64_t, 2> dirty_draw_state_{};
  std::array<uint64_t, 3> dirty_constants_{};
  uint32_t ring_base_ = 0;         // physical
  uint32_t ring_size_dwords_ = 0;
  std::atomic<uint32_t> read_index_{0};
  std::atomic<uint32_t> write_index_{0};
  uint32_t writeback_ptr_ = 0;     // physical
  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_user_data_ = 0;
  std::mutex interrupt_mutex_;
  std::mutex kick_mutex_;
  std::condition_variable kick_cv_;
  std::atomic<bool> running_{false};
  std::shared_ptr<GuestThread> worker_;
  std::shared_ptr<GuestThread> vsync_;
  std::atomic<uint64_t> swaps_{0};
  std::atomic<uint64_t> draws_{0};
  uint32_t event_counter_ = 0;
  uint64_t bin_mask_ = 0xFFFFFFFFull;
  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint32_t unknown_logged_ = 0;
};

}  // namespace xn
