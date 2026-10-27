#pragma once

#include "constant_setter.h"

namespace xn::gfx::adapters {

// Verified from Forza's 82316A28/82332F08 leaf routines. Preserve their scratch
// context as well as the device writes: generated callers can observe more
// than the conventional ABI's return register. No commands are submitted here.
inline void FinishForzaConstantSetter(PPCContext& ctx, const uint8_t* base,
                                     const ConstantSetterArgs& args,
                                     uint64_t updated_dirty) {
  const uint32_t groups = args.count / 4, tail = args.count % 4;
  if (groups) {
    const uint32_t last_group = args.source + (groups - 1) * 64;
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v59.u8),
                        LoadFloatCopyLeft(base, last_group + 48));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v60.u8),
                        LoadFloatCopyFull(base, last_group + 48));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v61.u8),
                        LoadFloatCopyFull(base, last_group + 32));
    if (!tail) {
      simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v62.u8),
                          LoadFloatCopyFull(base, last_group + 16));
      simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v63.u8),
                          LoadFloatCopyFull(base, last_group));
    }
    ctx.r4.u64 = 64;
    ctx.r6.u64 = 32;
  }
  if (tail) {
    const uint32_t last = args.source + (args.count - 1) * 16;
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v62.u8),
                        LoadFloatCopyRight(base, last + 16));
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v63.u8),
                        LoadFloatCopyFull(base, last));
  }
  ctx.r5.u64 = 16;
  ctx.r8.u64 = groups ? 48 : 128;
  ctx.r9.u64 = tail;
  ctx.r10.u64 = uint64_t(args.destination) + args.bytes;
  ctx.r11.u64 = updated_dirty;
  if (args.count) ctx.ctr.u64 = 0;
  ctx.cr6.compare<uint32_t>(tail, 0, ctx.xer);
}

inline bool TryForzaConstantSetter(PPCContext& ctx, uint8_t* base,
                                  uint64_t mapped_size,
                                  const ConstantSetterLayout& layout) {
  ConstantSetterArgs args;
  if (!ValidateConstantSetter(ctx, base, mapped_size, layout, args)) return false;
  const uint64_t updated_dirty = ApplyConstantSetterMemory(base, args, layout);
  FinishForzaConstantSetter(ctx, base, args, updated_dirty);
  return true;
}

}  // namespace xn::gfx::adapters
