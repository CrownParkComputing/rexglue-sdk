/**
 * @file        system/snapshot.cpp
 * @brief       Entry/exit register and memory snapshots of hooked guest functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Deliberately independent of the kernel: only <rex/ppc/context.h> and the
// C library, so tests/native/snap_replay can compile this file into a host
// executable without the runtime. Environment variables rather than cvars
// for the same reason as reach_trace.cpp: hooks run before config parsing.
//
// Dirtiness is not tracked with the runtime's write-watch machinery. That
// state is owned by the GPU shared-memory uploader and toggling it from a CPU
// hook would perturb exactly the residency behaviour being measured. Instead
// both copies of every declared range are stored and the reader diffs them.
// The limitation: writes outside the declared ranges are invisible. Declare
// every output range the function is known to touch (argument pointers, the
// object at r3, ring/palette buffers); undeclared side effects are what
// tee digests are for.

#include <rex/system/snapshot.h>
#include <rex/system/snapshot_format.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#define REX_SNAP_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define REX_SNAP_MKDIR(p) ::mkdir((p), 0755)
#endif

namespace rex::snapshot {

struct Call {
  FILE* file = nullptr;
  std::vector<RangeRecord> ranges;
};

namespace {

struct State {
  std::mutex mutex;
  std::unordered_map<uint32_t, uint32_t> per_addr;
  const char* dir = nullptr;
  uint32_t max_per_addr = 32;
  uint32_t max_bytes = 1u << 20;
  bool ready = false;
};

State& GetState() {
  static State* s = [] {
    auto* st = new State();
    st->dir = std::getenv("REX_SNAP_DIR");
    if (st->dir && *st->dir) {
      REX_SNAP_MKDIR(st->dir);  // best effort; fopen tells us if it failed
      st->ready = true;
    }
    if (const char* v = std::getenv("REX_SNAP_MAX"); v && *v)
      st->max_per_addr = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    if (const char* v = std::getenv("REX_SNAP_MAX_BYTES"); v && *v)
      st->max_bytes = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    return st;
  }();
  return *s;
}

std::string SanitiseTag(const char* tag) {
  std::string out;
  for (const char* p = tag ? tag : ""; *p && out.size() < 48; ++p) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.';
    out.push_back(ok ? c : '_');
  }
  if (out.empty()) out = "call";
  return out;
}

bool WriteRanges(FILE* f, const std::vector<RangeRecord>& ranges, const uint8_t* base) {
  for (const RangeRecord& r : ranges) {
    if (r.stored == 0) continue;
    if (std::fwrite(base + r.addr, 1, r.stored, f) != r.stored) return false;
  }
  return true;
}

}  // namespace

uint32_t ParseAddress(const char* name) {
  if (!name) return 0;
  const char* p = std::strstr(name, "sub_");
  if (!p) return 0;
  p += 4;
  char* end = nullptr;
  unsigned long v = std::strtoul(p, &end, 16);
  if (end == p) return 0;
  return static_cast<uint32_t>(v);
}

void Capture(const PPCContext& ctx, RegFile& out) {
  std::memset(&out, 0, sizeof(out));
  const PPCRegister* r[32] = {
      &ctx.r0,  &ctx.r1,  &ctx.r2,  &ctx.r3,  &ctx.r4,  &ctx.r5,  &ctx.r6,  &ctx.r7,
      &ctx.r8,  &ctx.r9,  &ctx.r10, &ctx.r11, &ctx.r12, &ctx.r13, &ctx.r14, &ctx.r15,
      &ctx.r16, &ctx.r17, &ctx.r18, &ctx.r19, &ctx.r20, &ctx.r21, &ctx.r22, &ctx.r23,
      &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
  for (int i = 0; i < 32; ++i) out.r[i] = r[i]->u64;
  const PPCRegister* f = &ctx.f0;
  for (int i = 0; i < 32; ++i) out.f[i] = f[i].u64;
  const PPCVRegister* v = &ctx.v0;
  for (int i = 0; i < 128; ++i) std::memcpy(out.v[i], &v[i], 16);
  out.lr = ctx.lr;
  out.ctr = ctx.ctr.u64;
  out.xer_so = ctx.xer.so;
  out.xer_ov = ctx.xer.ov;
  out.xer_ca = ctx.xer.ca;
  const PPCCRRegister* cr = &ctx.cr0;
  for (int i = 0; i < 8; ++i) out.cr[i] = static_cast<uint8_t>(cr[i].raw());
  out.fpscr = ctx.fpscr.csr;
  out.msr = ctx.msr;
  out.vscr_sat = ctx.vscr_sat;
}

void Restore(const RegFile& in, PPCContext& ctx) {
  PPCRegister* r[32] = {
      &ctx.r0,  &ctx.r1,  &ctx.r2,  &ctx.r3,  &ctx.r4,  &ctx.r5,  &ctx.r6,  &ctx.r7,
      &ctx.r8,  &ctx.r9,  &ctx.r10, &ctx.r11, &ctx.r12, &ctx.r13, &ctx.r14, &ctx.r15,
      &ctx.r16, &ctx.r17, &ctx.r18, &ctx.r19, &ctx.r20, &ctx.r21, &ctx.r22, &ctx.r23,
      &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
  for (int i = 0; i < 32; ++i) r[i]->u64 = in.r[i];
  PPCRegister* f = &ctx.f0;
  for (int i = 0; i < 32; ++i) f[i].u64 = in.f[i];
  PPCVRegister* v = &ctx.v0;
  for (int i = 0; i < 128; ++i) std::memcpy(&v[i], in.v[i], 16);
  ctx.lr = in.lr;
  ctx.ctr.u64 = in.ctr;
  ctx.xer.so = in.xer_so;
  ctx.xer.ov = in.xer_ov;
  ctx.xer.ca = in.xer_ca;
  PPCCRRegister* cr = &ctx.cr0;
  for (int i = 0; i < 8; ++i) cr[i].set_raw(in.cr[i]);
  ctx.fpscr.csr = in.fpscr;  // not applied to the host: the replayer decides
  ctx.msr = in.msr;
  ctx.vscr_sat = in.vscr_sat;
}

bool Enabled() { return GetState().ready; }

uint32_t CountFor(uint32_t addr) {
  State& st = GetState();
  std::lock_guard<std::mutex> lock(st.mutex);
  auto it = st.per_addr.find(addr);
  return it == st.per_addr.end() ? 0 : it->second;
}

Call* Record(const char* tag, uint32_t addr, const PPCContext& ctx, const uint8_t* base,
             std::span<const Range> ranges) {
  State& st = GetState();
  if (!st.ready) return nullptr;

  uint32_t seq;
  {
    std::lock_guard<std::mutex> lock(st.mutex);
    uint32_t& n = st.per_addr[addr];
    if (n >= st.max_per_addr) return nullptr;
    seq = n++;
  }

  char name[512];
  std::snprintf(name, sizeof(name), "%s/%08X_%s_%04u.snap", st.dir, addr,
                SanitiseTag(tag).c_str(), seq);
  FILE* f = std::fopen(name, "wb");
  if (!f) return nullptr;

  auto* call = new Call();
  call->file = f;
  for (const Range& r : ranges) {
    if (r.addr == 0 || r.size == 0) continue;
    RangeRecord rec{r.addr, r.size, r.size, 0};
    if (rec.stored > st.max_bytes) {
      rec.stored = st.max_bytes;
      rec.flags |= kTruncated;
    }
    call->ranges.push_back(rec);
  }

  std::string tag_s = tag ? tag : "";
  FileHeader h{};
  std::memcpy(h.magic, kMagic, sizeof(kMagic));
  h.version = kVersion;
  h.header_size = sizeof(FileHeader);
  h.addr = addr;
  h.range_count = static_cast<uint32_t>(call->ranges.size());
  h.timestamp_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  h.tag_len = static_cast<uint32_t>(tag_s.size());
  h.flags = 0;

  RegFile regs;
  Capture(ctx, regs);

  static const char zeros[4] = {0, 0, 0, 0};
  uint32_t pad = (4 - (h.tag_len & 3)) & 3;
  bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1 &&
            (h.tag_len == 0 || std::fwrite(tag_s.data(), 1, h.tag_len, f) == h.tag_len) &&
            (pad == 0 || std::fwrite(zeros, 1, pad, f) == pad) &&
            (call->ranges.empty() ||
             std::fwrite(call->ranges.data(), sizeof(RangeRecord), call->ranges.size(), f) ==
                 call->ranges.size()) &&
            std::fwrite(&regs, sizeof(regs), 1, f) == 1 && WriteRanges(f, call->ranges, base);
  if (!ok) {
    std::fclose(f);
    std::remove(name);
    delete call;
    return nullptr;
  }
  return call;
}

void Record(Call* call, const PPCContext& ctx, const uint8_t* base) {
  if (!call) return;
  RegFile regs;
  Capture(ctx, regs);
  FILE* f = call->file;
  bool ok = std::fwrite(&regs, sizeof(regs), 1, f) == 1 && WriteRanges(f, call->ranges, base);
  if (ok) {
    // Mark the recording complete: a reader treats a file without kHasExit as
    // a call that never returned (or the process died inside it).
    uint32_t flags = kHasExit;
    std::fflush(f);
    if (std::fseek(f, static_cast<long>(offsetof(FileHeader, flags)), SEEK_SET) == 0)
      std::fwrite(&flags, sizeof(flags), 1, f);
  }
  std::fclose(f);
  delete call;
}

}  // namespace rex::snapshot
