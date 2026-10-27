#pragma once

#include <array>
#include "constant_setter.h"

namespace xn::gfx::adapters {

struct ConstantEmitterLayout {
  uint32_t entry_after_save_lr;
  uint32_t cursor_offset = 48;
  uint32_t limit_offset = 52;
};

struct ConstantEmitRun {
  uint32_t header_address, source_address, header, blocks, leading;
  uint32_t cursor_alignment;
  uint64_t aligned_mask;
};

struct ConstantEmitterArgs {
  uint32_t device, stack, original_cursor, final_cursor, limit, final_register;
  uint32_t run_count;
  std::array<ConstantEmitRun, 32> runs;
};

inline uint32_t ReadConstantWord(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + address));
}

inline void WriteConstantWord(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<volatile uint32_t*>(base + address) = __builtin_bswap32(value);
}

// Prepare the complete call before writing anything. A run that would call the
// guest allocator, or any aliased/unsupported layout, forwards the WHOLE call.
// Consequently the guest slow path sees its original cursor and dirty mask.
inline bool ValidateConstantEmitter(const PPCContext& ctx, uint8_t* base,
                                    uint64_t mapped_size,
                                    const ConstantEmitterLayout& layout,
                                    ConstantEmitterArgs& out) {
#if !defined(__linux__) || !defined(__x86_64__)
  return false;
#endif
  if (!base || (reinterpret_cast<uintptr_t>(base) & 15) ||
      mapped_size > UINT64_C(0x100000000) || !ctx.r4.u64 ||
      ctx.r1.u64 > UINT32_MAX || ctx.r3.u64 > UINT32_MAX ||
      ctx.r5.u64 > UINT32_MAX || ctx.r6.u64 > UINT32_MAX ||
      ctx.r1.u32 < 0x10B0 || (ctx.r1.u32 & 15) ||
      ctx.r3.u32 < 0x1000 || (ctx.r3.u32 & 15) ||
      (layout.cursor_offset & 3) || (layout.limit_offset & 3)) return false;
  const uint64_t device = ctx.r3.u32, stack = ctx.r1.u32 - 176;
  const uint64_t cursor_word = device + layout.cursor_offset;
  const uint64_t limit_word = device + layout.limit_offset;
  const uint32_t register_base = ctx.r5.u32;
  if (register_base != 0x4000 && register_base != 0x4400) return false;
  const uint64_t source = ctx.r6.u32;
  if (source != device + (register_base == 0x4000 ? 1920 : 6016) ||
      source + 4096 > mapped_size || cursor_word + 4 > mapped_size ||
      limit_word + 4 > mapped_size || stack + 4 > mapped_size) return false;
  if (!ConstantStorageRangeValid(source, 4096) ||
      !ConstantStorageRangeValid(cursor_word, 4) ||
      !ConstantStorageRangeValid(limit_word, 4) ||
      !ConstantStorageRangeValid(stack, 4)) return false;
  auto mmio = [](uint64_t address, uint64_t bytes) {
    return ConstantRangesOverlap(address, bytes, 0x7F000000, 0x01000000);
  };
  if (mmio(cursor_word, 4) || mmio(limit_word, 4) || mmio(stack, 4) ||
      mmio(source, 4096) ||
      ConstantStorageRangesOverlap(stack, 4, source, 4096) ||
      ConstantStorageRangesOverlap(stack, 4, cursor_word, 4) ||
      ConstantStorageRangesOverlap(stack, 4, limit_word, 4)) return false;
  const uint32_t initial_cursor = ReadConstantWord(base, uint32_t(cursor_word));
  const uint32_t limit = ReadConstantWord(base, uint32_t(limit_word));
  if (initial_cursor < 0x1000 || (initial_cursor & 3) || limit > mapped_size) return false;
  uint64_t cursor = initial_cursor, mask = ctx.r4.u64;
  uint32_t group = 0, run_count = 0;
  while (mask) {
    const uint32_t leading = __builtin_clzll(mask);
    mask <<= leading;
    group += leading;
    const uint32_t blocks = ~mask ? __builtin_clzll(~mask) : 64;
    if (!blocks || group + blocks > 64 || run_count == out.runs.size()) return false;
    // This is the library's exact fast-path allocation threshold. The cursor
    // points at the last written dword, not at the first free byte.
    if (cursor + (uint64_t(blocks) * 16 + 3) * 4 >= limit) return false;
    const uint32_t alignment = uint32_t(cursor) & 15;
    const uint32_t padding = (8 - alignment) & 15;
    const uint64_t header_address = cursor + padding + 4;
    const uint64_t final_cursor = header_address + uint64_t(blocks) * 64;
    if (final_cursor > UINT32_MAX || final_cursor + 4 > mapped_size) return false;
    out.runs[run_count++] = {
        uint32_t(header_address), uint32_t(source + group * 64),
        ((blocks * 16 - 1) << 16) | (register_base + group * 16),
        blocks, leading, alignment, mask};
    cursor = final_cursor;
    group += blocks;
    mask = blocks == 64 ? 0 : mask << blocks;
  }
  const uint64_t output = uint64_t(initial_cursor) + 4, output_bytes = cursor - initial_cursor;
  if (mmio(output, output_bytes) || !ConstantStorageRangeValid(output, output_bytes) ||
      ConstantStorageRangesOverlap(output, output_bytes, source, 4096) ||
      ConstantStorageRangesOverlap(output, output_bytes, stack, 4) ||
      ConstantStorageRangesOverlap(output, output_bytes, cursor_word, 4) ||
      ConstantStorageRangesOverlap(output, output_bytes, limit_word, 4)) return false;
  out.device = uint32_t(device);
  out.stack = uint32_t(stack);
  out.original_cursor = initial_cursor;
  out.final_cursor = uint32_t(cursor);
  out.limit = limit;
  out.final_register = register_base + group * 16;
  out.run_count = run_count;
  return true;
}

inline bool TryConstantEmitter(PPCContext& ctx, uint8_t* base, uint64_t mapped_size,
                               const ConstantEmitterLayout& layout) {
  ConstantEmitterArgs args;
  if (!ValidateConstantEmitter(ctx, base, mapped_size, layout, args)) return false;
  WriteConstantWord(base, args.stack, ctx.r1.u32);
  uint32_t cursor = args.original_cursor;
  for (uint32_t i = 0; i < args.run_count; ++i) {
    const auto& run = args.runs[i];
    while (cursor + 4 < run.header_address) {
      cursor += 4;
      WriteConstantWord(base, cursor, 0x80000000);
    }
    WriteConstantWord(base, run.header_address, run.header);
    std::memcpy(base + run.header_address + 4, base + run.source_address,
                size_t(run.blocks) * 64);
    cursor = run.header_address + run.blocks * 64;
  }
  WriteConstantWord(base, args.device + layout.cursor_offset, args.final_cursor);
  const auto& last = args.runs[args.run_count - 1];
  const uint32_t source = last.source_address + (last.blocks - 1) * 64;
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v63.u8), LoadFloatCopyFull(base, source));
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v62.u8), LoadFloatCopyFull(base, source + 16));
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v61.u8), LoadFloatCopyFull(base, source + 32));
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(ctx.v60.u8), LoadFloatCopyFull(base, source + 48));
  ctx.r12.u64 = ctx.lr;
  ctx.lr = layout.entry_after_save_lr;
  ctx.r4.u64 = args.final_cursor;
  ctx.r5.u64 = args.final_register;
  ctx.r8.u64 = ~last.aligned_mask;
  constexpr uint64_t signed_nop = UINT64_C(0xFFFFFFFF80000000);
  ctx.r9.u64 = last.cursor_alignment == 12 ? signed_nop : args.limit;
  ctx.r10.u64 = (last.cursor_alignment == 0 || last.cursor_alignment == 12)
      ? signed_nop : last.leading * 16;
  ctx.r11.u64 = last.header;
  ctx.xer.ca = 1;
  ctx.cr0.compare<int32_t>(0, 0, ctx.xer);
  ctx.cr6.compare<uint64_t>(0, 0, ctx.xer);
  return true;
}

}  // namespace xn::gfx::adapters
