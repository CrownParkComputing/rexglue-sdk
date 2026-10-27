/**
 * @file        system/reach_trace.cpp
 * @brief       Per-function reach counters for recompiled guest code
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Companion of import_trace.cpp for guest functions instead of kernel
// imports. Slots live in the generated code (one static per function, see
// REX_FUNC_PROLOGUE under REX_GUEST_REACH in the pch template); this file
// only keeps the registry and writes it out.
//
// Design constraints:
//   * Register() runs on guest threads (fibers) from inside recompiled code:
//     no locks that a signal handler could then find held. The registry is a
//     lock-free intrusive stack (CAS on the head).
//   * Dump() runs from atexit, from a SIGTERM/SIGINT handler and from a
//     periodic thread: no malloc, no stdio, no locks. It formats into a stack
//     buffer and writes with write(2), to a temp file renamed over the target
//     so a reader never sees a half-written file.
//   * An environment variable rather than a cvar, like REX_TRACE_IMPORTS:
//     guest code runs before command-line parsing is done with, and the boot
//     path is exactly what must be counted.

#include <rex/system/reach_trace.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#define REX_REACH_OPEN(p) _open((p), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644)
#define REX_REACH_WRITE _write
#define REX_REACH_CLOSE _close
#define REX_REACH_RENAME(a, b) (MoveFileExA((a), (b), MOVEFILE_REPLACE_EXISTING) ? 0 : -1)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#define REX_REACH_OPEN(p) ::open((p), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)
#define REX_REACH_WRITE ::write
#define REX_REACH_CLOSE ::close
#define REX_REACH_RENAME(a, b) ::rename((a), (b))
#endif

namespace rex::reach {

namespace {

std::atomic<Slot*> g_head{nullptr};
std::atomic<int> g_installed{0};
std::atomic<int> g_dumping{0};

const char* TracePath() {
  static const char* path = std::getenv("REX_TRACE_REACH");
  return path;
}

unsigned IntervalMs() {
  static const unsigned ms = [] {
    const char* v = std::getenv("REX_TRACE_REACH_INTERVAL_MS");
    return v ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 5000u;
  }();
  return ms;
}

// Parse the hex address following "sub_" in a symbol name. Returns 0 when
// the name carries no address (a hand-written function using the macro).
uint32_t ParseAddress(const char* name) {
  if (!name) return 0;
  const char* p = std::strstr(name, "sub_");
  if (!p) return 0;
  p += 4;
  uint32_t v = 0;
  int digits = 0;
  for (; *p && digits < 8; ++p, ++digits) {
    char c = *p;
    uint32_t d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else break;
    v = (v << 4) | d;
  }
  return digits ? v : 0;
}

// Async-signal-safe buffered writer.
struct Writer {
  int fd;
  char buf[1 << 14];
  size_t used = 0;
  bool failed = false;

  void Flush() {
    size_t off = 0;
    while (off < used && !failed) {
      auto n = REX_REACH_WRITE(fd, buf + off, static_cast<unsigned>(used - off));
      if (n <= 0) { failed = true; break; }
      off += static_cast<size_t>(n);
    }
    used = 0;
  }
  void Put(const char* s, size_t n) {
    if (n > sizeof(buf) - used) Flush();
    std::memcpy(buf + used, s, n);
    used += n;
  }
  void PutStr(const char* s) { Put(s, std::strlen(s)); }
  void PutHex32(uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    char tmp[8];
    for (int i = 7; i >= 0; --i) { tmp[i] = digits[v & 0xF]; v >>= 4; }
    Put(tmp, 8);
  }
  void PutDec64(uint64_t v) {
    char tmp[20];
    int i = 20;
    do { tmp[--i] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    Put(tmp + i, static_cast<size_t>(20 - i));
  }
};

size_t WriteAll(const char* path) {
  // Rename over the target so a concurrent reader never sees a partial file.
  char tmp[1024];
  size_t len = std::strlen(path);
  if (len + 5 >= sizeof(tmp)) return 0;
  std::memcpy(tmp, path, len);
  std::memcpy(tmp + len, ".tmp", 5);

  int fd = REX_REACH_OPEN(tmp);
  if (fd < 0) return 0;
  static Writer w;  // static: 16 KB is too much for a signal-handler stack
  w.fd = fd;
  w.used = 0;
  w.failed = false;
  w.PutStr("addr,count\n");
  size_t n = 0;
  for (Slot* s = g_head.load(std::memory_order_acquire); s; s = s->next) {
    uint64_t c = s->count.load(std::memory_order_relaxed);
    if (s->addr == 0) {
      // Name without a guest address: emit the name so nothing is lost.
      w.PutStr(s->name ? s->name : "?");
    } else {
      w.PutHex32(s->addr);
    }
    w.Put(",", 1);
    w.PutDec64(c);
    w.Put("\n", 1);
    ++n;
  }
  w.Flush();
  REX_REACH_CLOSE(fd);
  if (w.failed || REX_REACH_RENAME(tmp, path) != 0) return 0;
  return n;
}

void AtExitDump() { Dump(); }

#if !defined(_WIN32)
struct sigaction g_prev_term;
struct sigaction g_prev_int;

void OnSignal(int sig, siginfo_t* info, void* uctx) {
  Dump();
  const struct sigaction& prev = (sig == SIGTERM) ? g_prev_term : g_prev_int;
  if (prev.sa_flags & SA_SIGINFO) {
    if (prev.sa_sigaction) prev.sa_sigaction(sig, info, uctx);
    return;
  }
  if (prev.sa_handler == SIG_IGN) return;
  if (prev.sa_handler != SIG_DFL && prev.sa_handler != nullptr) {
    prev.sa_handler(sig);
    return;
  }
  // Default disposition: restore it and re-raise so the exit status is the
  // one the caller expects (a timed run under `timeout` relies on this).
  sigaction(sig, &prev, nullptr);
  raise(sig);
}
#endif

void InstallOnce() {
  int expected = 0;
  if (!g_installed.compare_exchange_strong(expected, 1)) return;
  if (!Enabled()) return;
  std::atexit(AtExitDump);
#if !defined(_WIN32)
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = OnSignal;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, &g_prev_term);
  sigaction(SIGINT, &sa, &g_prev_int);
#endif
  if (unsigned ms = IntervalMs()) {
    std::thread([ms] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        Dump();
      }
    }).detach();
  }
}

}  // namespace

bool Enabled() {
  static const bool enabled = TracePath() != nullptr;
  return enabled;
}

void Register(Slot* slot, const char* name) {
  slot->name = name;
  slot->addr = ParseAddress(name);
  Slot* head = g_head.load(std::memory_order_relaxed);
  do {
    slot->next = head;
  } while (!g_head.compare_exchange_weak(head, slot, std::memory_order_release,
                                         std::memory_order_relaxed));
  InstallOnce();
}

size_t DumpTo(const char* path) {
  if (!path) return 0;
  // A signal arriving while the periodic thread is mid-dump: skip rather
  // than interleave. The next dump picks up the counts anyway.
  int expected = 0;
  if (!g_dumping.compare_exchange_strong(expected, 1)) return 0;
  size_t n = WriteAll(path);
  g_dumping.store(0);
  return n;
}

size_t Dump() {
  if (!Enabled()) return 0;
  return DumpTo(TracePath());
}

}  // namespace rex::reach
