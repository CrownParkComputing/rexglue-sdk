// Identity test for the snapshot harness. A synthetic generated-style
// function (same shape as a DEFINE_REX_FUNC body: reads argument registers,
// reads and writes guest memory through base + u32, returns in r3 and f1) is
// wrapped with REX_SNAP_HOOK, run on a fake arena with REX_SNAP_DIR set, and
// the resulting .snap is replayed against:
//   1. the recorded exit state itself (ReplayIdentity)        -> MATCH
//   2. the original body                                      -> MATCH
//   3. a replacement that is off by one ULP in f1 and one float -> MISMATCH at
//      ulp 0, MATCH at float_ulp = 1 (from a TOML tolerance file)
//   4. a replacement that forgets one store                    -> MISMATCH naming
//      the address
// Exit status 0 when every expectation holds. No runtime, no Catch2: the test
// must build wherever snapshot.cpp does.
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <rex/system/snapshot.h>

#include "snap_replay.h"

namespace {

int g_failures = 0;
#define EXPECT(cond, msg)                                              \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
      ++g_failures;                                                    \
    } else {                                                           \
      std::printf("ok   %s\n", msg);                                   \
    }                                                                  \
  } while (0)

inline uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
inline void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// "Generated" body: r3 = pointer to u32 array, r4 = count, r5 = out pointer.
// Sums the array (big-endian), writes each element doubled back, writes the
// sum to *r5, returns the sum in r3 and sum * 0.5 in f1, and writes a float
// (sum as f32 / 3) at r5+4. Uses the stack (a store below r1) like real code.
void Body(PPCContext& ctx, uint8_t* base, float divisor) {
  uint32_t arr = ctx.r3.u32, n = ctx.r4.u32, out = ctx.r5.u32;
  uint32_t sum = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v = LoadBE32(base + arr + i * 4);
    sum += v;
    StoreBE32(base + arr + i * 4, v * 2);
  }
  StoreBE32(base + out, sum);
  StoreBE32(base + out + 4, std::bit_cast<uint32_t>(static_cast<float>(sum) / divisor));
  StoreBE32(base + ctx.r1.u32 - 8, 0xDEADBEEF);  // scratch on the stack
  ctx.r3.u64 = sum;
  ctx.f1.f64 = sum * 0.5;
  ctx.cr0.set_raw(sum ? 4 : 2);
}

}  // namespace

extern "C" void __imp__sub_00401000(PPCContext& ctx, uint8_t* base) { Body(ctx, base, 3.0f); }

// Off by one ULP in f1 and in the stored float.
extern "C" void repl_ulp(PPCContext& ctx, uint8_t* base) {
  Body(ctx, base, 3.0f);
  ctx.f1.f64 = std::nextafter(ctx.f1.f64, 1e300);
  uint32_t out = ctx.r5.u32;
  float f = std::bit_cast<float>(LoadBE32(base + out + 4));
  StoreBE32(base + out + 4, std::bit_cast<uint32_t>(std::nextafterf(f, 1e30f)));
}

// Forgets the sum store.
extern "C" void repl_missing_store(PPCContext& ctx, uint8_t* base) {
  uint32_t out = ctx.r5.u32;
  uint32_t before = LoadBE32(base + out);
  Body(ctx, base, 3.0f);
  StoreBE32(base + out, before);
}

// The hook under test, exactly as a port would write it.
REX_SNAP_HOOK(sub_00401000, {ctx.r3.u32, ctx.r4.u32 * 4}, {ctx.r5.u32, 8})

int main() {
  namespace fs = std::filesystem;
  // SNAP_TEST_KEEP_DIR=<dir> keeps the recordings there (for trying the CLI).
  const char* keep = std::getenv("SNAP_TEST_KEEP_DIR");
  fs::path dir = keep ? fs::path(keep)
                      : fs::temp_directory_path() / ("snap_replay_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  setenv("REX_SNAP_DIR", dir.c_str(), 1);
  setenv("REX_SNAP_MAX", "2", 1);
  EXPECT(rex::snapshot::Enabled(), "snapshots enabled through REX_SNAP_DIR");

  // Recording side: a small arena standing in for guest memory.
  snap::Arena rec_arena;
  EXPECT(rec_arena.ok(), "recording arena reserved");
  const uint32_t kArr = 0x0A704000, kOut = 0x0A73A8B0, kStack = 0x7003F000;
  rec_arena.Commit(kArr, 64);
  rec_arena.Commit(kOut, 16);
  rec_arena.Commit(kStack - 0x10000, 0x11000);
  uint8_t* base = rec_arena.base();
  for (uint32_t i = 0; i < 8; ++i) StoreBE32(base + kArr + i * 4, 100 + i);

  PPCContext ctx{};
  ctx.r1.u32 = kStack;
  ctx.r3.u32 = kArr;
  ctx.r4.u32 = 8;
  ctx.r5.u32 = kOut;
  ctx.lr = 0x82000010;
  for (int i = 0; i < 3; ++i) {
    // Reset inputs so every recording sees the same entry state.
    for (uint32_t k = 0; k < 8; ++k) StoreBE32(base + kArr + k * 4, 100 + k);
    ctx.r3.u32 = kArr; ctx.r4.u32 = 8; ctx.r5.u32 = kOut;
    sub_00401000(ctx, base);
  }
  EXPECT(ctx.r3.u32 == 828, "wrapped call still returns the sum");
  EXPECT(rex::snapshot::CountFor(0x00401000) == 2, "REX_SNAP_MAX=2 caps recordings per address");

  std::string first;
  int files = 0;
  for (auto& e : fs::directory_iterator(dir))
    if (e.path().extension() == ".snap") { ++files; if (first.empty() || e.path().string() < first) first = e.path().string(); }
  EXPECT(files == 2, "two .snap files written");
  EXPECT(first.find("00401000_sub_00401000_0000.snap") != std::string::npos, "file name is <ADDR8>_<tag>_<seq>.snap");

  snap::Snapshot s;
  std::string err;
  EXPECT(snap::Load(first, s, err), ("load: " + err).c_str());
  EXPECT(s.has_exit, "recording carries exit state");
  EXPECT(s.header.addr == 0x00401000 && s.tag == "sub_00401000", "header addr and tag");
  EXPECT(s.ranges.size() == 2 && s.ranges[0].rec.size == 32 && s.ranges[1].rec.size == 8, "two ranges as declared");
  EXPECT(s.entry.r[3] == kArr && s.exit.r[3] == 828, "entry/exit r3");
  std::printf("%s", snap::Describe(s).c_str());

  snap::Tolerance tol;
  {
    snap::Arena a; snap::Report r;
    EXPECT(snap::ReplayIdentity(s, a, tol, r), ("identity vs recorded exit: " + snap::Describe(r)).c_str());
  }
  {
    snap::Arena a; snap::Report r;
    EXPECT(snap::Replay(s, a, &__imp__sub_00401000, tol, r), ("replay against original body: " + snap::Describe(r)).c_str());
    EXPECT(r.recorded_writes == r.replacement_writes && r.recorded_writes > 0, "declared writes agree");
  }
  {
    snap::Arena a; snap::Report r;
    EXPECT(!snap::Replay(s, a, &repl_ulp, tol, r), "1-ulp replacement mismatches at tolerance 0");
    bool f1 = false, mem = false;
    for (auto& m : r.mismatches) { f1 |= m.what == "f1"; mem |= m.what == "mem" && m.addr >= kOut + 4 && m.addr < kOut + 8; }
    EXPECT(f1, "mismatch names f1");
    EXPECT(mem, "byte-wise mismatch names the float slot");
  }
  {
    fs::path toml = dir / "native.toml";
    FILE* f = std::fopen(toml.c_str(), "w");
    std::fprintf(f, "[snap.default]\nfloat_ulp = 0\n[snap.\"00401000\"]\nfloat_ulp = 1\nfloat_ranges = [\"%08X+4\"]\n", kOut + 4);
    std::fclose(f);
    snap::Tolerance t2;
    EXPECT(snap::LoadTolerance(toml.string(), 0x00401000, t2, err), ("toml: " + err).c_str());
    EXPECT(t2.float_ulp == 1 && t2.float_ranges.size() == 1, "per-address tolerance overrides default");
    snap::Arena a; snap::Report r;
    EXPECT(snap::Replay(s, a, &repl_ulp, t2, r), ("1-ulp replacement matches at float_ulp=1 with float_ranges: " + snap::Describe(r)).c_str());
    snap::Tolerance t3;
    EXPECT(snap::LoadTolerance(toml.string(), 0x00401004, t3, err) && t3.float_ulp == 0, "other address gets the default");
  }
  {
    snap::Arena a; snap::Report r;
    EXPECT(!snap::Replay(s, a, &repl_missing_store, tol, r), "missing store is a mismatch");
    bool named = false;
    for (auto& m : r.mismatches) named |= m.what == "mem" && m.addr >= kOut && m.addr < kOut + 4 && m.detail.find("replacement did not write here") != std::string::npos;
    EXPECT(named, "mismatch names the un-written address");
    std::printf("%s", snap::Describe(r).c_str());
  }

  if (!keep) fs::remove_all(dir);
  if (g_failures) std::fprintf(stderr, "%d failure(s)\n", g_failures);
  else std::printf("snap_replay_test: all passed\n");
  return g_failures ? 1 : 0;
}
