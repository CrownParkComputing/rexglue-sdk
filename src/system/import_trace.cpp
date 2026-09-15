/**
 * @file        system/import_trace.cpp
 * @brief       Record which kernel imports a title actually calls
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// A title's XEX import table says what it MIGHT call. Replacing ReXGlue for one
// title needs what it DOES call, which is a much shorter list - Geometry Wars
// imports 142 kernel functions, and the set it reaches at runtime is the actual
// work. Set REX_TRACE_IMPORTS to a path and every import is recorded the first
// time it runs; the file is written at exit, one name per line.
//
// An environment variable rather than a cvar on purpose: imports are called
// before command-line parsing finishes, so a cvar would miss the boot sequence,
// which is exactly the part that matters.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

namespace rex {

namespace {

std::mutex& TraceMutex() {
  static std::mutex m;
  return m;
}

std::set<std::string>& TraceSet() {
  static std::set<std::string> s;
  return s;
}

const char* TracePath() {
  static const char* path = std::getenv("REX_TRACE_IMPORTS");
  return path;
}

// Appended as each import is first seen, not written at exit. A title ends by
// hard-exiting and a timed run ends with SIGTERM, and neither runs static
// destructors - an exit-time write produced no file at all.
FILE* TraceFile() {
  static FILE* f = TracePath() ? std::fopen(TracePath(), "w") : nullptr;
  return f;
}

}  // namespace

bool ImportTraceEnabled() {
  static const bool enabled = TracePath() != nullptr;
  return enabled;
}

void NoteImportUsed(const char* name) {
  if (!ImportTraceEnabled()) {
    return;
  }
  std::lock_guard<std::mutex> lock(TraceMutex());
  if (!TraceSet().insert(name).second) {
    return;
  }
  if (FILE* f = TraceFile()) {
    std::fprintf(f, "%s\n", name);
    std::fflush(f);
  }
}

}  // namespace rex
