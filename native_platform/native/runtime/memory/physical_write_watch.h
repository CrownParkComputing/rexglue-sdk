#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <signal.h>

namespace xn {

// Linux write protection for every CPU alias of a physical allocation. A
// nonzero token proves that no CPU write occurred since that page was armed.
// Callers still snapshot/hash the page on the first observation of a token.
class PhysicalWriteWatch {
 public:
  static constexpr size_t kPageSize = 4096;
  struct Alias { uint8_t* base; size_t size; size_t physical_offset; };
  PhysicalWriteWatch(uint8_t* physical_base, size_t size, std::span<const Alias> aliases);
  ~PhysicalWriteWatch(); // all users must have stopped before destruction
  bool Enable();
  uint64_t Token(size_t page); // calls are serialized by the renderer
  uint64_t fault_count() const { return faults_.load(std::memory_order_relaxed); }
  static PhysicalWriteWatch* Find(uint8_t* physical_base, size_t size);

  // Optional, single-consumer dirty-page subscription. These three calls and
  // Token are serialized by the renderer; CPU writers may run concurrently.
  // BeginTrace requires Enable and fails if a trace is already active. Its
  // first take includes every physical page, so the caller can capture a base.
  bool BeginTrace();
  void EndTrace();
  // Replaces pages with unique physical page indices dirtied since the prior
  // take. This neither copies memory nor rearms protection: call Token before
  // snapshotting each page. An unarmed, writable page cannot report subsequent
  // ordinary stores. Pinned, disabled, or transitioning pages stay pending
  // until a stable token can be obtained. A failed Token also queues
  // another observation. Delivery of a concurrent write may be in this take
  // or the next; this is not a CPU/kernel write-completion barrier. Normal
  // guest buffer ownership/fences still apply when reading the page contents.
  void TakeTraceDirtyPages(std::vector<uint32_t>& pages);

  // Kernel read(2) writes do not deliver a userspace SIGSEGV. Keep these
  // ranges writable for the entire syscall, including buffered file reads.
  void Pin(void* address, size_t size);
  void Unpin(void* address, size_t size);

 private:
  enum : uint32_t { kDirty, kArming, kWatched, kDisarming, kDisabled };
  struct Page {
    std::atomic<uint32_t> state{kDirty}, pins{0};
    std::atomic<uint64_t> generation{0};
  };
  bool Protect(size_t page, int protection);
  bool Invalidate(size_t page);
  bool HandleFault(void* address);
  static void Signal(int signal, siginfo_t* info, void* context);
  void ChangePins(void* address, size_t size, bool pin);
  void MarkTraceDirty(size_t page);
  uint8_t* physical_base_;
  size_t size_, page_count_;
  std::vector<Alias> aliases_;
  std::unique_ptr<Page[]> pages_;
  std::atomic<uint64_t> faults_{0};
  // Allocated only on first BeginTrace and kept until destruction: a signal
  // handler which observed enabled=true must remain safe across EndTrace.
  std::unique_ptr<std::atomic<uint64_t>[]> trace_dirty_, trace_summary_;
  size_t trace_dirty_words_ = 0, trace_summary_words_ = 0;
  std::atomic<bool> trace_enabled_{false};
  struct sigaction previous_{};
  bool enabled_ = false;
};

class GuestWriteScope {
 public:
  GuestWriteScope(void* address, size_t size);
  ~GuestWriteScope();
  GuestWriteScope(const GuestWriteScope&) = delete;
  GuestWriteScope& operator=(const GuestWriteScope&) = delete;
 private:
  PhysicalWriteWatch* watch_;
  void* address_;
  size_t size_;
};

} // namespace xn
