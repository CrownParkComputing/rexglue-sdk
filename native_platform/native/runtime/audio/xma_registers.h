// xenon-native: the XMA decoder's MMIO register block.
//
// Two ways a title can drive the XMA hardware, and this runtime answers both:
//
//   through the XMA* kernel imports  - XMAInitializeContext, XMAEnableContext,
//     XMAIsOutputBufferValid and the rest (runtime/audio/imports_audio.cpp).
//     Geometry Wars 1 does this.
//   through the register block       - the title allocates contexts with
//     XMACreateContext, reads the context-array address out of a register,
//     turns a context pointer into an index, and kicks by setting one bit.
//     Geometry Wars 2 does this, and imports only XMACreateContext and
//     XMAReleaseContext.
//
// Either way the work is the same XmaEngine: this class is a register
// decoder, not a second decoder.
//
// Endianness. The block is little-endian on the bus, so the guest reaches it
// with lwbrx/stwbrx (or, for a constant, a pre-swapped stwx). The MMIO seam
// hands us the value a plain lwz/stw would see, so every register here is
// byte-swapped on the way in and on the way out. Getting this backwards is
// silent: the context-array address comes back nonsense and every context
// index is wrong.
//
// Registers, as word indices into the window (address = base + index * 4):
//
//   0x600  CONTEXT_ARRAY_ADDRESS   r/w  physical address of the 64-byte context table
//   0x601  CONTROL                 w    0 then 3 is the decoder reset the guest
//                                       issues when a context will not go idle
//   0x606  CURRENT_CONTEXT_INDEX   r    0x200 | index while that context is being
//                                       decoded, 0 when idle. The guest XORs with
//                                       0x200 and compares against its own indices,
//                                       so 0 can never look like a match.
//   0x650  KICK bitmask            w    one bit per context, 32 contexts per dword
//   0x690  LOCK bitmask            w    one bit per context, 32 contexts per dword
//   0x6A0  CLEAR bitmask           w    one bit per context, 32 contexts per dword
//
// The three bitmask groups and their names come from ReXGlue's XMA register
// table (rex/audio/xma/register_table.inc). Which of them a title uses is not
// a matter of taste:
//
//   Geometry Wars 2 writes CLEAR when a context is created (the context is
//     all zero at that point), KICK after every refill, and LOCK to stop one.
//     Measured on the running title (XN_XMA_TRACE_EVENTS=1): at 0.418 ms
//     0x650 gets 1<<bit with the context fully armed (input 0: 1 packet at
//     physical 0x11db4000, valid; output ring 8 blocks, valid), 0x690 follows
//     11 us later, and the title itself zeroes the input fields after that.
//     An earlier reading of the disassembly missed the kick because its store
//     address is computed (sub_821469F8 does (index >> 5) + 0x1FFA86A0 then
//     scales by 4), not a literal.
//   Boom Boom Rocket writes CLEAR at creation (0x82142888), LOCK on stop
//     (0x82142958) and **KICK after every refill** (0x82142BE0, right after a
//     dcbf sweep of the output ring and the vector copy-back). Its per-refill
//     "go" signal is KICK.
//
// So KICK and CLEAR both have to start the context and LOCK has to stop it.
// Until this file modelled KICK, a write to 0x650 fell through to the
// unmodelled-register warning and was dropped, which for a title with Boom
// Boom Rocket's shape means every refill after the first is silently ignored.
//
// The kick also has to *decode* before it returns: the title's own lock lands
// 11 us after the kick, so a pass that waits for the worker thread's 2 ms
// sweep never happens. See the WorkContext call in ForEachContext.
#pragma once
#include <cstdint>

namespace xn::audio {

class XmaEngine;

class XmaRegisterFile {
 public:
  // The window the console puts this block at. A title installs it with
  // MMIOHandler::RegisterRange(kWindowBase, kWindowMask, kWindowSize, ...).
  static constexpr uint32_t kWindowBase = 0x7FEA0000;
  static constexpr uint32_t kWindowMask = 0xFFFF0000;
  static constexpr uint32_t kWindowSize = 0x10000;

  // Word indices.
  static constexpr uint32_t kRegContextArrayAddress = 0x600;
  static constexpr uint32_t kRegControl = 0x601;
  static constexpr uint32_t kRegCurrentContextIndex = 0x606;
  static constexpr uint32_t kRegKickBase = 0x650;
  static constexpr uint32_t kRegLockBase = 0x690;
  static constexpr uint32_t kRegClearBase = 0x6A0;
  static constexpr uint32_t kRegBitmaskWords = 10;  // 320 contexts
  // The "a context is being decoded" bit the index register carries.
  static constexpr uint32_t kCurrentContextBusyBit = 0x200;

  // context_table_physical is what MmGetPhysicalAddress() returns for the
  // engine's context table: the guest subtracts it from the physical address
  // of its own context to get the index, so it must be the same number the
  // kernel would hand back, not the virtual address.
  void Setup(XmaEngine* engine, uint32_t context_table_physical);

  uint32_t Read(uint32_t address);
  void Write(uint32_t address, uint32_t value);

  // Counters, for a title's log line at shutdown.
  //
  // kicks() is "context starts", from KICK or CLEAR, because that is what it
  // counted before KICK was modelled and two titles already print it. The two
  // registers are also counted separately: both start a context, and which one
  // a title leans on is how you tell its driver generation without reading its
  // disassembly again.
  uint64_t kicks() const { return kicks_ + clears_; }
  uint64_t kick_writes() const { return kicks_; }
  uint64_t clear_writes() const { return clears_; }
  uint64_t locks() const { return locks_; }
  uint64_t resets() const { return resets_; }

 private:
  uint32_t ContextAddress(uint32_t index) const;
  // Walks one bitmask dword. Returns how many bits were set and acted on.
  uint64_t ForEachContext(uint32_t base_index, uint32_t value, bool enable, const char* what);

  XmaEngine* engine_ = nullptr;
  uint32_t table_physical_ = 0;
  uint64_t kicks_ = 0, clears_ = 0, locks_ = 0, resets_ = 0;
};

}  // namespace xn::audio
