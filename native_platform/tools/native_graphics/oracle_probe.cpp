// Source-only oracle harness. Original audited title bodies and their metadata
// are generated into an ignored local output directory by verify_hooks.py.
#include "oracle_metadata.h"
#include "constant_emitter.h"
#include "forza4_constant_setter.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {
using namespace xn::gfx::adapters;
constexpr uint32_t kDevice = 0x1000, kSource = 0x6000, kStack = 0xD000;
using Memory = std::array<uint8_t, 65536>;
uint64_t equivalence_cases = 0, rejected_cases = 0;
std::mt19937_64 random_bits(0x46525A344D434C41ull);

void CorruptHighGpr(PPCContext& ctx, unsigned reg) {
  constexpr uint64_t high = UINT64_C(0x100000000);
  switch (reg) {
    case 1: ctx.r1.u64 |= high; break;
    case 3: ctx.r3.u64 |= high; break;
    case 4: ctx.r4.u64 |= high; break;
    case 5: ctx.r5.u64 |= high; break;
    case 6: ctx.r6.u64 |= high; break;
    default: std::abort();
  }
}

template <typename T> void Randomize(T& object) {
  auto* bytes = reinterpret_cast<uint8_t*>(&object);
  for (size_t offset = 0; offset < sizeof(object); offset += sizeof(uint64_t)) {
    const uint64_t value = random_bits();
    const size_t count = std::min(sizeof(value), sizeof(object) - offset);
    std::memcpy(bytes + offset, &value, count);
  }
}

bool Native(const OracleHook& hook, PPCContext& ctx, uint8_t* base, uint64_t mapped_size) {
  if (hook.profile == OracleProfile::GroupedEmitter)
    return TryConstantEmitter(ctx, base, mapped_size, {hook.entry_after_save_lr});
  ConstantSetterLayout layout{hook.constants_offset, hook.dirty_offset, 256,
                              hook.profile == OracleProfile::MclaFloat4};
  if (hook.profile == OracleProfile::ForzaFloat4)
    return TryForzaConstantSetter(ctx, base, mapped_size, layout);
  return TryMclaConstantSetter(ctx, base, mapped_size, layout);
}

[[noreturn]] void Fail(const OracleHook& hook, const char* reason,
                       uint32_t count = 0, uint32_t alignment = 0) {
  std::fprintf(stderr, "Oracle %08X: %s (count=%u alignment=%u)\n",
               hook.address, reason, count, alignment);
  std::exit(1);
}

void Equivalent(const OracleHook& hook, const PPCContext& initial, const Memory& seed,
                uint32_t count, uint32_t alignment) {
  alignas(32) Memory original = seed, native = seed;
  PPCContext expected, actual;
  // PPCContext has alignment padding that assignment need not copy.
  std::memcpy(&expected, &initial, sizeof(expected));
  std::memcpy(&actual, &initial, sizeof(actual));
  hook.original(expected, original.data());
  if (!Native(hook, actual, native.data(), native.size()))
    Fail(hook, "supported call rejected", count, alignment);
  if (std::memcmp(&expected, &actual, sizeof(expected))) {
    for (size_t i = 0; i < sizeof(expected); ++i) {
      if (reinterpret_cast<const uint8_t*>(&expected)[i] !=
          reinterpret_cast<const uint8_t*>(&actual)[i]) {
        std::fprintf(stderr, "First context difference at byte %zu\n", i);
        break;
      }
    }
    Fail(hook, "complete context mismatch", count, alignment);
  }
  if (original != native) {
    for (size_t i = 0; i < original.size(); ++i) if (original[i] != native[i]) {
      std::fprintf(stderr, "First memory difference at byte %zu\n", i);
      break;
    }
    Fail(hook, "complete guest memory mismatch", count, alignment);
  }
  ++equivalence_cases;
}

void Rejected(const OracleHook& hook, const PPCContext& initial, const Memory& seed,
              uint64_t mapped_size = sizeof(Memory), bool null_base = false) {
  alignas(32) Memory memory = seed;
  PPCContext ctx;
  std::memcpy(&ctx, &initial, sizeof(ctx));
  if (Native(hook, ctx, null_base ? nullptr : memory.data(), mapped_size))
    Fail(hook, "unsupported call consumed");
  if (std::memcmp(&ctx, &initial, sizeof(ctx)) || memory != seed)
    Fail(hook, "fallback changed context or memory");
  ++rejected_cases;
}

void Setter(const OracleHook& hook) {
  alignas(32) Memory seed;
  Randomize(seed);
  for (uint32_t count = 0; count <= 256; ++count) {
    std::array<uint32_t, 3> starts{0, (256 - count) / 2, 256 - count};
    for (uint32_t index : starts) for (uint32_t alignment = 0; alignment < 16; ++alignment) {
      PPCContext initial;
      Randomize(initial);
      initial.r1.u64 = kStack;
      initial.r3.u64 = kDevice;
      initial.r4.u64 = index;
      initial.r5.u64 = kSource + alignment;
      initial.r6.u64 = count;
      initial.r7.u64 = random_bits();
      Equivalent(hook, initial, seed, count, alignment);
    }
  }
  PPCContext valid{};
  valid.r1.u64 = kStack;
  valid.r3.u64 = kDevice;
  valid.r4.u64 = 0;
  valid.r5.u64 = kSource;
  valid.r6.u64 = 16;
  valid.r7.u64 = 0xA000000000000001ull;
  auto bad = valid;
  Rejected(hook, valid, seed, seed.size(), true);
  Rejected(hook, valid, seed, 4096);
  for (unsigned reg : {3u, 4u, 5u, 6u}) {
    bad = valid;
    CorruptHighGpr(bad, reg);
    Rejected(hook, bad, seed);
  }
  bad = valid; bad.r3.u64 += 4; Rejected(hook, bad, seed);
  bad = valid; bad.r4.u64 = 257; Rejected(hook, bad, seed);
  bad = valid; bad.r6.u64 = 257; Rejected(hook, bad, seed);
  bad = valid; bad.r4.u64 = 250; Rejected(hook, bad, seed);
  bad = valid; bad.r5.u64 = kDevice + hook.constants_offset; Rejected(hook, bad, seed);
  bad = valid; bad.r5.u64 = kDevice + hook.dirty_offset; Rejected(hook, bad, seed);
  bad = valid; bad.r5.u64 = 0x7F000000; Rejected(hook, bad, seed, UINT64_C(0x100000000));
  bad = valid; bad.r3.u64 = 0x7F000000; Rejected(hook, bad, seed, UINT64_C(0x100000000));
  bad = valid; bad.r5.u64 = 0x7EFFFFF0; Rejected(hook, bad, seed, UINT64_C(0x100000000));
  // All unsupported physical-alias cases must reject before touching their
  // high virtual addresses. The host test only maps the small seed above.
  bad = valid; bad.r3.u64 = 0xC0010000;
  bad.r5.u64 = 0xA0010000 + hook.constants_offset;
  Rejected(hook, bad, seed, UINT64_C(0x100000000));
  bad.r5.u64 = 0xE000F000 + hook.constants_offset;
  Rejected(hook, bad, seed, UINT64_C(0x100000000));
  bad.r5.u64 = 0xA0010000 + hook.dirty_offset;
  Rejected(hook, bad, seed, UINT64_C(0x100000000));
  bad = valid; bad.r5.u64 = 0xBFFFFFF0;
  Rejected(hook, bad, seed, UINT64_C(0x100000000));
  bad = valid; bad.r5.u64 = 0xFFFFEFF0;
  Rejected(hook, bad, seed, UINT64_C(0x100000000));
  if (hook.profile == OracleProfile::MclaFloat4) {
    bad = valid; bad.r1.u64 = 7; Rejected(hook, bad, seed);
    bad = valid; bad.r1.u64 += 1; Rejected(hook, bad, seed);
    bad = valid; bad.r1.u64 |= UINT64_C(0x100000000); Rejected(hook, bad, seed);
    bad = valid; bad.r5.u64 = kStack - 8; Rejected(hook, bad, seed);
    bad = valid; bad.r1.u64 = 0xE000F008; bad.r5.u64 = 0xA0010000;
    Rejected(hook, bad, seed, UINT64_C(0x100000000));
  }
}

void Emitter(const OracleHook& hook) {
  alignas(32) Memory seed;
  Randomize(seed);
  std::vector<uint64_t> masks{~UINT64_C(0), UINT64_C(0xAAAAAAAAAAAAAAAA),
                            UINT64_C(0x5555555555555555), UINT64_C(0x8000000000000001),
                            UINT64_C(0xFF00000000000000), UINT64_C(0xFFFF)};
  for (unsigned i = 0; i < 64; ++i) masks.push_back(UINT64_C(1) << i);
  for (unsigned i = 0; i < 1000; ++i) masks.push_back(random_bits() | 1);
  for (uint32_t stage : {0u, 1u}) for (uint32_t alignment : {0u, 4u, 8u, 12u}) {
    WriteConstantWord(seed.data(), kDevice + 48, 0x5000 + alignment);
    WriteConstantWord(seed.data(), kDevice + 52, 0x9000);
    for (uint64_t mask : masks) {
      PPCContext initial;
      Randomize(initial);
      initial.r1.u64 = kStack;
      initial.r3.u64 = kDevice;
      initial.r4.u64 = mask;
      initial.r5.u64 = stage ? 0x4400 : 0x4000;
      initial.r6.u64 = kDevice + (stage ? 6016 : 1920);
      Equivalent(hook, initial, seed, uint32_t(std::popcount(mask)) * 4, alignment);
    }
  }
  WriteConstantWord(seed.data(), kDevice + 48, 0x5000);
  WriteConstantWord(seed.data(), kDevice + 52, 0x9000);
  PPCContext valid{};
  valid.r1.u64 = kStack;
  valid.r3.u64 = kDevice;
  valid.r4.u64 = UINT64_C(0x8000000000000000);
  valid.r5.u64 = 0x4000;
  valid.r6.u64 = kDevice + 1920;
  Rejected(hook, valid, seed, seed.size(), true);
  Rejected(hook, valid, seed, 4096);
  auto bad = valid;
  for (unsigned reg : {1u, 3u, 5u, 6u}) {
    bad = valid; CorruptHighGpr(bad, reg);
    Rejected(hook, bad, seed);
  }
  bad = valid; bad.r4.u64 = 0; Rejected(hook, bad, seed);
  bad = valid; bad.r3.u64 += 4; Rejected(hook, bad, seed);
  bad = valid; bad.r5.u64 = 0x4500; Rejected(hook, bad, seed);
  bad = valid; bad.r6.u64 += 16; Rejected(hook, bad, seed);
  bad = valid; bad.r1.u64 += 1; Rejected(hook, bad, seed);
  bad = valid; bad.r1.u64 = 8; Rejected(hook, bad, seed);
  WriteConstantWord(seed.data(), kDevice + 48, 0x5001);
  Rejected(hook, valid, seed);
  WriteConstantWord(seed.data(), kDevice + 48, 0x5000);
  WriteConstantWord(seed.data(), kDevice + 52, 0x5000 + (16 + 3) * 4);
  Rejected(hook, valid, seed);
  // This mask fits its first run, then reaches the allocator. Validation must
  // reject the whole call before emitting that first run or its backchain.
  bad = valid; bad.r4.u64 = UINT64_C(0xAAAAAAAAAAAAAAAA);
  WriteConstantWord(seed.data(), kDevice + 52, 0x5000 + 200);
  Rejected(hook, bad, seed);
  WriteConstantWord(seed.data(), kDevice + 52, 0xF000);
  for (uint32_t cursor : {kDevice + 1920, kDevice + 32, kStack - 192}) {
    WriteConstantWord(seed.data(), kDevice + 48, cursor);
    Rejected(hook, valid, seed);
  }
  WriteConstantWord(seed.data(), kDevice + 48, 0x7F000000);
  WriteConstantWord(seed.data(), kDevice + 52, 0x80000000);
  Rejected(hook, valid, seed, UINT64_C(0x100000000));
}
}  // namespace

int main() {
  for (const auto& hook : kOracleHooks) {
    if (hook.profile == OracleProfile::GroupedEmitter) Emitter(hook);
    else Setter(hook);
  }
  std::printf("{\"equivalence_cases\":%llu,\"rejected_cases\":%llu}\n",
              static_cast<unsigned long long>(equivalence_cases),
              static_cast<unsigned long long>(rejected_cases));
}
