// snap_replay: load a .snap recording, rebuild its guest state on a private
// arena, run a replacement PPCFunc over it and diff the result against the
// recorded exit. Library half; snap_replay_main.cpp is the CLI and
// snap_replay_test.cpp the identity test.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/snapshot.h>
#include <rex/system/snapshot_format.h>

namespace snap {

struct RangeData {
  rex::snapshot::RangeRecord rec;
  std::vector<uint8_t> entry;
  std::vector<uint8_t> exit;
};

struct Snapshot {
  rex::snapshot::FileHeader header{};
  std::string tag;
  rex::snapshot::RegFile entry{};
  rex::snapshot::RegFile exit{};
  std::vector<RangeData> ranges;
  bool has_exit = false;
};

// Reads a .snap; returns false and fills `error` on a malformed file.
bool Load(const std::string& path, Snapshot& out, std::string& error);

struct AddrRange {
  uint32_t lo, hi;  // inclusive
  bool contains(uint32_t a) const { return a >= lo && a <= hi; }
};

// Per-function comparison tolerances. Parsed from a TOML table:
//
//   [snap.default]
//   float_ulp = 0
//   [snap."8269ADD8"]                 # 8 upper-case hex digits, quoted
//   float_ulp = 2                     # for f1 / fN and float_ranges
//   ignore_ranges = ["0A70462C-0A70463F", "0A73A8B0+16"]
//   float_ranges  = ["0A704000-0A73A8AF"]   # compared as big-endian f32
//   compare_regs  = ["r3", "f1"]      # default; rN fN vN crN lr ctr xer
//   ignore_regs   = []
//   r_bits        = 32                # compare low 32 bits of rN (or 64)
//   stack_bytes   = 65536             # committed below r1 for the callee
struct Tolerance {
  uint32_t float_ulp = 0;
  std::vector<AddrRange> ignore_ranges;
  std::vector<AddrRange> float_ranges;
  std::vector<std::string> compare_regs{"r3", "f1"};
  std::vector<std::string> ignore_regs;
  int r_bits = 32;
  uint32_t stack_bytes = 64 * 1024;
};

// Loads [snap.default] then [snap."ADDR8"] over it. A missing file or missing
// tables leave the defaults. Returns false with `error` on a parse error.
bool LoadTolerance(const std::string& toml_path, uint32_t addr, Tolerance& out,
                   std::string& error);
// Parses "LO-HI" (inclusive) or "BASE+LEN" hex forms.
bool ParseAddrRange(const std::string& text, AddrRange& out);

// A 4 GiB reserved region so `base + guest_addr` is valid for any u32; only
// the recorded ranges (and a stack window) are committed.
class Arena {
 public:
  Arena();
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  uint8_t* base() const { return base_; }
  bool ok() const { return base_ != nullptr; }
  // Commit [addr, addr+size) read/write (page-rounded), zero-filled.
  bool Commit(uint32_t addr, uint32_t size);

 private:
  uint8_t* base_ = nullptr;
  size_t reserved_ = 0;
};

struct Mismatch {
  std::string what;  // "r3", "f1", "mem"
  uint32_t addr = 0;  // for mem
  std::string detail;
};

struct Report {
  std::vector<Mismatch> mismatches;
  size_t bytes_compared = 0;
  size_t bytes_ignored = 0;
  size_t recorded_writes = 0;     // bytes the recorded call changed
  size_t replacement_writes = 0;  // bytes the replacement changed
  bool ok() const { return mismatches.empty(); }
};

// Materialise `snap` on `arena`, call `fn`, diff. `max_mismatches` bounds the
// report (0 = unbounded). Returns report.ok().
bool Replay(const Snapshot& snap, Arena& arena, PPCFunc* fn, const Tolerance& tol, Report& out,
            size_t max_mismatches = 32);

// Same, but materialises the recorded exit state instead of calling anything:
// proves loader and differ agree with each other on this file.
bool ReplayIdentity(const Snapshot& snap, Arena& arena, const Tolerance& tol, Report& out);

std::string Describe(const Snapshot& snap);
std::string Describe(const Report& report);

// ULP distance between two doubles (or floats); huge for NaN vs number.
uint64_t UlpDistance(double a, double b);
uint32_t UlpDistance(float a, float b);

}  // namespace snap
