// MCLA keeps more than 32 XMA voices live. Exercise the complete hardware
// context table, exhaustion/reuse and the last MMIO mask word without assets.
#include <algorithm>
#include <cstdio>
#include <vector>

#include "audio/xma.h"
#include "audio/xma_registers.h"

using namespace xn::audio;

struct Memory : XmaMemory {
  std::vector<uint8_t> bytes;
  uint8_t* Physical(uint32_t address) override {
    return address < bytes.size() ? bytes.data() + address : nullptr;
  }
};

int main() {
  int failures = 0;
  auto check = [&](bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
  };
  constexpr uint32_t table = 0x1000, count = 320, size = count * 64;
  Memory memory;
  memory.bytes.resize(table + size + 16, 0xCA);
  XmaEngine engine;
  engine.Setup(&memory, memory.bytes.data() + table, table);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t address = engine.AllocateContext();
    check(address == table + i * 64, "all 320 hardware slots must be available and distinct");
  }
  check(engine.AllocateContext() == 0, "allocation fails only after all hardware slots are used");
  check(engine.Lookup(table + size) == nullptr && engine.Lookup(table + 1) == nullptr,
        "lookup rejects out-of-range and unaligned contexts");
  for (uint32_t i : {31u, 255u, 319u}) {
    const uint32_t address = table + i * 64;
    if (auto* context = engine.Lookup(address)) {
      context->guest()[20] = 0x5A;
      engine.ReleaseContext(address);
      check(engine.AllocateContext() == address, "released slot is reusable");
      check(std::all_of(context->guest(), context->guest() + 64,
                        [](uint8_t b) { return b == 0; }), "reused context is reset");
    }
  }
  XmaRegisterFile registers;
  registers.Setup(&engine, table);
  auto* last = engine.Lookup(table + 319 * 64);
  if (last) {
    last->Enable();
    registers.Write(XmaRegisterFile::kWindowBase + (XmaRegisterFile::kRegLockBase + 9) * 4,
                    __builtin_bswap32(1u << 31));
    check(!last->enabled(), "last MMIO mask bit controls context 319");
  }
  // GW2 drives the hardware entirely through this register block: it arms a
  // context, writes KICK (0x650) for every refill, and LOCK (0x690) to stop
  // it. The committed baseline had KICK at 0x6A0 - which is CLEAR - so the
  // real kick fell through to the unmodelled-register path and was dropped:
  // the context was started once at creation and never again, which is
  // "no sound at all" for a title whose per-refill go signal is KICK.
  // This is the regression for that: a kick must decode, and it must decode
  // on the writing thread, because the title reads the ring microseconds later.
  {
    constexpr uint32_t kIndex = 3, kRing = 0x400, kBlocks = 8;
    const uint32_t address = table + kIndex * 64;
    auto* context = engine.Lookup(address);
    check(context != nullptr, "context 3 exists");
    if (context) {
      // One packet of silence is enough: the decoder only has to run.
      const uint32_t input = uint32_t(memory.bytes.size());
      memory.bytes.resize(input + kXmaPacketBytes, 0);
      // A packet header the frame walker accepts: first frame at bit 32, no skip.
      memory.bytes[input + 0] = 1 << 2;
      memory.bytes[input + 2] = 1;
      std::fill(memory.bytes.begin() + kRing, memory.bytes.begin() + kRing + kBlocks * 256, 0);
      XmaContextData data{};
      data.input_buffer_0_ptr = input;
      data.input_buffer_0_packet_count = 1;
      data.input_buffer_0_valid = 1;
      data.output_buffer_ptr = kRing;
      data.output_buffer_block_count = kBlocks;
      data.output_buffer_valid = 1;
      data.subframe_decode_count = 2;
      data.is_stereo = 1;
      data.sample_rate = 3;
      data.input_buffer_read_offset = kXmaPacketHeaderBits;
      data.Store(context->guest());
      const uint64_t before = context->stats().passes;
      registers.Write(XmaRegisterFile::kWindowBase + XmaRegisterFile::kRegKickBase * 4,
                      __builtin_bswap32(1u << kIndex));
      const uint64_t after = context->stats().passes;
      check(context->enabled(), "KICK at 0x650 enables the context");
      check(after > before, "KICK decodes on the writing thread, not only on the worker sweep");
      check(registers.kicks() > 0, "KICK is counted as a context start");
      // CLEAR (0x6A0) also starts a context - the baseline's mapping - and LOCK
      // stops it. Both must keep working now that KICK has its own address.
      registers.Write(XmaRegisterFile::kWindowBase + XmaRegisterFile::kRegLockBase * 4,
                      __builtin_bswap32(1u << kIndex));
      check(!context->enabled(), "LOCK at 0x690 stops the context");
      registers.Write(XmaRegisterFile::kWindowBase + XmaRegisterFile::kRegClearBase * 4,
                      __builtin_bswap32(1u << kIndex));
      check(context->enabled(), "CLEAR at 0x6A0 starts the context");

      // The silence mechanism, as a negative control. GW2's cycle is
      // kick -> (decode) -> lock ~11 us later -> refill -> kick. With KICK
      // unmodelled the lock still lands, so the context stays disabled and no
      // refill is ever decoded again: the first pass plays and everything after
      // it is silence. Here: lock, refill, and check that without a kick
      // nothing decodes, then that a kick decodes immediately.
      registers.Write(XmaRegisterFile::kWindowBase + XmaRegisterFile::kRegLockBase * 4,
                      __builtin_bswap32(1u << kIndex));
      check(!context->enabled(), "the title's lock leaves the context disabled");
      XmaContextData refill = XmaContextData::Load(context->guest());
      refill.input_buffer_0_valid = 1;
      refill.output_buffer_valid = 1;
      refill.input_buffer_read_offset = kXmaPacketHeaderBits;
      refill.Store(context->guest());
      const uint64_t locked_passes = context->stats().passes;
      for (int i = 0; i < 4; i++) engine.WorkAll();
      check(context->stats().passes == locked_passes,
            "a refill with no kick decodes nothing: this is what silence looked like");
      registers.Write(XmaRegisterFile::kWindowBase + XmaRegisterFile::kRegKickBase * 4,
                      __builtin_bswap32(1u << kIndex));
      check(context->stats().passes > locked_passes, "the kick after a refill decodes it");
    }
  }

  engine.Shutdown();
  check(memory.bytes[table - 1] == 0xCA && memory.bytes[table + size] == 0xCA,
        "context operations stay inside the table");
  std::printf("xma_context_pool_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
