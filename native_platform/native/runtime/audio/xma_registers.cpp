#include "audio/xma_registers.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "audio/xma.h"

namespace xn::audio {

namespace {
// The block is little-endian on the bus; the MMIO seam speaks the guest's
// natural (big-endian register) value. See the header.
inline uint32_t Swap(uint32_t v) { return __builtin_bswap32(v); }

// XN_XMA_DUMP_CONTEXTS=<n>: print the 64-byte hardware context of the first n
// kicked contexts, in the canonical DWORD order xma.h reads. A title that
// programs contexts directly leaves no other trace of what it wrote, and "the
// context never had an input buffer" otherwise looks the same as "the decoder
// dropped it".
int ContextDumpLimit() {
  static const int limit = [] {
    const char* v = std::getenv("XN_XMA_DUMP_CONTEXTS");
    return v ? std::atoi(v) : 0;
  }();
  return limit;
}
int g_contexts_dumped = 0;

// XN_XMA_TRACE_EVENTS=1: one line per kick/lock and per busy read of the
// current-context register, with the context snapshot the title left behind.
// This is the only way to see the title's refill protocol: whether it re-arms
// an input buffer before or after the kick, and whether it waits on the busy
// bit at all.
bool EventTrace() {
  static const bool on = std::getenv("XN_XMA_TRACE_EVENTS") != nullptr;
  return on;
}
int g_events_logged = 0;
constexpr int kMaxEventLines = 120;
double NowMs() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
std::string Snapshot(XmaEngine* engine, uint32_t guest_context) {
  if (!engine) return "-";
  XmaContext* c = engine->Lookup(guest_context);
  if (!c || !c->guest()) return "-";
  const XmaContextData d = XmaContextData::Load(c->guest());
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "in0=%u/%#010x%s in1=%u/%#010x%s cur=%u ro=%u out=%#010x blocks=%u w=%u %s lb=%u/%u",
                d.input_buffer_0_packet_count, d.input_buffer_0_ptr,
                d.input_buffer_0_valid ? "V" : "-", d.input_buffer_1_packet_count,
                d.input_buffer_1_ptr, d.input_buffer_1_valid ? "V" : "-", d.current_buffer,
                d.input_buffer_read_offset, d.output_buffer_ptr, d.output_buffer_block_count,
                d.output_buffer_write_offset, d.output_buffer_valid ? "outV" : "out-",
                d.loop_start, d.loop_end);
  return buf;
}
void LogEvent(const char* what, XmaEngine* engine, uint32_t index, uint32_t guest_context) {
  if (!EventTrace() || g_events_logged >= kMaxEventLines) return;
  g_events_logged++;
  std::fprintf(stderr, "[xma] %8.3f ms %-5s ctx%u: %s\n", NowMs(), what, index,
               Snapshot(engine, guest_context).c_str());
}

void DumpContext(XmaEngine* engine, uint32_t guest_context) {
  if (!engine || g_contexts_dumped >= ContextDumpLimit()) return;
  XmaContext* c = engine->Lookup(guest_context);
  if (!c || !c->guest()) return;
  g_contexts_dumped++;
  const uint8_t* p = c->guest();
  std::fprintf(stderr, "[xma] ctx%u kick dump (%#010x):", c->id(), guest_context);
  for (uint32_t w = 0; w < kXmaContextBytes / 4; w++) {
    uint32_t v;
    std::memcpy(&v, p + w * 4, 4);
    std::fprintf(stderr, " %08x", Swap(v));
  }
  const XmaContextData d = XmaContextData::Load(p);
  std::fprintf(stderr,
               "\n[xma]   word0: in0_packets=%u loop_count=%u in0_valid=%u in1_valid=%u "
               "out_blocks=%u out_write=%u\n[xma]   word1: in1_packets=%u sdc=%u rate=%u ch=%u "
               "out_valid=%u\n[xma]   word2: read_offset=%u | word3 loop_start=%u | "
               "word4 loop_end=%u cur=%u\n[xma]   in0_ptr=%#010x in1_ptr=%#010x out_ptr=%#010x "
               "work_ptr=%#010x\n",
               d.input_buffer_0_packet_count, d.loop_count, d.input_buffer_0_valid,
               d.input_buffer_1_valid, d.output_buffer_block_count, d.output_buffer_write_offset,
               d.input_buffer_1_packet_count, d.subframe_decode_count, d.sample_rate, d.is_stereo,
               d.output_buffer_valid, d.input_buffer_read_offset, d.loop_start, d.loop_end,
               d.current_buffer, d.input_buffer_0_ptr, d.input_buffer_1_ptr, d.output_buffer_ptr,
               d.work_buffer_ptr);
}
}  // namespace

void XmaRegisterFile::Setup(XmaEngine* engine, uint32_t context_table_physical) {
  engine_ = engine;
  table_physical_ = context_table_physical;
  std::fprintf(stderr, "[xma] registers at %#010x, context array at physical %#010x\n",
               kWindowBase, table_physical_);
}

uint32_t XmaRegisterFile::ContextAddress(uint32_t index) const {
  return engine_ ? engine_->table_address() + index * kXmaContextBytes : 0;
}

uint32_t XmaRegisterFile::Read(uint32_t address) {
  const uint32_t reg = (address & 0xFFFF) / 4;
  switch (reg) {
    case kRegContextArrayAddress:
      return Swap(table_physical_);
    case kRegCurrentContextIndex: {
      // While a pass may be writing a guest context, report that context as
      // busy so the guest's own block-until-idle loop does not copy a half
      // written 64-byte context out from under the decoder.
      int32_t working = engine_ ? engine_->WorkingContextIndex() : -1;
      if (working >= 0 && EventTrace() && g_events_logged < kMaxEventLines) {
        g_events_logged++;
        std::fprintf(stderr, "[xma] %8.3f ms read  0x606: busy ctx%d (guest polls)\n", NowMs(),
                     working);
      }
      return Swap(working < 0 ? 0u : (kCurrentContextBusyBit | uint32_t(working)));
    }
    default:
      break;
  }
  static bool warned = false;
  if (!warned) {
    warned = true;
    std::fprintf(stderr, "[xma] read of unmodelled register %#x (%#010x) returns 0\n", reg,
                 address);
  }
  return 0;
}

uint64_t XmaRegisterFile::ForEachContext(uint32_t base_index, uint32_t value, bool enable,
                                         const char* what) {
  uint64_t acted = 0;
  for (uint32_t bit = 0; bit < 32; bit++) {
    if (!(value & (1u << bit))) continue;
    const uint32_t index = base_index + bit;
    if (index >= kXmaContextCount) {
      std::fprintf(stderr, "[xma] %s for context %u beyond the %u we model\n", what, index,
                   kXmaContextCount);
      continue;
    }
    acted++;
    if (!engine_) continue;
    if (enable) {
      LogEvent(what, engine_, index, ContextAddress(index));
      DumpContext(engine_, ContextAddress(index));
      engine_->Enable(ContextAddress(index));
      // The title kicks, then locks 11 us later (GW2) or reads the output right
      // away (BBR): the pass has to happen inside that window, so it runs here
      // on the guest's thread exactly as the hardware decodes on the kick. The
      // worker thread still services contexts the title leaves enabled.
      engine_->WorkContext(ContextAddress(index));
    } else {
      LogEvent(what, engine_, index, ContextAddress(index));
      engine_->Disable(ContextAddress(index));
    }
  }
  return acted;
}

// XN_XMA_IGNORE_KICK=1 drops writes to the KICK bitmask, reproducing the
// register map this file shipped with before KICK was modelled (it had KICK at
// 0x6A0, which is CLEAR, so the real kick fell through and was dropped). Kept
// as the A/B for "no sound at all" in a title whose per-refill go signal is
// KICK; see docs/AUDIO.md.
static bool IgnoreKick() {
  static const bool on = [] {
    const char* v = std::getenv("XN_XMA_IGNORE_KICK");
    const bool set = v && *v == '1';
    if (set) std::fprintf(stderr, "[xma] XN_XMA_IGNORE_KICK=1: KICK writes are dropped\n");
    return set;
  }();
  return on;
}

void XmaRegisterFile::Write(uint32_t address, uint32_t value) {
  const uint32_t reg = (address & 0xFFFF) / 4;
  const uint32_t v = Swap(value);
  // KICK and CLEAR both start a context; LOCK stops one. See the header for
  // which titles write which.
  if (reg >= kRegKickBase && reg < kRegKickBase + kRegBitmaskWords) {
    if (IgnoreKick()) return;
    kicks_ += ForEachContext((reg - kRegKickBase) * 32, v, true, "kick");
    return;
  }
  if (reg >= kRegClearBase && reg < kRegClearBase + kRegBitmaskWords) {
    clears_ += ForEachContext((reg - kRegClearBase) * 32, v, true, "clear");
    return;
  }
  if (reg >= kRegLockBase && reg < kRegLockBase + kRegBitmaskWords) {
    locks_ += ForEachContext((reg - kRegLockBase) * 32, v, false, "lock");
    return;
  }
  switch (reg) {
    case kRegContextArrayAddress:
      // The console lets the kernel place the array; a title that writes it is
      // relocating the block, which we do not support - say so rather than
      // quietly decoding from the wrong table.
      if (v != table_physical_) {
        std::fprintf(stderr, "[xma] guest set the context array to %#010x, ours is %#010x\n", v,
                     table_physical_);
      }
      return;
    case kRegControl:
      // 0 then 3 is the reset the guest issues after its block-until-idle loop
      // times out. Reaching here means a context stayed busy for 125 ms, which
      // is a decoder problem worth seeing rather than absorbing.
      if (v) {
        resets_++;
        std::fprintf(stderr,
                     "[xma] decoder reset requested (control=%#x); a context did not go idle\n", v);
      }
      return;
    default:
      break;
  }
  static bool warned = false;
  if (!warned) {
    warned = true;
    std::fprintf(stderr, "[xma] write of unmodelled register %#x (%#010x) = %#010x dropped\n",
                 reg, address, v);
  }
}

}  // namespace xn::audio
