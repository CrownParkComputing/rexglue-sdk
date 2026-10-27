#include "command_processor.h"
#include "frame_rate.h"

#include <bit>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <vector>

#include <rex/logging.h>

#include "kernel/kernel.h"
#include "renderer.h"

namespace xn {

namespace {

// PM4 type-3 opcodes (public Radeon/Xenos names).
enum : uint32_t {
  PM4_NOP = 0x10, PM4_REG_RMW = 0x21, PM4_DRAW_INDX = 0x22, PM4_VIZ_QUERY = 0x23,
  PM4_SET_STATE = 0x25, PM4_WAIT_FOR_IDLE = 0x26, PM4_IM_LOAD = 0x27, PM4_IM_LOAD_IMMEDIATE = 0x2B,
  PM4_IM_STORE = 0x2C, PM4_SET_CONSTANT = 0x2D, PM4_LOAD_CONSTANT_CONTEXT = 0x2E,
  PM4_LOAD_ALU_CONSTANT = 0x2F, PM4_DRAW_INDX_BIN = 0x34, PM4_DRAW_INDX_2_BIN = 0x35,
  PM4_DRAW_INDX_2 = 0x36, PM4_INDIRECT_BUFFER_PFD = 0x37, PM4_INVALIDATE_STATE = 0x3B,
  PM4_WAIT_REG_MEM = 0x3C, PM4_MEM_WRITE = 0x3D, PM4_REG_TO_MEM = 0x3E, PM4_INDIRECT_BUFFER = 0x3F,
  PM4_COND_EXEC = 0x44, PM4_COND_WRITE = 0x45, PM4_EVENT_WRITE = 0x46, PM4_ME_INIT = 0x48,
  PM4_SET_SHADER_BASES = 0x4A, PM4_SET_BIN_BASE_OFFSET = 0x4B, PM4_SET_BIN_MASK = 0x50,
  PM4_SET_BIN_SELECT = 0x51, PM4_WAIT_REG_EQ = 0x52, PM4_WAIT_REG_GTE = 0x53, PM4_INTERRUPT = 0x54,
  PM4_SET_CONSTANT2 = 0x55, PM4_SET_SHADER_CONSTANTS = 0x56, PM4_EVENT_WRITE_SHD = 0x58,
  PM4_EVENT_WRITE_CFL = 0x59, PM4_EVENT_WRITE_EXT = 0x5A, PM4_EVENT_WRITE_ZPD = 0x5B,
  PM4_WAIT_UNTIL_READ = 0x5C, PM4_WAIT_IB_PFD_COMPLETE = 0x5D, PM4_CONTEXT_UPDATE = 0x5E,
  PM4_SET_BIN_MASK_LO = 0x60, PM4_SET_BIN_MASK_HI = 0x61, PM4_SET_BIN_SELECT_LO = 0x62,
  PM4_SET_BIN_SELECT_HI = 0x63, PM4_XE_SWAP = 0x64,
};

// GPU register indices we act on.
enum : uint32_t {
  REG_CP_RB_BASE = 0x01C0, REG_CP_RB_CNTL = 0x01C1, REG_CP_RB_RPTR_ADDR = 0x01C3,
  REG_CP_RB_RPTR = 0x01C4, REG_CP_RB_WPTR = 0x01C5, REG_CP_RB_RPTR_WR = 0x01C7,
  REG_SCRATCH_UMSK = 0x01DC, REG_SCRATCH_ADDR = 0x01DD, REG_CP_INT_STATUS = 0x01F3,
  REG_CP_INT_ACK = 0x01F4, REG_RBBM_STATUS = 0x05D0, REG_SCRATCH_REG0 = 0x0578,
  REG_WAIT_UNTIL = 0x05C8, REG_COHER_STATUS_HOST = 0x0A31, REG_VGT_EVENT_INITIATOR = 0x21F9,
  REG_D1MODE_V_COUNTER = 0x194C, REG_D1MODE_VBLANK_STATUS = 0x1951,
  REG_D1MODE_VIEWPORT_SIZE = 0x1961, REG_RB_EDRAM_TIMING = 0x0F00, REG_RB_BC_CONTROL = 0x0F01,
};

constexpr uint32_t kSwapSignature = 0x53574150;  // 'SWAP'

inline uint32_t Swap32(uint32_t v) { return __builtin_bswap32(v); }
bool BatchableRegisters(uint32_t index, uint32_t count) {
  return (index >= 0x2000 && index < 0x2800 && count <= 0x2800 - index) ||
         (index >= 0x4000 && index < 0x4928 && count <= 0x4928 - index);
}
inline uint32_t GpuSwap(uint32_t value, uint32_t endian) {
  switch (endian & 3) {
    case 1: return ((value & 0x00FF00FF) << 8) | ((value & 0xFF00FF00) >> 8);  // 8in16
    case 2: return __builtin_bswap32(value);                                     // 8in32
    case 3: return (value << 16) | (value >> 16);                                // 16in32
    default: return value;
  }
}

}  // namespace

CommandProcessor::CommandProcessor(IRenderer* renderer) : renderer_(renderer) {
  auto enabled = [](const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] == '1';
  };
  const char* batch_override = std::getenv("XN_NATIVE_GFX_BATCH");
  batch_registers_ = batch_override ? batch_override[0] == '1' : enabled("XN_NATIVE_GFX");
  draw_state_batch_ = batch_registers_ && enabled("XN_CP_DRAW_STATE_BATCH");
  shader_load_requires_registers_ = !renderer_ || renderer_->RequiresRegistersForShaderLoad();
  if (batch_registers_) REXGPU_INFO("native graphics: host-endian register batching enabled");
  if (draw_state_batch_) REXGPU_INFO("native graphics: fused draw state batch enabled");
}
CommandProcessor::~CommandProcessor() { Stop(); }

void CommandProcessor::MarkRegisterBatch(uint32_t index, uint32_t count) {
  if (!count) return;
  auto mark = [&](auto& dirty, uint32_t base) {
    const uint32_t first = (index - base) / 16;
    const uint32_t last = (index - base + count - 1) / 16;
    for (uint32_t block = first; block <= last; ++block)
      dirty[block / 64] |= uint64_t(1) << (block % 64);
  };
  if (index < 0x2800) mark(dirty_draw_state_, 0x2000);
  else mark(dirty_constants_, 0x4000);
  register_batch_pending_ = true;
}

std::unique_lock<std::recursive_mutex> CommandProcessor::LockRegisterBatch() {
  std::unique_lock lock(register_batch_mutex_, std::defer_lock);
  if (batch_registers_) lock.lock();
  return lock;
}

void CommandProcessor::FlushRegisterBatch() {
  if (!batch_registers_) return;
  auto state_lock = LockRegisterBatch();
  FlushRegisterBatchLocked();
}

void CommandProcessor::FlushRegisterBatchLocked() {
  DeliverRegisterBatchLocked(nullptr);
}

void CommandProcessor::DrawLocked(const DrawPacket& draw) {
  if (draw_state_batch_) {
    DeliverRegisterBatchLocked(&draw);
  } else {
    FlushRegisterBatchLocked();
    renderer_->OnDraw(draw, regs_);
  }
}

void CommandProcessor::DeliverRegisterBatchLocked(const DrawPacket* draw) {
  // SetRegister/SetConstants and register-consuming callbacks already own the
  // state mutex. Do not reacquire it recursively for an empty or dirty flush.
  if (!register_batch_pending_) {
    if (draw) renderer_->OnDraw(*draw, regs_);
    return;
  }
  std::array<RegisterRange, 128 + 147> ranges;
  size_t count = 0;
  auto append = [&](auto& dirty, uint32_t base, uint32_t limit) {
    for (uint32_t word = 0; word < dirty.size(); ++word) {
      uint64_t bits = dirty[word];
      dirty[word] = 0;
      while (bits) {
        const uint32_t bit = std::countr_zero(bits);
        bits &= bits - 1;
        const uint32_t index = base + (word * 64 + bit) * 16;
        const uint32_t words = std::min(16u, limit - index);
        if (count && ranges[count - 1].index + ranges[count - 1].count == index)
          ranges[count - 1].count += words;
        else ranges[count++] = {index, words};
      }
    }
  };
  append(dirty_draw_state_, 0x2000, 0x2800);
  append(dirty_constants_, 0x4000, 0x4928);
  register_batch_pending_ = false;
  const auto batch = std::span(ranges.data(), count);
  if (draw) renderer_->OnDrawWithRegisterBatch(*draw, regs_, batch);
  else renderer_->OnRegisterBatch(regs_, batch);
}

uint32_t CommandProcessor::ReadPhysical32(uint32_t physical) const {
  uint32_t v;
  std::memcpy(&v, K().arena.TranslatePhysical(physical), 4);
  return Swap32(v);
}
void CommandProcessor::WritePhysical32(uint32_t physical, uint32_t value) {
  uint32_t v = Swap32(value);
  std::memcpy(K().arena.TranslatePhysical(physical), &v, 4);
}

void CommandProcessor::Start() {
  running_ = true;
  worker_ = CreateHostWorker("gpu-cp", [this](GuestThread& t) { WorkerMain(t); });
  vsync_ = CreateHostWorker("gpu-vsync", [this](GuestThread& t) { VsyncMain(t); });
}

void CommandProcessor::Stop() {
  if (!running_.exchange(false)) return;
  kick_cv_.notify_all();
}

void CommandProcessor::InitializeRingBuffer(uint32_t physical_address, uint32_t size_log2) {
  ring_base_ = physical_address & 0x1FFFFFFF;
  ring_size_dwords_ = (1u << (size_log2 + 3)) / 4;
  read_index_ = 0;
  write_index_ = 0;
  REXGPU_INFO("ring buffer at physical {:#010x}, {} dwords", ring_base_, ring_size_dwords_);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t physical_address, uint32_t block_size_log2) {
  writeback_ptr_ = physical_address & 0x1FFFFFFF;
  REXGPU_INFO("read pointer write-back at physical {:#010x} (block {})", writeback_ptr_,
              block_size_log2);
  WriteBackReadPointer();
}

void CommandProcessor::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  std::lock_guard lock(interrupt_mutex_);
  interrupt_callback_ = callback;
  interrupt_user_data_ = user_data;
  REXGPU_INFO("graphics interrupt callback {:#010x} user_data {:#010x}", callback, user_data);
}

uint32_t CommandProcessor::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case REG_RB_EDRAM_TIMING: return 0x08100748;
    case REG_RB_BC_CONTROL: return 0x0000200E;
    case REG_D1MODE_V_COUNTER: return K().config.display_height;
    case REG_D1MODE_VBLANK_STATUS: return 1;  // "in vblank"
    case REG_D1MODE_VIEWPORT_SIZE:
      return (K().config.display_width << 16) | K().config.display_height;
    case REG_CP_RB_RPTR: return read_index_;
    case REG_RBBM_STATUS: return 0;  // idle
    default: break;
  }
  auto state_lock = LockRegisterBatch();
  if (r == REG_COHER_STATUS_HOST) return regs_[r] & ~0x80000000u;
  return regs_[r];
}

void CommandProcessor::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;
  if (r == REG_CP_RB_WPTR) {
    write_index_.store(value % (ring_size_dwords_ ? ring_size_dwords_ : 1), std::memory_order_release);
    kick_cv_.notify_one();
    return;
  }
  // MMIO state writes are outside the CP stream: preserve their immediate
  // callback contract rather than deferring them into an unrelated draw.
  auto state_lock = LockRegisterBatch();
  FlushRegisterBatchLocked();
  SetRegister(r, value);
  FlushRegisterBatchLocked();
}

void CommandProcessor::SetRegister(uint32_t index, uint32_t value) {
  auto state_lock = LockRegisterBatch();
  if (index >= GpuRegisterFile::kCount) return;
  if (batch_registers_ && BatchableRegisters(index, 1)) {
    regs_.values[index] = value;
    MarkRegisterBatch(index, 1);
    return;
  }
  FlushRegisterBatchLocked();
  regs_.values[index] = value;
  if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8) {
    // Scratch registers mirror to memory when their umask bit is set; D3D
    // uses this for fences.
    uint32_t n = index - REG_SCRATCH_REG0;
    if ((regs_[REG_SCRATCH_UMSK] >> n) & 1) {
      uint32_t addr = (regs_[REG_SCRATCH_ADDR] & 0x1FFFFFFF) + n * 4;
      WritePhysical32(addr, value);
    }
  } else if (index == REG_COHER_STATUS_HOST) {
    regs_.values[index] = value & ~0x80000000u;  // coherency requests complete instantly
  }
  renderer_->OnRegisterWrite(index, regs_.values[index]);
}

void CommandProcessor::WriteBackReadPointer() {
  FlushRegisterBatch();
  if (writeback_ptr_) WritePhysical32(writeback_ptr_, read_index_);
}

void CommandProcessor::SetConstants(uint32_t index, const uint32_t* words, uint32_t count) {
  auto state_lock = LockRegisterBatch();
  if (index >= GpuRegisterFile::kCount) return;
  count = std::min(count, GpuRegisterFile::kCount - index);
  // XN_TRACE_CONST=<vertex float constant index>: log every packet whose write
  // range covers that constant, with the values it delivers. Answers whether a
  // constant that reads back as zero at draw time was never written, or was
  // written and then lost.
  static const char* const trace_const = std::getenv("XN_TRACE_CONST");
  if (const char* tc = trace_const) {
    const uint32_t traced = 0x4000 + uint32_t(std::atoi(tc)) * 4;
    if (count && index <= traced + 3 && index + count > traced) {
      const uint32_t first = traced > index ? traced - index : 0;
      REXLOG_INFO("[constwrite] c{} index={:#x} count={} via {} from {:#x} -> ({:g},{:g},{:g},{:g})",
                  std::atoi(tc), index, count, const_src_, const_src_addr_,
                  first + 0 < count ? std::bit_cast<float>(Swap32(words[first + 0])) : 0.f,
                  first + 1 < count ? std::bit_cast<float>(Swap32(words[first + 1])) : 0.f,
                  first + 2 < count ? std::bit_cast<float>(Swap32(words[first + 2])) : 0.f,
                  first + 3 < count ? std::bit_cast<float>(Swap32(words[first + 3])) : 0.f);
    }
  }
  if (!count) return;
  // Draw-state and ALU/fetch/bool/loop storage have no write side effects. Deliver
  // these packets once instead of taking the renderer mutex for every word
  // and then sending the same constants again. Other ranges retain scratch,
  // coherency and display-register handling through SetRegister.
  if ((index >= 0x2000 && index + count <= 0x2800) ||
      (index >= 0x4000 && index + count <= 0x4928)) {
    for (uint32_t i = 0; i < count; ++i) regs_.values[index + i] = Swap32(words[i]);
    if (batch_registers_) {
      MarkRegisterBatch(index, count);
      return;
    }
  } else {
    for (uint32_t i = 0; i < count; ++i) SetRegister(index + i, Swap32(words[i]));
  }
  FlushRegisterBatchLocked();
  renderer_->OnConstants(index, words, count);
}

void CommandProcessor::DispatchInterrupt(uint32_t source, uint32_t cpu) {
  uint32_t callback, data;
  {
    std::lock_guard lock(interrupt_mutex_);
    callback = interrupt_callback_;
    data = interrupt_user_data_;
  }
  if (!callback) return;
  if (GuestThread* t = CurrentThread()) t->SetActiveCpu(uint8_t(cpu));
  CallGuest(callback, {source, data});
}

void CommandProcessor::VsyncMain(GuestThread& self) {
  using namespace std::chrono;
  auto next = steady_clock::now();
  uint64_t n = 0;
  while (running_) {
    const auto period = nanoseconds(1000000000 / FrameRate());
    next += period;
    // A late interrupt must not cause a burst of catch-up frames.
    if (next < steady_clock::now()) next = steady_clock::now();
    std::this_thread::sleep_until(next);
    DispatchInterrupt(0, 2);  // vblank
    if (++n == 1 || n % 600 == 0) REXGPU_DEBUG("vblank #{} (swaps {}, draws {})", n, swaps_.load(), draws_.load());
  }
}

void CommandProcessor::WorkerMain(GuestThread& self) {
  while (running_) {
    uint32_t write_index;
    {
      std::unique_lock lock(kick_mutex_);
      kick_cv_.wait_for(lock, std::chrono::milliseconds(2),
                        [&] { return !running_ || write_index_.load() != read_index_; });
      write_index = write_index_.load();
    }
    if (!running_) break;
    if (write_index != read_index_ && ring_size_dwords_) {
      read_index_ = ExecuteRange(read_index_, write_index);
      WriteBackReadPointer();
    }
  }
}

uint32_t CommandProcessor::ExecuteRange(uint32_t start, uint32_t end) {
  // Packets may straddle the ring boundary. SG submits a three-dword IB at
  // indices 8190, 8191, 0; dropping its wrapped length loses the fence inside
  // the IB and leaves the guest waiting forever with an apparently empty ring.
  uint32_t index = start;
  while (index != end) {
    const uint32_t* words = reinterpret_cast<const uint32_t*>(
        K().arena.TranslatePhysical(ring_base_ + index * 4));
    const uint32_t header = Swap32(words[0]);
    const uint32_t type = header >> 30;
    const uint32_t length = type == 2 ? 1 : type == 1 ? 3 : ((header >> 16) & 0x3FFF) + 2;
    const uint32_t available = (end + ring_size_dwords_ - index) % ring_size_dwords_;
    if (length > available) break;  // wait for the rest before advancing RPTR
    if (length > ring_size_dwords_ - index) {
      std::vector<uint32_t> packet(length);
      const uint32_t tail = ring_size_dwords_ - index;
      std::memcpy(packet.data(), words, tail * 4);
      std::memcpy(packet.data() + tail, K().arena.TranslatePhysical(ring_base_),
                  (length - tail) * 4);
      ExecutePacket(packet.data(), length, 0);
    } else {
      ExecutePacket(words, length, 0);
    }
    index = (index + length) % ring_size_dwords_;
  }
  return index;
}

bool CommandProcessor::ExecuteBuffer(uint32_t physical_address, uint32_t dword_count, int depth) {
  if (depth > 8) { REXGPU_WARN("indirect buffer nesting too deep"); return false; }
  const uint32_t* words = reinterpret_cast<const uint32_t*>(
      K().arena.TranslatePhysical(physical_address & 0x1FFFFFFF));
  uint32_t index = 0;
  while (index < dword_count) {
    uint32_t consumed = ExecutePacket(words + index, dword_count - index, depth);
    if (!consumed) return false;
    index += consumed;
  }
  return true;
}

uint32_t CommandProcessor::ExecutePacket(const uint32_t* words, uint32_t available, int depth) {
  if (!available) return 0;
  uint32_t packet = Swap32(words[0]);
  uint32_t type = packet >> 30;
  switch (type) {
    case 0: {  // register writes
      uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      uint32_t index = packet & 0x7FFF;
      bool one_reg = (packet >> 15) & 1;
      if (count + 1 > available) return available;
      if (!one_reg && ((index >= 0x2000 && index + count <= 0x2800) ||
                       (index >= 0x4000 && index + count <= 0x4928))) {
        SetConstants(index, words + 1, count);
        return count + 1;
      }
      for (uint32_t i = 0; i < count; i++) {
        uint32_t target = one_reg ? index : index + i;
        uint32_t value = Swap32(words[1 + i]);
        if (target == REG_CP_RB_WPTR) continue;
        SetRegister(target, value);
      }
      return count + 1;
    }
    case 1: {  // two register writes
      uint32_t a = packet & 0x7FF, b = (packet >> 11) & 0x7FF;
      if (available < 3) return available;
      SetRegister(a, Swap32(words[1]));
      SetRegister(b, Swap32(words[2]));
      return 3;
    }
    case 2:  // NOP
      return 1;
    case 3: {
      uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      uint32_t opcode = (packet >> 8) & 0x7F;
      if (count + 1 > available) return available;
      // Bit 0 predicates the complete packet against the 64-bit bin mask.
      // This applies to control packets and indirect buffers as well as draws.
      if ((packet & 1) && (!(bin_select_ & bin_mask_) || opcode == PM4_XE_SWAP))
        return count + 1;
      ExecuteType3(opcode, words + 1, count, depth);
      return count + 1;
    }
  }
  return 1;
}

bool CommandProcessor::EvaluateWait(uint32_t wait_info, uint32_t poll_addr, uint32_t ref, uint32_t mask) {
  uint32_t value;
  if (wait_info & 0x10) {
    value = GpuSwap(ReadPhysical32(poll_addr & ~3u), poll_addr & 3);
    // ReadPhysical32 already byte-swapped from guest BE; GpuSwap with endian 2
    // (the usual "8in32") would undo that, so apply it to the raw word instead.
    uint32_t raw;
    std::memcpy(&raw, K().arena.TranslatePhysical(poll_addr & ~3u & 0x1FFFFFFF), 4);
    value = GpuSwap(raw, poll_addr & 3);
  } else {
    value = ReadRegister((poll_addr & 0x7FFF) * 4);
  }
  switch (wait_info & 7) {
    case 0: return false;
    case 1: return (value & mask) < ref;
    case 2: return (value & mask) <= ref;
    case 3: return (value & mask) == ref;
    case 4: return (value & mask) != ref;
    case 5: return (value & mask) >= ref;
    case 6: return (value & mask) > ref;
    default: return true;
  }
}

void CommandProcessor::ExecuteType3(uint32_t opcode, const uint32_t* d, uint32_t count, int depth) {
  auto w = [&](uint32_t i) { return i < count ? Swap32(d[i]) : 0u; };
  switch (opcode) {
    case PM4_NOP:
      break;
    case PM4_ME_INIT:
      break;
    case PM4_INTERRUPT: {
      FlushRegisterBatch();
      uint32_t cpu_mask = w(0);
      for (uint32_t n = 0; n < 6; n++)
        if (cpu_mask & (1u << n)) DispatchInterrupt(1, n);
      break;
    }
    case PM4_XE_SWAP: {
      if (w(0) != kSwapSignature) { REXGPU_WARN("XE_SWAP without signature"); break; }
      uint32_t frontbuffer = w(1), width = w(2), height = w(3);
      FlushRegisterBatch();
      swaps_.fetch_add(1);
      renderer_->OnSwap(frontbuffer, width, height);
      ++event_counter_;
      break;
    }
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD: {
      uint32_t ptr = w(0) & 0x1FFFFFFF;
      uint32_t len = w(1) & 0xFFFFF;
      ExecuteBuffer(ptr, len, depth + 1);
      break;
    }
    case PM4_WAIT_REG_MEM: {
      FlushRegisterBatch();
      uint32_t wait_info = w(0), poll_addr = w(1), ref = w(2), mask = w(3), wait = w(4);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      while (!EvaluateWait(wait_info, poll_addr, ref, mask)) {
        if (std::chrono::steady_clock::now() > deadline) {
          REXGPU_WARN("WAIT_REG_MEM timed out ({} {:#010x} ref {:#x} mask {:#x})",
                      (wait_info & 0x10) ? "mem" : "reg", poll_addr, ref, mask);
          break;
        }
        if (wait >= 0x100) std::this_thread::sleep_for(std::chrono::microseconds(50));
        else std::this_thread::yield();
      }
      break;
    }
    case PM4_WAIT_REG_EQ:
    case PM4_WAIT_REG_GTE:
    case PM4_WAIT_FOR_IDLE:
    case PM4_WAIT_UNTIL_READ:
    case PM4_WAIT_IB_PFD_COMPLETE:
      break;
    case PM4_REG_RMW: {
      auto state_lock = LockRegisterBatch();
      uint32_t rmw = w(0), and_mask = w(1), or_mask = w(2);
      uint32_t index = rmw & 0x1FFF;
      uint32_t value = regs_[index];
      uint32_t and_v = (rmw >> 31) & 1 ? regs_[and_mask & 0x1FFF] : and_mask;
      uint32_t or_v = (rmw >> 30) & 1 ? regs_[or_mask & 0x1FFF] : or_mask;
      SetRegister(index, (value & and_v) | or_v);
      break;
    }
    case PM4_REG_TO_MEM: {
      auto state_lock = LockRegisterBatch();
      FlushRegisterBatchLocked();
      uint32_t reg = w(0) & 0x7FFF, addr = w(1);
      uint32_t raw = GpuSwap(regs_[reg], addr & 3);
      std::memcpy(K().arena.TranslatePhysical(addr & ~3u & 0x1FFFFFFF), &raw, 4);
      break;
    }
    case PM4_MEM_WRITE: {
      FlushRegisterBatch();
      uint32_t addr = w(0);
      for (uint32_t i = 1; i < count; i++) {
        uint32_t raw = GpuSwap(w(i), addr & 3);
        std::memcpy(K().arena.TranslatePhysical(addr & ~3u & 0x1FFFFFFF), &raw, 4);
        addr += 4;
      }
      break;
    }
    case PM4_COND_WRITE: {
      uint32_t wait_info = w(0), poll_addr = w(1), ref = w(2), mask = w(3), write_addr = w(4), data = w(5);
      if (EvaluateWait(wait_info, poll_addr, ref, mask)) {
        if (wait_info & 0x100) {
          FlushRegisterBatch();
          uint32_t raw = GpuSwap(data, write_addr & 3);
          std::memcpy(K().arena.TranslatePhysical(write_addr & ~3u & 0x1FFFFFFF), &raw, 4);
        } else {
          SetRegister(write_addr & 0x7FFF, data);
        }
      }
      break;
    }
    case PM4_EVENT_WRITE: {
      auto state_lock = LockRegisterBatch();
      uint32_t initiator = w(0) & 0x3F;
      SetRegister(REG_VGT_EVENT_INITIATOR, initiator);
      FlushRegisterBatchLocked();
      renderer_->OnEvent(initiator, regs_);
      break;
    }
    case PM4_EVENT_WRITE_SHD: {
      auto state_lock = LockRegisterBatch();
      uint32_t initiator = w(0), addr = w(1), value = w(2);
      SetRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      uint32_t data = (initiator >> 31) & 1 ? event_counter_ : value;
      uint32_t raw = GpuSwap(data, addr & 3);
      std::memcpy(K().arena.TranslatePhysical(addr & ~3u & 0x1FFFFFFF), &raw, 4);
      FlushRegisterBatchLocked();
      renderer_->OnEvent(initiator & 0x3F, regs_);
      break;
    }
    case PM4_EVENT_WRITE_CFL:
      break;
    case PM4_EVENT_WRITE_EXT: {
      FlushRegisterBatch();
      uint32_t addr = w(1);
      // Screen extents: min x, max x, min y, max y (in 8-pixel units), min z, max z.
      uint16_t extents[] = {0, 2560 >> 3, 0, 2560 >> 3, 0, 1};
      uint8_t* p = K().arena.TranslatePhysical(addr & ~3u & 0x1FFFFFFF);
      for (size_t i = 0; i < 6; i++) {
        uint16_t v = __builtin_bswap16(extents[i]);
        std::memcpy(p + i * 2, &v, 2);
      }
      break;
    }
    case PM4_EVENT_WRITE_ZPD:
      break;
    case PM4_DRAW_INDX: {
      auto state_lock = LockRegisterBatch();
      uint32_t initiator = w(1);
      DrawPacket draw{};
      draw.registers_notified = true;
      draw.opcode = opcode;
      draw.prim_type = initiator & 0x3F;
      draw.index_count = initiator >> 16;
      draw.source = (initiator >> 6) & 3;
      if (draw.source == 0 && count >= 4) {
        draw.index_base = w(2) & 0x1FFFFFFF;
        uint32_t size = w(3);
        draw.index_size = size & 0x00FFFFFF;
        draw.index_32bit = (initiator >> 11) & 1;
        draw.index_endian = size >> 30;
      }
      draws_.fetch_add(1);
      DrawLocked(draw);
      break;
    }
    case PM4_DRAW_INDX_2: {
      auto state_lock = LockRegisterBatch();
      uint32_t initiator = w(0);
      DrawPacket draw{};
      draw.registers_notified = true;
      draw.opcode = opcode;
      draw.prim_type = initiator & 0x3F;
      draw.index_count = initiator >> 16;
      draw.source = (initiator >> 6) & 3;
      draw.index_32bit = (initiator >> 11) & 1;
      draw.immediate = d + 1;
      draw.immediate_words = count - 1;
      draws_.fetch_add(1);
      DrawLocked(draw);
      break;
    }
    case PM4_DRAW_INDX_BIN:
    case PM4_DRAW_INDX_2_BIN:
      break;
    case PM4_SET_CONSTANT: {
      uint32_t offset_type = w(0);
      uint32_t index = offset_type & 0x7FF;
      uint32_t type = (offset_type >> 16) & 0xFF;
      switch (type) {
        case 0: index += 0x4000; break;  // ALU
        case 1: index += 0x4800; break;  // FETCH
        case 2: index += 0x4900; break;  // BOOL
        case 3: index += 0x4908; break;  // LOOP
        case 4: index += 0x2000; break;  // REGISTERS
        default: return;
      }
      const_src_ = "SET_CONSTANT";
      const_src_addr_ = 0;
      if (count > 1) SetConstants(index, d + 1, count - 1);
      break;
    }
    case PM4_SET_CONSTANT2:
    case PM4_SET_SHADER_CONSTANTS: {
      uint32_t offset_type = w(0);
      uint32_t index = offset_type & 0xFFFF;
      const_src_ = "SET_SHADER_CONSTANTS";
      const_src_addr_ = 0;
      if (count > 1) SetConstants(index, d + 1, count - 1);
      break;
    }
    case PM4_LOAD_ALU_CONSTANT: {
      uint32_t address = w(0) & 0x3FFFFFFF;
      uint32_t offset_type = w(1);
      uint32_t index = offset_type & 0x7FF;
      uint32_t size = w(2) & 0xFFF;
      uint32_t type = (offset_type >> 16) & 0xFF;
      switch (type) {
        case 0: index += 0x4000; break;
        case 1: index += 0x4800; break;
        case 2: index += 0x4900; break;
        case 3: index += 0x4908; break;
        case 4: index += 0x2000; break;
        default: return;
      }
      const uint32_t* src = reinterpret_cast<const uint32_t*>(K().arena.TranslatePhysical(address));
      const_src_ = "LOAD_ALU_CONSTANT";
      const_src_addr_ = address;
      SetConstants(index, src, size);
      break;
    }
    case PM4_IM_LOAD: {
      uint32_t addr_type = w(0);
      uint32_t shader_type = addr_type & 3;
      uint32_t addr = addr_type & ~3u;
      uint32_t size = w(1) & 0xFFFF;  // dwords
      if (shader_load_requires_registers_) FlushRegisterBatch();
      renderer_->OnShaderLoad(shader_type, addr & 0x1FFFFFFF, size,
                              reinterpret_cast<const uint32_t*>(K().arena.TranslatePhysical(addr)));
      break;
    }
    case PM4_IM_LOAD_IMMEDIATE: {
      uint32_t shader_type = w(0) & 3;
      uint32_t size = w(1) & 0xFFFF;
      if (shader_load_requires_registers_) FlushRegisterBatch();
      renderer_->OnShaderLoad(shader_type, 0, std::min(size, count - 2), d + 2);
      break;
    }
    case PM4_INVALIDATE_STATE:
    case PM4_SET_STATE:
    case PM4_VIZ_QUERY:
    case PM4_IM_STORE:
    case PM4_LOAD_CONSTANT_CONTEXT:
    case PM4_SET_SHADER_BASES:
    case PM4_SET_BIN_BASE_OFFSET:
    case PM4_CONTEXT_UPDATE:
    case PM4_COND_EXEC:
      break;
    case PM4_SET_BIN_MASK:
      bin_mask_ = (uint64_t(w(0)) << 32) | w(1);
      break;
    case PM4_SET_BIN_SELECT:
      bin_select_ = (uint64_t(w(0)) << 32) | w(1);
      break;
    case PM4_SET_BIN_MASK_LO:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | w(0);
      break;
    case PM4_SET_BIN_MASK_HI:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(w(0)) << 32);
      break;
    case PM4_SET_BIN_SELECT_LO:
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | w(0);
      break;
    case PM4_SET_BIN_SELECT_HI:
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(w(0)) << 32);
      break;
    default:
      if (unknown_logged_++ < 20) REXGPU_WARN("unknown PM4 type-3 opcode {:#x} ({} dwords)", opcode, count);
      break;
  }
}

}  // namespace xn
