#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>

namespace xn::gfx::adapters {

// Title metadata for the raw float4 copy leaf. These are device byte offsets,
// not Xenos register indices. The original library owns dirty-range emission
// and synchronization; this replacement never submits, drops or reorders PM4.
struct ConstantSetterLayout {
  uint32_t constants_offset;
  uint32_t dirty_offset;
  uint32_t constant_count = 256;
  bool spill_zero_r31 = false;
};

struct ConstantSetterArgs {
  uint32_t device, index, source, count, destination, bytes, stack_spill;
  uint64_t dirty_mask;
  bool spill_zero_r31;
};

inline bool ConstantRangesOverlap(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size) {
  return a_size && b_size && a < b + b_size && b < a + a_size;
}

// Linux Xenon arenas expose the same physical bytes through A/C/E aliases;
// E has a physical +0x1000 skew. Virtual address separation alone therefore
// cannot justify memcpy or delayed scratch-vector reads.
inline uint64_t ConstantStorageKey(uint64_t address) {
  if (address >= 0xE0000000) return UINT64_C(0x100000000) + address - 0xE0000000 + 0x1000;
  if (address >= 0xC0000000) return UINT64_C(0x100000000) + address - 0xC0000000;
  if (address >= 0xA0000000) return UINT64_C(0x100000000) + address - 0xA0000000;
  return address;
}

inline bool ConstantStorageRangeValid(uint64_t address, uint64_t size) {
  if (!size) return address <= UINT32_MAX;
  if (address + size > UINT64_C(0x100000000)) return false;
  const uint64_t key = ConstantStorageKey(address);
  const uint64_t last = ConstantStorageKey(address + size - 1);
  return last == key + size - 1 &&
      (address < 0xA0000000 || last < UINT64_C(0x120000000));
}

inline bool ConstantStorageRangesOverlap(uint64_t a, uint64_t a_size,
                                         uint64_t b, uint64_t b_size) {
  return ConstantRangesOverlap(ConstantStorageKey(a), a_size,
                               ConstantStorageKey(b), b_size);
}

// All guards precede mutation so an unsupported call may execute its original
// generated body. mmap validity/permissions are still the runtime's contract;
// this checks address arithmetic and rejects MMIO, aliasing and invalid ranges.
inline bool ValidateConstantSetter(const PPCContext& ctx, uint8_t* base, uint64_t mapped_size,
                                  const ConstantSetterLayout& layout, ConstantSetterArgs& out) {
#if !defined(__linux__) || !defined(__x86_64__)
  // Other SDK hosts use different E-alias host offsets and need their own
  // memory-view adapter before this direct-address experiment is enabled.
  return false;
#endif
  if (!base || (reinterpret_cast<uintptr_t>(base) & 15) ||
      mapped_size > UINT64_C(0x100000000) ||
      ctx.r3.u64 > UINT32_MAX || ctx.r4.u64 > UINT32_MAX ||
      ctx.r5.u64 > UINT32_MAX || ctx.r6.u64 > UINT32_MAX ||
      (layout.constants_offset & 15) || (layout.dirty_offset & 7) ||
      !layout.constant_count || layout.constant_count > 256) return false;
  const uint64_t device = ctx.r3.u32, index = ctx.r4.u32;
  const uint64_t source = ctx.r5.u32, count = ctx.r6.u32;
  if (device < 0x1000 || (device & 15) || index > layout.constant_count ||
      count > layout.constant_count - index) return false;
  const uint64_t destination = device + layout.constants_offset + index * 16;
  const uint64_t dirty = device + layout.dirty_offset, bytes = count * 16;
  // The VMX leaf may load the aligned block following its last unaligned
  // float4. Finishing its volatile vector values must stay within the mapping.
  const uint64_t source_read_begin = source & ~UINT64_C(15);
  const uint64_t source_read_end = (source + bytes + 15) & ~UINT64_C(15);
  if (destination > UINT32_MAX || destination + bytes > mapped_size ||
      dirty + 8 > mapped_size ||
      (bytes && (source < 0x1000 || source_read_end > mapped_size))) return false;
  const uint64_t source_read_bytes = bytes ? source_read_end - source_read_begin : 0;
  if (!ConstantStorageRangeValid(destination, bytes) ||
      !ConstantStorageRangeValid(dirty, 8) ||
      !ConstantStorageRangeValid(source_read_begin, source_read_bytes)) return false;
  auto touches_mmio = [](uint64_t address, uint64_t size) {
    return ConstantRangesOverlap(address, size, 0x7F000000, 0x01000000);
  };
  if (touches_mmio(destination, bytes) || touches_mmio(dirty, 8) ||
      touches_mmio(source_read_begin, bytes ? source_read_end - source_read_begin : 0) ||
      ConstantStorageRangesOverlap(source_read_begin, source_read_bytes, destination, bytes) ||
      ConstantStorageRangesOverlap(source_read_begin, source_read_bytes, dirty, 8) ||
      ConstantStorageRangesOverlap(destination, bytes, dirty, 8)) return false;
  uint64_t spill = 0;
  if (layout.spill_zero_r31) {
    if (ctx.r1.u64 > UINT32_MAX || ctx.r1.u32 < 0x1008 || (ctx.r1.u32 & 7)) return false;
    spill = uint64_t(ctx.r1.u32) - 8;
    if (spill + 8 > mapped_size || touches_mmio(spill, 8) ||
        !ConstantStorageRangeValid(spill, 8) ||
        ConstantStorageRangesOverlap(spill, 8, source_read_begin, source_read_bytes) ||
        ConstantStorageRangesOverlap(spill, 8, destination, bytes) ||
        ConstantStorageRangesOverlap(spill, 8, dirty, 8)) return false;
  }
  out = {uint32_t(device), uint32_t(index), uint32_t(source), uint32_t(count),
         uint32_t(destination), uint32_t(bytes), uint32_t(spill), ctx.r7.u64,
         layout.spill_zero_r31};
  return true;
}

inline simde__m128i LoadFloatCopyLeft(const uint8_t* base, uint32_t address) {
  return simde_mm_shuffle_epi8(
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(base + (address & ~15u))),
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(
          &rex::ppc::VectorMaskL[(address & 15) * 16])));
}

inline simde__m128i LoadFloatCopyRight(const uint8_t* base, uint32_t address) {
  if (!(address & 15)) return simde_mm_setzero_si128();
  return simde_mm_shuffle_epi8(
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(base + (address & ~15u))),
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(
          &rex::ppc::VectorMaskR[(address & 15) * 16])));
}

inline simde__m128i LoadFloatCopyFull(const uint8_t* base, uint32_t address) {
  return simde_mm_shuffle_epi8(
      simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(base + address)),
      simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(rex::ppc::VectorMaskL)));
}

inline uint64_t ApplyConstantSetterMemory(uint8_t* base, const ConstantSetterArgs& args,
                                         const ConstantSetterLayout& layout) {
  if (args.spill_zero_r31)
    *reinterpret_cast<volatile uint64_t*>(base + args.stack_spill) = 0;
  if (args.bytes) std::memcpy(base + args.destination, base + args.source, args.bytes);
  auto* dirty = reinterpret_cast<volatile uint64_t*>(base + args.device + layout.dirty_offset);
  const uint64_t value = __builtin_bswap64(*dirty) | args.dirty_mask;
  *dirty = __builtin_bswap64(value);
  return value;
}

// MCLA leaves a raw VMX copying ABI visible to its callers. Reproduce every
// modified context register, including the last partial vector fragments,
// without running that VMX loop for every copied constant.
inline void FinishMclaConstantSetter(PPCContext& ctx, const uint8_t* base,
                                    const ConstantSetterArgs& args, uint64_t dirty) {
  const uint32_t groups = args.count / 4, tail = args.count % 4;
  if (groups) {
    const uint32_t source = args.source + (groups - 1) * 64;
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v59.u8), LoadFloatCopyLeft(base, source + 48));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v60.u8), LoadFloatCopyFull(base, source + 48));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v61.u8), LoadFloatCopyFull(base, source + 32));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v62.u8), LoadFloatCopyFull(base, source + 16));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v63.u8), LoadFloatCopyFull(base, source));
    ctx.r5.u64 = 32;
    ctx.r6.u64 = 48;
  }
  if (tail) {
    const uint32_t source = args.source + (args.count - 1) * 16;
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v63.u8), LoadFloatCopyFull(base, source));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v62.u8), LoadFloatCopyRight(base, source + 16));
  }
  ctx.r4.u64 = 16;
  ctx.r8.u64 = groups ? 0 : 128;
  ctx.r9.u64 = 0;
  ctx.r10.u64 = uint64_t(args.destination) + args.bytes;
  ctx.r11.u64 = dirty;
  ctx.cr6.compare<uint32_t>(tail, 0, ctx.xer);
  if (args.count) {
    ctx.xer.ca = 1;
    ctx.cr0.compare<int32_t>(0, 0, ctx.xer);
  }
}

inline bool TryMclaConstantSetter(PPCContext& ctx, uint8_t* base, uint64_t mapped_size,
                                  const ConstantSetterLayout& layout) {
  ConstantSetterArgs args;
  if (!ValidateConstantSetter(ctx, base, mapped_size, layout, args)) return false;
  const uint64_t dirty = ApplyConstantSetterMemory(base, args, layout);
  FinishMclaConstantSetter(ctx, base, args, dirty);
  return true;
}

}  // namespace xn::gfx::adapters
