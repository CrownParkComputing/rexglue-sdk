/**
 * @file        src/system/guest_memory_watch.cpp
 * @brief       Sampler that reports changes to a guest memory range.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/guest_memory_watch.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/types.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(
    guest_watch, "", "Kernel",
    "Report every change to one or more guest memory ranges, as "
    "'address:length' separated by commas - e.g. '0x830F2A48:256'. Empty "
    "disables. Use on a stalled bring-up to see which structure stopped being "
    "filled in, and when.");
REXCVAR_DEFINE_UINT32(guest_watch_ms, 50, "Kernel",
                      "Sampling interval for --guest_watch, in milliseconds.");
REXCVAR_DEFINE_BOOL(
    guest_watch_writers, true, "Kernel",
    "Also name the guest code that writes a --guest_watch range, by protecting its pages and "
    "reporting the faulting lr. Costs a page fault per write to those pages; turn off for a "
    "range written in a hot loop.");
REXCVAR_DEFINE_UINT32(
    guest_watch_rearm_us, 1000, "Kernel",
    "How soon a --guest_watch page is re-protected after a write is let through, in microseconds. "
    "Protection is page-granular, so a hot neighbour on the same page keeps opening the window "
    "this closes, and the guest's own page protection undoes the watch outright; lower "
    "catches more writers and costs more syscalls.");
REXCVAR_DEFINE_UINT32(guest_watch_find, 0, "Kernel",
                      "Instead of a fixed address, poll guest memory for this 32-bit value and "
                      "watch wherever it turns up. For anything the title allocates, the address "
                      "differs every run, so it has to be found inside the run that watches it.");
REXCVAR_DEFINE_UINT32(guest_watch_find_from, 0x00010000, "Kernel",
                      "Lowest guest address guest_watch_find searches.");
REXCVAR_DEFINE_UINT32(guest_watch_find_to, 0x20000000, "Kernel",
                      "Highest guest address guest_watch_find searches.");
REXCVAR_DEFINE_UINT32(guest_watch_find_length, 4, "Kernel",
                      "Bytes to watch around a guest_watch_find match.");
REXCVAR_DEFINE_UINT32(guest_watch_find_pair, 0, "Kernel",
                      "Require the word before a guest_watch_find match to equal this.");
REXCVAR_DEFINE_UINT32(guest_watch_find_delay_ms, 0, "Kernel",
                      "Wait this long before searching, so the allocation exists.");
REXCVAR_DEFINE_BOOL(guest_watch_words, true, "Kernel",
                    "Report --guest_watch changes as big-endian 32-bit words (the guest's own "
                    "view). Off reports raw bytes, for unaligned or byte-packed data.");

namespace rex::system {

namespace {

struct WatchRange {
  uint32_t address;
  uint32_t length;
  std::vector<uint8_t> shadow;
  bool primed = false;
};

std::atomic<bool> g_started{false};
// Read on every access violation in the process, so it must stay cheap and
// must be false unless a watch is actually armed.
std::atomic<bool> g_active{false};

struct ArmedPage {
  uint8_t* host_address;   // page-aligned base of the protected span
  uint32_t length;         // protected span, in bytes
  uint8_t* watch_first;    // first byte the user actually asked about
  uint8_t* watch_last;     // last byte, inclusive
};

std::mutex g_armed_lock;
std::vector<ArmedPage> g_armed;

// "0x830F2A48:256,0x40010D2C:64" -> two ranges. A range with no length gets
// one word, which is the common "what writes this pointer" case.
std::vector<WatchRange> ParseRanges(const std::string& spec) {
  std::vector<WatchRange> ranges;
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t comma = spec.find(',', pos);
    std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos = comma == std::string::npos ? spec.size() : comma + 1;
    if (item.empty()) {
      continue;
    }
    size_t colon = item.find(':');
    std::string address_text = item.substr(0, colon);
    std::string length_text = colon == std::string::npos ? std::string() : item.substr(colon + 1);
    uint32_t address = 0, length = 4;
    try {
      address = static_cast<uint32_t>(std::stoul(address_text, nullptr, 0));
      if (!length_text.empty()) {
        length = static_cast<uint32_t>(std::stoul(length_text, nullptr, 0));
      }
    } catch (const std::exception&) {
      REXLOG_ERROR("[GWATCH] cannot parse range '{}' - expected address:length", item);
      continue;
    }
    if (!address || !length) {
      REXLOG_ERROR("[GWATCH] ignoring empty range '{}'", item);
      continue;
    }
    // A sampler that walks megabytes cannot keep up with the interval, and a
    // diff that long is unreadable anyway.
    constexpr uint32_t kMaxLength = 64 * 1024;
    if (length > kMaxLength) {
      REXLOG_WARN("[GWATCH] {:#010x}: clamping {} bytes to {}", address, length, kMaxLength);
      length = kMaxLength;
    }
    WatchRange range;
    range.address = address;
    range.length = length;
    range.shadow.resize(length);
    ranges.push_back(std::move(range));
  }
  return ranges;
}

// Arms every page overlapping the range. A page is shared with whatever else
// lives on it, so unrelated writes fault too; they are reported and let
// through like any other, which is noise but never wrong.
void ArmRangeLocked(rex::memory::Memory* memory, uint32_t address, uint32_t length) {
  const size_t page_size = rex::memory::page_size();
  auto* host = memory->TranslateVirtual<uint8_t*>(address);
  if (!host) {
    return;
  }
  auto* page = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(host) & ~(page_size - 1));
  const size_t span =
      ((reinterpret_cast<uintptr_t>(host) + length + page_size - 1) & ~(uintptr_t(page_size) - 1)) -
      reinterpret_cast<uintptr_t>(page);
  if (!rex::memory::Protect(page, span, rex::memory::PageAccess::kReadOnly, nullptr)) {
    REXLOG_WARN("[GWATCH] could not protect {:#010x} - writer names unavailable", address);
    return;
  }
  g_armed.push_back({page, static_cast<uint32_t>(span), host, host + length - 1});
  g_active.store(true, std::memory_order_release);
}

// Re-arms whatever the last round of faults let through. Protecting a page
// that is already protected is a no-op, so this can simply redo all of them.
void RearmAll(rex::memory::Memory* memory, const std::vector<WatchRange>& ranges) {
  std::lock_guard<std::mutex> lock(g_armed_lock);
  g_armed.clear();
  for (const auto& range : ranges) {
    ArmRangeLocked(memory, range.address, range.length);
  }
}

void ReportWords(const WatchRange& range, const uint8_t* current) {
  for (uint32_t offset = 0; offset + 4 <= range.length; offset += 4) {
    uint32_t was = 0, now = 0;
    std::memcpy(&was, range.shadow.data() + offset, 4);
    std::memcpy(&now, current + offset, 4);
    if (was == now) {
      continue;
    }
    REXLOG_INFO("[GWATCH] {:#010x}+{:#06x}  {:08X} -> {:08X}", range.address, offset,
                rex::byte_swap(was), rex::byte_swap(now));
  }
  // A tail shorter than a word still matters; fall back to bytes for it.
  for (uint32_t offset = range.length & ~3u; offset < range.length; ++offset) {
    if (range.shadow[offset] != current[offset]) {
      REXLOG_INFO("[GWATCH] {:#010x}+{:#06x}  {:02X} -> {:02X}", range.address, offset,
                  range.shadow[offset], current[offset]);
    }
  }
}

void ReportBytes(const WatchRange& range, const uint8_t* current) {
  uint32_t offset = 0;
  while (offset < range.length) {
    if (range.shadow[offset] == current[offset]) {
      ++offset;
      continue;
    }
    // Coalesce a run of changed bytes into one line: a memcpy into the range
    // would otherwise produce one line per byte.
    uint32_t run = offset;
    while (run < range.length && range.shadow[run] != current[run]) {
      ++run;
    }
    std::string was, now;
    for (uint32_t i = offset; i < run && i < offset + 16; ++i) {
      was += fmt::format("{:02X}", range.shadow[i]);
      now += fmt::format("{:02X}", current[i]);
    }
    REXLOG_INFO("[GWATCH] {:#010x}+{:#06x}  {} bytes  {} -> {}", range.address, offset,
                run - offset, was, now);
    offset = run;
  }
}

void WatchMain(rex::memory::Memory* memory, std::vector<WatchRange> ranges) {
  const auto interval = std::chrono::milliseconds(REXCVAR_GET(guest_watch_ms));
  const bool words = REXCVAR_GET(guest_watch_words);
  const auto started = std::chrono::steady_clock::now();
  for (const auto& range : ranges) {
    REXLOG_INFO("[GWATCH] watching {:#010x} for {} bytes every {} ms", range.address, range.length,
                interval.count());
  }
  std::vector<uint8_t> current;
  while (true) {
    std::this_thread::sleep_for(interval);
    for (auto& range : ranges) {
      const auto* host = memory->TranslateVirtual<const uint8_t*>(range.address);
      if (!host) {
        continue;
      }
      current.assign(host, host + range.length);
      if (!range.primed) {
        // The first sample is the baseline, not a change - otherwise every
        // watch opens with a wall of "0 -> whatever it already was".
        range.shadow = current;
        range.primed = true;
        continue;
      }
      if (std::memcmp(range.shadow.data(), current.data(), range.length) == 0) {
        continue;
      }
      REXLOG_INFO("[GWATCH] {:#010x} changed at t+{:.2f}s",
                  range.address,
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
      if (words) {
        ReportWords(range, current.data());
      } else {
        ReportBytes(range, current.data());
      }
      range.shadow = current;
    }
  }
}

// Re-arming is its own loop, and a fast one. A write is let through by
// unprotecting the whole page, and protection is page-granular: any unrelated
// neighbour writing in a loop would otherwise hold the page open for a whole
// sampling interval and the write that matters would slip through unnamed.
void RearmMain(rex::memory::Memory* memory, std::vector<WatchRange> ranges) {
  const auto interval = std::chrono::microseconds(REXCVAR_GET(guest_watch_rearm_us));
  while (true) {
    std::this_thread::sleep_for(interval);
    // Unconditionally, not only after a fault of ours: the guest heap and the
    // module loader protect their own pages, and either silently undoes this
    // watch. Assuming a page is still armed because nothing faulted is exactly
    // how a watch reports nothing and looks like proof that nothing wrote.
    RearmAll(memory, ranges);
  }
}

}  // namespace

// Poll guest memory for a 32-bit value, and hand back ranges covering the first
// few places it appears. A watch is only useful once you know the address, and
// for anything the title allocates that address changes every run - so the
// address has to be discovered inside the run that watches it.
std::vector<WatchRange> FindValueRanges(rex::memory::Memory* memory, uint32_t needle) {
  std::vector<WatchRange> ranges;
  // Wait before looking: the interesting allocation usually does not exist yet
  // at start-up, and scanning early finds an unrelated match instead.
  rex::thread::Sleep(std::chrono::milliseconds(REXCVAR_GET(guest_watch_find_delay_ms)));
  const uint32_t pair = REXCVAR_GET(guest_watch_find_pair);
  for (uint32_t attempt = 0; attempt < 400 && ranges.empty(); ++attempt) {
    // Two windows: the title's own virtual allocations, and the virtual mirror
    // of physical memory, which is where anything the GPU also reads shows up.
    // Scanning every mapped address takes minutes per pass, which is longer
    // than the window in which the value is interesting. Search the narrowest
    // region that can hold it.
    const uint32_t window[2] = {REXCVAR_GET(guest_watch_find_from),
                                REXCVAR_GET(guest_watch_find_to)};
    {
    for (uint32_t address = window[0]; address < window[1] && ranges.size() < 4; address += 4) {
      // Must match how the watch arms pages (TranslateVirtual); searching the
      // physical mapping yields addresses the watch then protects elsewhere,
      // and it silently reports nothing.
      auto* word = memory->TranslateVirtual<const uint32_t*>(address);
      if (!word || rex::byte_swap(*word) != needle) {
        continue;
      }
      // An isolated match is almost always coincidence. Requiring the word
      // before it to be a known neighbour pins the actual structure.
      if (pair) {
        auto* prev = memory->TranslateVirtual<const uint32_t*>(address - 4);
        if (!prev || rex::byte_swap(*prev) != pair) {
          continue;
        }
      }
      REXLOG_INFO("[GWATCH] found {:#010x} at {:#010x}", needle, address);
      WatchRange range;
      // A value written once, before it can be found, is never caught by
      // watching its own word. Widening the window to the structure around it
      // catches the writes to its neighbours, which the same code makes.
      range.length = REXCVAR_GET(guest_watch_find_length);
      range.address = address > range.length / 2 ? address - range.length / 2 : address;
      range.shadow.resize(range.length);
      ranges.push_back(std::move(range));
    }
    }
    if (ranges.empty()) {
      rex::thread::Sleep(std::chrono::milliseconds(250));
    }
  }
  if (ranges.empty()) {
    REXLOG_WARN("[GWATCH] value {:#010x} never appeared in guest memory", needle);
  }
  return ranges;
}

void FindAndWatchMain(rex::memory::Memory* memory, uint32_t needle) {
  auto ranges = FindValueRanges(memory, needle);
  if (ranges.empty()) {
    return;
  }
  if (REXCVAR_GET(guest_watch_writers)) {
    RearmAll(memory, ranges);
    std::thread(RearmMain, memory, ranges).detach();
  }
  WatchMain(memory, std::move(ranges));
}

void StartGuestMemoryWatch(rex::memory::Memory* memory) {
  if (!memory) {
    return;
  }
  if (uint32_t needle = REXCVAR_GET(guest_watch_find)) {
    bool find_expected = false;
    if (g_started.compare_exchange_strong(find_expected, true)) {
      std::thread(FindAndWatchMain, memory, needle).detach();
    }
    return;
  }
  const std::string spec = REXCVAR_GET(guest_watch);
  if (spec.empty()) {
    return;
  }
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) {
    return;
  }
  auto ranges = ParseRanges(spec);
  if (ranges.empty()) {
    return;
  }
  if (REXCVAR_GET(guest_watch_writers)) {
    RearmAll(memory, ranges);
  }
  // Detached like the wait watchdog: a diagnostic must not introduce a
  // shutdown ordering problem of its own.
  if (REXCVAR_GET(guest_watch_writers)) {
    std::thread(RearmMain, memory, ranges).detach();
  }
  std::thread(WatchMain, memory, std::move(ranges)).detach();
}

bool GuestMemoryWatchActive() {
  return g_active.load(std::memory_order_acquire);
}

bool ReportGuestWatchFault(void* host_address, uint32_t guest_address, bool is_write,
                           uint32_t guest_lr) {
  if (!g_active.load(std::memory_order_acquire)) {
    return false;
  }
  auto* address = reinterpret_cast<uint8_t*>(host_address);
  std::lock_guard<std::mutex> lock(g_armed_lock);
  for (size_t i = 0; i < g_armed.size(); ++i) {
    const auto& armed = g_armed[i];
    if (address < armed.host_address || address >= armed.host_address + armed.length) {
      continue;
    }
    // Protection is page-granular, so neighbours on the same page fault too.
    // They are let through like any other write, but reporting them would bury
    // the range that was asked about under unrelated traffic.
    if (address >= armed.watch_first && address <= armed.watch_last) {
      REXLOG_INFO("[GWATCH] {} {:#010x} from guest lr {:#010x}", is_write ? "write to" : "read of",
                  guest_address, guest_lr);
    }
    // Let it through: the page goes writable and the instruction is retried.
    // The sampler re-arms on its next pass and reports what actually changed.
    rex::memory::Protect(armed.host_address, armed.length, rex::memory::PageAccess::kReadWrite,
                         nullptr);
    g_armed.erase(g_armed.begin() + static_cast<ptrdiff_t>(i));
    if (g_armed.empty()) {
      g_active.store(false, std::memory_order_release);
    }
    return true;
  }
  return false;
}

}  // namespace rex::system
