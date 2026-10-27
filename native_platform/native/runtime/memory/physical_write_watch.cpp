#include "physical_write_watch.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <limits>
#include <new>
#include <sys/mman.h>
#include <ucontext.h>

namespace xn {
namespace {
std::atomic<PhysicalWriteWatch*> active{nullptr};
static_assert(std::atomic<PhysicalWriteWatch*>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
inline void Pause() { __builtin_ia32_pause(); }
} // namespace

PhysicalWriteWatch::PhysicalWriteWatch(uint8_t* base, size_t size, std::span<const Alias> aliases)
    : physical_base_(base), size_(size), page_count_(size/kPageSize),
      aliases_(aliases.begin(),aliases.end()), pages_(new Page[page_count_]) {}

bool PhysicalWriteWatch::Enable() {
  if (enabled_) return true;
  if (!size_ || size_%kPageSize || uintptr_t(physical_base_)%kPageSize || aliases_.empty()) return false;
  for (const auto& alias : aliases_)
    if (uintptr_t(alias.base)%kPageSize || alias.size%kPageSize ||
        alias.physical_offset%kPageSize || alias.physical_offset > size_ ||
        alias.size > size_-alias.physical_offset) return false;
  PhysicalWriteWatch* empty = nullptr;
  if (!active.compare_exchange_strong(empty,this)) return false;
  struct sigaction action{};
  action.sa_sigaction = Signal;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGSEGV,&action,&previous_) != 0) {
    active.store(nullptr);
    return false;
  }
  enabled_ = true;
  return true;
}

PhysicalWriteWatch::~PhysicalWriteWatch() {
  EndTrace();
  if (!enabled_) return;
  for (const auto& alias : aliases_) mprotect(alias.base,alias.size,PROT_READ|PROT_WRITE);
  active.store(nullptr);
  sigaction(SIGSEGV,&previous_,nullptr);
}

PhysicalWriteWatch* PhysicalWriteWatch::Find(uint8_t* base, size_t size) {
  auto* watch = active.load(std::memory_order_acquire);
  return watch && watch->physical_base_ == base && watch->size_ == size ? watch : nullptr;
}

bool PhysicalWriteWatch::BeginTrace() {
  if (!enabled_ || trace_enabled_.load(std::memory_order_acquire) ||
      page_count_ > uint64_t(std::numeric_limits<uint32_t>::max())+1) return false;
  if (!trace_dirty_) {
    const size_t dirty_words = (page_count_+63)/64;
    const size_t summary_words = (dirty_words+63)/64;
    auto dirty = std::unique_ptr<std::atomic<uint64_t>[]>(
        new (std::nothrow) std::atomic<uint64_t>[dirty_words]);
    auto summary = std::unique_ptr<std::atomic<uint64_t>[]>(
        new (std::nothrow) std::atomic<uint64_t>[summary_words]);
    if (!dirty || !summary) return false;
    trace_dirty_ = std::move(dirty);
    trace_summary_ = std::move(summary);
    trace_dirty_words_ = dirty_words;
    trace_summary_words_ = summary_words;
  }
  for (size_t word=0; word<trace_dirty_words_; ++word)
    trace_dirty_[word].store(0,std::memory_order_relaxed);
  for (size_t word=0; word<trace_summary_words_; ++word)
    trace_summary_[word].store(0,std::memory_order_relaxed);
  trace_enabled_.store(true,std::memory_order_release);
  // Initial work is bounded by physical memory size. Subsequent takes scan
  // only the small summary and dirty words, never the full page array.
  for (size_t page=0; page<page_count_; ++page) MarkTraceDirty(page);
  return true;
}

void PhysicalWriteWatch::EndTrace() {
  trace_enabled_.store(false,std::memory_order_release);
}

void PhysicalWriteWatch::MarkTraceDirty(size_t page) {
  if (!trace_enabled_.load(std::memory_order_acquire)) return;
  const size_t word = page/64;
  // Publish the page before its summary bit. Taking the summary followed by
  // exchanging the dirty word cannot lose a racing publication: it is either
  // consumed now or leaves a summary bit for the next take (occasionally empty).
  trace_dirty_[word].fetch_or(uint64_t{1}<<(page%64),std::memory_order_release);
  trace_summary_[word/64].fetch_or(uint64_t{1}<<(word%64),std::memory_order_release);
}

void PhysicalWriteWatch::TakeTraceDirtyPages(std::vector<uint32_t>& pages) {
  pages.clear();
  if (!trace_enabled_.load(std::memory_order_acquire)) return;
  for (size_t summary_word=0; summary_word<trace_summary_words_; ++summary_word) {
    uint64_t words = trace_summary_[summary_word].exchange(0,std::memory_order_acq_rel);
    while (words) {
      const size_t word = summary_word*64+std::countr_zero(words);
      words &= words-1;
      uint64_t dirty = trace_dirty_[word].exchange(0,std::memory_order_acq_rel);
      while (dirty) {
        const size_t page = word*64+std::countr_zero(dirty);
        dirty &= dirty-1;
        pages.push_back(uint32_t(page));
        const auto& p = pages_[page];
        const uint32_t state = p.state.load(std::memory_order_acquire);
        if (p.pins.load(std::memory_order_acquire) || state == kDisabled ||
            state == kArming || state == kDisarming) MarkTraceDirty(page);
      }
    }
  }
}

bool PhysicalWriteWatch::Protect(size_t page, int protection) {
  const size_t offset = page*kPageSize;
  bool ok = true;
  for (const auto& alias : aliases_)
    if (offset >= alias.physical_offset && offset-alias.physical_offset < alias.size)
      ok &= mprotect(alias.base+offset-alias.physical_offset,kPageSize,protection) == 0;
  return ok;
}

uint64_t PhysicalWriteWatch::Token(size_t page) {
  if (!enabled_ || page >= page_count_) return 0;
  auto& p = pages_[page];
  if (p.pins.load(std::memory_order_acquire)) {
    MarkTraceDirty(page);
    return 0;
  }
  uint32_t state = p.state.load(std::memory_order_acquire);
  if (state == kDirty && p.state.compare_exchange_strong(state,kArming,std::memory_order_acq_rel)) {
    // Publish the epoch before protection: a writer fault may already be
    // waiting for the arming sequence to finish on another alias.
    p.generation.fetch_add(1,std::memory_order_relaxed);
    if (!Protect(page,PROT_READ)) {
      Protect(page,PROT_READ|PROT_WRITE);
      p.state.store(kDisabled,std::memory_order_release);
      MarkTraceDirty(page);
      return 0;
    }
    p.state.store(kWatched,std::memory_order_release);
  }
  if (p.pins.load(std::memory_order_acquire)) {
    Invalidate(page);
    return 0;
  }
  // A zero result asks the caller to use ordinary content validation. In
  // particular, never spin or trust a token while another thread disarms it.
  if (p.state.load(std::memory_order_acquire) != kWatched) {
    MarkTraceDirty(page);
    return 0;
  }
  return p.generation.load(std::memory_order_acquire);
}

bool PhysicalWriteWatch::Invalidate(size_t page) {
  auto& p = pages_[page];
  for (;;) {
    uint32_t state = p.state.load(std::memory_order_acquire);
    if (state == kDirty) { MarkTraceDirty(page); return true; }
    if (state == kDisabled) { MarkTraceDirty(page); return false; }
    if (state != kWatched) { Pause(); continue; }
    if (!p.state.compare_exchange_weak(state,kDisarming,std::memory_order_acq_rel)) continue;
    const bool ok = Protect(page,PROT_READ|PROT_WRITE);
    p.state.store(ok ? kDirty : kDisabled,std::memory_order_release);
    // Publish only after making the page writable. Publishing solely before
    // disarming lets a consumer take/rearm/copy and then miss the pending store.
    MarkTraceDirty(page);
    return ok;
  }
}

bool PhysicalWriteWatch::HandleFault(void* address) {
  const uintptr_t at = uintptr_t(address);
  for (const auto& alias : aliases_) {
    if (at < uintptr_t(alias.base) || at-uintptr_t(alias.base) >= alias.size) continue;
    const size_t page = (at-uintptr_t(alias.base)+alias.physical_offset)/kPageSize;
    // A fault may have been queued before a different writer disarmed the
    // page. Retrying is valid even when it is already writable now.
    if (!pages_[page].generation.load(std::memory_order_acquire)) return false;
    faults_.fetch_add(1,std::memory_order_relaxed);
    return Invalidate(page);
  }
  return false;
}

void PhysicalWriteWatch::Signal(int signal, siginfo_t* info, void* context) {
  const int saved_errno = errno;
  auto* watch = active.load(std::memory_order_acquire);
  const auto* uc = static_cast<ucontext_t*>(context);
  const bool write = uc->uc_mcontext.gregs[REG_ERR] & 2;
  if (watch && info->si_code == SEGV_ACCERR && write && watch->HandleFault(info->si_addr)) {
    errno = saved_errno;
    return;
  }
  errno = saved_errno;
  if (watch) {
    const auto& old = watch->previous_;
    if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
      if (old.sa_flags & SA_SIGINFO) old.sa_sigaction(signal,info,context);
      else old.sa_handler(signal);
      return;
    }
    if (old.sa_handler != SIG_IGN) { sigaction(signal,&old,nullptr); return; }
  }
  struct sigaction action{};
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  sigaction(signal,&action,nullptr);
  // Returning retries the faulting instruction with the previous disposition.
}

void PhysicalWriteWatch::ChangePins(void* address, size_t size, bool pin) {
  const uintptr_t first = uintptr_t(address);
  const uintptr_t last = size > std::numeric_limits<uintptr_t>::max()-first ?
      std::numeric_limits<uintptr_t>::max() : first+size;
  for (const auto& alias : aliases_) {
    const uintptr_t lo = std::max(first,uintptr_t(alias.base));
    const uintptr_t hi = std::min(last,uintptr_t(alias.base)+alias.size);
    if (lo >= hi) continue;
    const size_t begin = (lo-uintptr_t(alias.base)+alias.physical_offset)/kPageSize;
    const size_t end = (hi-uintptr_t(alias.base)+alias.physical_offset+kPageSize-1)/kPageSize;
    for (size_t page=begin; page<end; ++page) {
      if (pin) {
        pages_[page].pins.fetch_add(1,std::memory_order_acq_rel);
        Invalidate(page);
      } else {
        pages_[page].pins.fetch_sub(1,std::memory_order_release);
        // A take during the syscall may have observed only part of its output.
        // Publish its completion even when no protection fault was possible.
        MarkTraceDirty(page);
      }
    }
  }
}

void PhysicalWriteWatch::Pin(void* address, size_t size) { ChangePins(address,size,true); }
void PhysicalWriteWatch::Unpin(void* address, size_t size) { ChangePins(address,size,false); }
GuestWriteScope::GuestWriteScope(void* address, size_t size)
    : watch_(active.load(std::memory_order_acquire)), address_(address), size_(size) {
  if (watch_) watch_->Pin(address_,size_);
}
GuestWriteScope::~GuestWriteScope() { if (watch_) watch_->Unpin(address_,size_); }
} // namespace xn
