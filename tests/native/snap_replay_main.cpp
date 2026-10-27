// snap_replay CLI.
//
//   snap_replay <file.snap> [--toml native.toml] [--dump] [--identity]
//               [--plugin lib.so [--symbol sub_XXXXXXXX]] [--max N] [--float-ulp N]
//
// --dump      print the recording (header, entry/exit summary, ranges) and exit
// --identity  replay the recorded exit state against itself (loader/differ check)
// --plugin    dlopen a shared object holding the replacement; --symbol names
//             it (default: sub_<ADDR8> from the file header)
// Exit status 0 on match, 1 on mismatch, 2 on usage or load error.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "snap_replay.h"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

int main(int argc, char** argv) {
  std::string file, toml, plugin, symbol;
  bool dump = false, identity = false;
  size_t max_mismatches = 32;
  int float_ulp = -1;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](std::string& dst) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", a.c_str());
        std::exit(2);
      }
      dst = argv[++i];
    };
    if (a == "--toml") next(toml);
    else if (a == "--plugin") next(plugin);
    else if (a == "--symbol") next(symbol);
    else if (a == "--dump") dump = true;
    else if (a == "--identity") identity = true;
    else if (a == "--max") { std::string v; next(v); max_mismatches = std::strtoul(v.c_str(), nullptr, 10); }
    else if (a == "--float-ulp") { std::string v; next(v); float_ulp = std::atoi(v.c_str()); }
    else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    else file = a;
  }
  if (file.empty()) {
    std::fprintf(stderr, "usage: snap_replay <file.snap> [--toml native.toml] [--dump] [--identity] "
                         "[--plugin lib.so [--symbol name]] [--max N] [--float-ulp N]\n");
    return 2;
  }
  snap::Snapshot s;
  std::string err;
  if (!snap::Load(file, s, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  std::printf("%s", snap::Describe(s).c_str());
  if (dump && !identity && plugin.empty()) return 0;

  snap::Tolerance tol;
  if (!snap::LoadTolerance(toml, s.header.addr, tol, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  if (float_ulp >= 0) tol.float_ulp = static_cast<uint32_t>(float_ulp);

  snap::Arena arena;
  snap::Report report;
  bool ok;
  if (identity) {
    ok = snap::ReplayIdentity(s, arena, tol, report);
  } else {
#if defined(_WIN32)
    std::fprintf(stderr, "--plugin is not supported on this platform\n");
    return 2;
#else
    void* h = dlopen(plugin.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
      std::fprintf(stderr, "dlopen %s: %s\n", plugin.c_str(), dlerror());
      return 2;
    }
    if (symbol.empty()) {
      char b[32];
      std::snprintf(b, sizeof(b), "sub_%08X", s.header.addr);
      symbol = b;
    }
    auto* fn = reinterpret_cast<PPCFunc*>(dlsym(h, symbol.c_str()));
    if (!fn) {
      std::fprintf(stderr, "dlsym %s: %s\n", symbol.c_str(), dlerror());
      return 2;
    }
    ok = snap::Replay(s, arena, fn, tol, report, max_mismatches);
#endif
  }
  std::printf("%s", snap::Describe(report).c_str());
  return ok ? 0 : 1;
}
