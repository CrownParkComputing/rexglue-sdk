// snap_replay library: see snap_replay.h.
#include "snap_replay.h"

#include <bit>
#include <cctype>
#include <limits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace snap {

using namespace rex::snapshot;

//------------------------------------------------------------------------------
// Loading
//------------------------------------------------------------------------------

namespace {

bool ReadExact(FILE* f, void* dst, size_t n) { return n == 0 || std::fread(dst, 1, n, f) == n; }

}  // namespace

bool Load(const std::string& path, Snapshot& out, std::string& error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  auto fail = [&](const char* why) {
    error = path + ": " + why;
    std::fclose(f);
    return false;
  };
  if (!ReadExact(f, &out.header, sizeof(out.header))) return fail("short header");
  if (std::memcmp(out.header.magic, kMagic, sizeof(kMagic)) != 0) return fail("bad magic");
  if (out.header.version != kVersion) return fail("unsupported version");
  if (out.header.header_size != sizeof(FileHeader)) return fail("header size mismatch");
  out.tag.resize(out.header.tag_len);
  if (!ReadExact(f, out.tag.data(), out.header.tag_len)) return fail("short tag");
  uint32_t pad = (4 - (out.header.tag_len & 3)) & 3;
  char padbuf[4];
  if (!ReadExact(f, padbuf, pad)) return fail("short tag pad");
  out.ranges.resize(out.header.range_count);
  for (auto& r : out.ranges)
    if (!ReadExact(f, &r.rec, sizeof(r.rec))) return fail("short range table");
  if (!ReadExact(f, &out.entry, sizeof(out.entry))) return fail("short entry registers");
  for (auto& r : out.ranges) {
    r.entry.resize(r.rec.stored);
    if (!ReadExact(f, r.entry.data(), r.rec.stored)) return fail("short entry bytes");
  }
  out.has_exit = (out.header.flags & kHasExit) != 0;
  if (out.has_exit) {
    if (!ReadExact(f, &out.exit, sizeof(out.exit))) return fail("short exit registers");
    for (auto& r : out.ranges) {
      r.exit.resize(r.rec.stored);
      if (!ReadExact(f, r.exit.data(), r.rec.stored)) return fail("short exit bytes");
    }
  }
  std::fclose(f);
  return true;
}

//------------------------------------------------------------------------------
// Tolerances
//------------------------------------------------------------------------------

bool ParseAddrRange(const std::string& text, AddrRange& out) {
  size_t dash = text.find('-');
  size_t plus = text.find('+');
  char* end = nullptr;
  if (dash != std::string::npos) {
    out.lo = static_cast<uint32_t>(std::strtoul(text.substr(0, dash).c_str(), &end, 16));
    if (!end || *end) return false;
    out.hi = static_cast<uint32_t>(std::strtoul(text.substr(dash + 1).c_str(), &end, 16));
    return end && !*end && out.hi >= out.lo;
  }
  if (plus != std::string::npos) {
    out.lo = static_cast<uint32_t>(std::strtoul(text.substr(0, plus).c_str(), &end, 16));
    if (!end || *end) return false;
    uint32_t len = static_cast<uint32_t>(std::strtoul(text.substr(plus + 1).c_str(), &end, 16));
    if (!end || *end || len == 0) return false;
    out.hi = out.lo + len - 1;
    return true;
  }
  return false;
}

namespace {

bool ApplyTable(const toml::table& t, Tolerance& tol, std::string& error) {
  if (auto v = t["float_ulp"].value<int64_t>()) tol.float_ulp = static_cast<uint32_t>(*v);
  if (auto v = t["r_bits"].value<int64_t>()) tol.r_bits = static_cast<int>(*v);
  if (auto v = t["stack_bytes"].value<int64_t>()) tol.stack_bytes = static_cast<uint32_t>(*v);
  auto ranges = [&](const char* key, std::vector<AddrRange>& dst) {
    if (auto* arr = t[key].as_array()) {
      dst.clear();
      for (auto& e : *arr) {
        AddrRange r;
        auto s = e.value<std::string>();
        if (!s || !ParseAddrRange(*s, r)) {
          error = std::string("bad range in ") + key;
          return false;
        }
        dst.push_back(r);
      }
    }
    return true;
  };
  auto strings = [&](const char* key, std::vector<std::string>& dst) {
    if (auto* arr = t[key].as_array()) {
      dst.clear();
      for (auto& e : *arr)
        if (auto s = e.value<std::string>()) dst.push_back(*s);
    }
  };
  if (!ranges("ignore_ranges", tol.ignore_ranges)) return false;
  if (!ranges("float_ranges", tol.float_ranges)) return false;
  strings("compare_regs", tol.compare_regs);
  strings("ignore_regs", tol.ignore_regs);
  return true;
}

}  // namespace

bool LoadTolerance(const std::string& toml_path, uint32_t addr, Tolerance& out,
                   std::string& error) {
  if (toml_path.empty()) return true;
  toml::parse_result res = toml::parse_file(toml_path);
  if (!res) {
    std::ostringstream ss;
    ss << toml_path << ": " << res.error().description();
    error = ss.str();
    return false;
  }
  const toml::table& root = res.table();
  auto* snap_tbl = root["snap"].as_table();
  if (!snap_tbl) return true;
  if (auto* d = (*snap_tbl)["default"].as_table())
    if (!ApplyTable(*d, out, error)) return false;
  char key[16];
  std::snprintf(key, sizeof(key), "%08X", addr);
  if (auto* d = (*snap_tbl)[key].as_table())
    if (!ApplyTable(*d, out, error)) return false;
  return true;
}

//------------------------------------------------------------------------------
// Arena
//------------------------------------------------------------------------------

Arena::Arena() {
  reserved_ = size_t{1} << 32;
#if defined(_WIN32)
  base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, reserved_, MEM_RESERVE, PAGE_NOACCESS));
#else
  void* p = mmap(nullptr, reserved_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  base_ = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

Arena::~Arena() {
  if (!base_) return;
#if defined(_WIN32)
  VirtualFree(base_, 0, MEM_RELEASE);
#else
  munmap(base_, reserved_);
#endif
}

bool Arena::Commit(uint32_t addr, uint32_t size) {
  if (!base_ || size == 0) return false;
  constexpr uint64_t kPage = 4096;
  uint64_t lo = addr & ~(kPage - 1);
  uint64_t hi = (uint64_t{addr} + size + kPage - 1) & ~(kPage - 1);
  if (hi > reserved_) hi = reserved_;
#if defined(_WIN32)
  return VirtualAlloc(base_ + lo, hi - lo, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
  return mprotect(base_ + lo, hi - lo, PROT_READ | PROT_WRITE) == 0;
#endif
}

//------------------------------------------------------------------------------
// Comparison
//------------------------------------------------------------------------------

uint64_t UlpDistance(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return 0;
  if (std::isnan(a) || std::isnan(b)) return ~uint64_t{0};
  if (a == b) return 0;  // covers +0/-0
  auto key = [](double d) {
    int64_t i = std::bit_cast<int64_t>(d);
    return i < 0 ? (std::numeric_limits<int64_t>::min() - i) : i;
  };
  int64_t ka = key(a), kb = key(b);
  return static_cast<uint64_t>(ka > kb ? ka - kb : kb - ka);
}

uint32_t UlpDistance(float a, float b) {
  if (std::isnan(a) && std::isnan(b)) return 0;
  if (std::isnan(a) || std::isnan(b)) return ~uint32_t{0};
  if (a == b) return 0;
  auto key = [](float d) {
    int32_t i = std::bit_cast<int32_t>(d);
    return i < 0 ? (std::numeric_limits<int32_t>::min() - i) : i;
  };
  int32_t ka = key(a), kb = key(b);
  return static_cast<uint32_t>(ka > kb ? ka - kb : kb - ka);
}

namespace {

bool InAny(const std::vector<AddrRange>& rs, uint32_t a) {
  for (auto& r : rs)
    if (r.contains(a)) return true;
  return false;
}

bool Listed(const std::vector<std::string>& v, const std::string& s) {
  for (auto& e : v)
    if (e == s) return true;
  return false;
}

std::string Hex(uint64_t v, int digits) {
  char b[32];
  std::snprintf(b, sizeof(b), "%0*llx", digits, static_cast<unsigned long long>(v));
  return b;
}

void CompareRegs(const RegFile& want, const RegFile& got, const Tolerance& tol, Report& out,
                 size_t max_mismatches) {
  auto add = [&](const std::string& what, const std::string& detail) {
    if (max_mismatches && out.mismatches.size() >= max_mismatches) return;
    out.mismatches.push_back({what, 0, detail});
  };
  for (const std::string& reg : tol.compare_regs) {
    if (Listed(tol.ignore_regs, reg)) continue;
    if (reg.size() >= 2 && reg[0] == 'r' && std::isdigit(static_cast<unsigned char>(reg[1]))) {
      int i = std::atoi(reg.c_str() + 1);
      if (i < 0 || i > 31) continue;
      uint64_t mask = tol.r_bits >= 64 ? ~uint64_t{0} : 0xFFFFFFFFull;
      if ((want.r[i] & mask) != (got.r[i] & mask))
        add(reg, "want " + Hex(want.r[i] & mask, tol.r_bits / 4) + " got " +
                     Hex(got.r[i] & mask, tol.r_bits / 4));
    } else if (reg.size() >= 2 && reg[0] == 'f' &&
               std::isdigit(static_cast<unsigned char>(reg[1]))) {
      int i = std::atoi(reg.c_str() + 1);
      if (i < 0 || i > 31) continue;
      double a = std::bit_cast<double>(want.f[i]), b = std::bit_cast<double>(got.f[i]);
      uint64_t d = UlpDistance(a, b);
      if (d > tol.float_ulp) {
        char b1[128];
        std::snprintf(b1, sizeof(b1), "want %.17g got %.17g (%llu ulp, tolerance %u)", a, b,
                      static_cast<unsigned long long>(d), tol.float_ulp);
        add(reg, b1);
      }
    } else if (reg.size() >= 2 && reg[0] == 'v' &&
               std::isdigit(static_cast<unsigned char>(reg[1]))) {
      int i = std::atoi(reg.c_str() + 1);
      if (i < 0 || i > 127) continue;
      if (std::memcmp(want.v[i], got.v[i], 16) != 0) {
        std::string w, g;
        for (int k = 0; k < 16; ++k) {
          w += Hex(want.v[i][k], 2);
          g += Hex(got.v[i][k], 2);
        }
        add(reg, "want " + w + " got " + g);
      }
    } else if (reg.size() >= 3 && reg.compare(0, 2, "cr") == 0) {
      int i = std::atoi(reg.c_str() + 2);
      if (i < 0 || i > 7) continue;
      if (want.cr[i] != got.cr[i]) add(reg, "want " + Hex(want.cr[i], 1) + " got " + Hex(got.cr[i], 1));
    } else if (reg == "lr") {
      if (want.lr != got.lr) add(reg, "want " + Hex(want.lr, 8) + " got " + Hex(got.lr, 8));
    } else if (reg == "ctr") {
      if (want.ctr != got.ctr) add(reg, "want " + Hex(want.ctr, 8) + " got " + Hex(got.ctr, 8));
    } else if (reg == "xer") {
      if (want.xer_so != got.xer_so || want.xer_ov != got.xer_ov || want.xer_ca != got.xer_ca)
        add(reg, "so/ov/ca differ");
    }
  }
}

float LoadBEFloat(const uint8_t* p) {
  uint32_t u = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
  return std::bit_cast<float>(u);
}

void CompareMemory(const Snapshot& snap, const uint8_t* base, const Tolerance& tol, Report& out,
                   size_t max_mismatches) {
  auto add = [&](uint32_t addr, const std::string& detail) {
    if (max_mismatches && out.mismatches.size() >= max_mismatches) return;
    out.mismatches.push_back({"mem", addr, detail});
  };
  for (const RangeData& r : snap.ranges) {
    const uint8_t* got = base + r.rec.addr;
    uint32_t n = r.rec.stored;
    for (uint32_t i = 0; i < n;) {
      uint32_t a = r.rec.addr + i;
      if (InAny(tol.ignore_ranges, a)) {
        out.bytes_ignored++;
        i++;
        continue;
      }
      // Float slot: 4 aligned bytes inside a float range compared by ULP.
      if ((a & 3) == 0 && i + 4 <= n && InAny(tol.float_ranges, a) &&
          InAny(tol.float_ranges, a + 3)) {
        float w = LoadBEFloat(&r.exit[i]), g = LoadBEFloat(got + i);
        out.bytes_compared += 4;
        if (std::memcmp(&r.entry[i], &r.exit[i], 4) != 0) out.recorded_writes += 4;
        if (std::memcmp(&r.entry[i], got + i, 4) != 0) out.replacement_writes += 4;
        uint32_t d = UlpDistance(w, g);
        if (d > tol.float_ulp) {
          char b[160];
          std::snprintf(b, sizeof(b), "f32 want %.9g got %.9g (%u ulp, tolerance %u)%s", w, g, d,
                        tol.float_ulp,
                        std::memcmp(&r.entry[i], &r.exit[i], 4) == 0 ? " [recorded call did not write here]" : "");
          add(a, b);
        }
        i += 4;
        continue;
      }
      out.bytes_compared++;
      bool rec_wrote = r.entry[i] != r.exit[i];
      bool rep_wrote = r.entry[i] != got[i];
      if (rec_wrote) out.recorded_writes++;
      if (rep_wrote) out.replacement_writes++;
      if (r.exit[i] != got[i]) {
        std::string d = "want " + Hex(r.exit[i], 2) + " got " + Hex(got[i], 2) + " (entry " +
                        Hex(r.entry[i], 2) + ")";
        if (!rec_wrote) d += " [recorded call did not write here]";
        if (!rep_wrote) d += " [replacement did not write here]";
        add(a, d);
      }
      i++;
    }
  }
}

bool Materialise(const Snapshot& snap, Arena& arena, const Tolerance& tol, PPCContext& ctx,
                 Report& out) {
  if (!arena.ok()) {
    out.mismatches.push_back({"arena", 0, "cannot reserve 4 GiB guest arena"});
    return false;
  }
  for (const RangeData& r : snap.ranges) {
    if (r.rec.stored == 0) continue;
    if (!arena.Commit(r.rec.addr, r.rec.stored)) {
      out.mismatches.push_back({"arena", r.rec.addr, "commit failed"});
      return false;
    }
    std::memcpy(arena.base() + r.rec.addr, r.entry.data(), r.rec.stored);
  }
  Restore(snap.entry, ctx);
  // Stack window: the callee may push a frame below r1 even if the recorder
  // did not declare the stack. Zero-filled; a function that reads its caller's
  // frame needs the range declared to see real contents.
  uint32_t r1 = static_cast<uint32_t>(snap.entry.r[1]);
  if (r1 && tol.stack_bytes) {
    uint32_t lo = r1 > tol.stack_bytes ? r1 - tol.stack_bytes : 0;
    arena.Commit(lo, tol.stack_bytes + 16 * 1024);  // committing over a range is harmless
    // Re-apply any declared range that overlaps the stack window (Commit does
    // not clear already-committed pages, but be explicit about precedence).
    for (const RangeData& r : snap.ranges)
      if (r.rec.stored) std::memcpy(arena.base() + r.rec.addr, r.entry.data(), r.rec.stored);
  }
  return true;
}

}  // namespace

bool Replay(const Snapshot& snap, Arena& arena, PPCFunc* fn, const Tolerance& tol, Report& out,
            size_t max_mismatches) {
  if (!snap.has_exit) {
    out.mismatches.push_back({"file", 0, "recording has no exit state (call never returned)"});
    return false;
  }
  PPCContext ctx{};
  if (!Materialise(snap, arena, tol, ctx, out)) return false;
  fn(ctx, arena.base());
  RegFile got;
  Capture(ctx, got);
  CompareRegs(snap.exit, got, tol, out, max_mismatches);
  CompareMemory(snap, arena.base(), tol, out, max_mismatches);
  return out.ok();
}

bool ReplayIdentity(const Snapshot& snap, Arena& arena, const Tolerance& tol, Report& out) {
  if (!snap.has_exit) {
    out.mismatches.push_back({"file", 0, "recording has no exit state (call never returned)"});
    return false;
  }
  PPCContext ctx{};
  if (!Materialise(snap, arena, tol, ctx, out)) return false;
  Restore(snap.exit, ctx);
  for (const RangeData& r : snap.ranges)
    if (r.rec.stored) std::memcpy(arena.base() + r.rec.addr, r.exit.data(), r.rec.stored);
  RegFile got;
  Capture(ctx, got);
  CompareRegs(snap.exit, got, tol, out, 0);
  CompareMemory(snap, arena.base(), tol, out, 0);
  return out.ok();
}

std::string Describe(const Snapshot& snap) {
  std::ostringstream ss;
  ss << "addr " << Hex(snap.header.addr, 8) << " tag '" << snap.tag << "' ranges "
     << snap.ranges.size() << (snap.has_exit ? "" : " (NO EXIT)") << "\n";
  ss << "  entry r3=" << Hex(snap.entry.r[3], 8) << " r4=" << Hex(snap.entry.r[4], 8)
     << " r5=" << Hex(snap.entry.r[5], 8) << " r6=" << Hex(snap.entry.r[6], 8)
     << " r1=" << Hex(snap.entry.r[1], 8) << " lr=" << Hex(snap.entry.lr, 8) << "\n";
  if (snap.has_exit) {
    ss << "  exit  r3=" << Hex(snap.exit.r[3], 8) << " f1=" << std::bit_cast<double>(snap.exit.f[1])
       << "\n";
  }
  for (const RangeData& r : snap.ranges) {
    size_t changed = 0;
    if (snap.has_exit)
      for (uint32_t i = 0; i < r.rec.stored; ++i) changed += r.entry[i] != r.exit[i];
    ss << "  range " << Hex(r.rec.addr, 8) << "+" << Hex(r.rec.size, 1) << " stored "
       << r.rec.stored << (r.rec.flags & kTruncated ? " (truncated)" : "") << " changed "
       << changed << "\n";
  }
  return ss.str();
}

std::string Describe(const Report& report) {
  std::ostringstream ss;
  ss << (report.ok() ? "MATCH" : "MISMATCH") << ": " << report.mismatches.size()
     << " mismatches, " << report.bytes_compared << " bytes compared, " << report.bytes_ignored
     << " ignored, recorded call wrote " << report.recorded_writes
     << " bytes, replacement wrote " << report.replacement_writes << "\n";
  for (const Mismatch& m : report.mismatches) {
    ss << "  " << m.what;
    if (m.what == "mem" || m.what == "arena") ss << " " << Hex(m.addr, 8);
    ss << ": " << m.detail << "\n";
  }
  return ss.str();
}

}  // namespace snap
