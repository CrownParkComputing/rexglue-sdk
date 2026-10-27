#include "arena.h"
#include "physical_write_watch.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <cstdlib>
#include <format>

#include <rex/logging.h>

namespace xn {

static uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

void PageHeap::Init(uint32_t base, uint32_t size, uint32_t page_size) {
  base_ = base;
  size_ = size;
  page_size_ = page_size;
  page_shift_ = __builtin_ctz(page_size);
  used_.assign(size / page_size, 0);
}

uint32_t PageHeap::Alloc(uint32_t size, uint32_t alignment, bool top_down, uint32_t low,
                         uint32_t high) {
  std::lock_guard lock(mutex_);
  size = RoundUp(size, page_size_);
  alignment = std::max(alignment, page_size_);
  const uint32_t pages = size >> page_shift_;
  const uint32_t align_pages = alignment >> page_shift_;
  const uint32_t total = uint32_t(used_.size());
  if (!pages || pages > total) return 0;
  uint32_t lo_page = low > base_ ? (low - base_) >> page_shift_ : 0;
  uint64_t hi_end = std::min<uint64_t>(uint64_t(high) + 1, uint64_t(base_) + size_);
  uint32_t hi_page = hi_end > base_ ? uint32_t((hi_end - base_) >> page_shift_) : 0;
  hi_page = std::min(hi_page, total);
  auto fits = [&](uint32_t start) {
    if (start + pages > hi_page) return false;
    for (uint32_t i = 0; i < pages; i++)
      if (used_[start + i]) return false;
    return true;
  };
  auto commit = [&](uint32_t start) {
    for (uint32_t i = 0; i < pages; i++) used_[start + i] = 1;
    uint32_t address = base_ + (start << page_shift_);
    allocs_[address] = size;
    return address;
  };
  if (top_down) {
    if (hi_page < pages) return 0;
    uint32_t start = (hi_page - pages) / align_pages * align_pages;
    while (start >= lo_page) {
      if (fits(start)) return commit(start);
      if (start < align_pages) break;
      start -= align_pages;
    }
  } else {
    uint32_t start = RoundUp(lo_page, align_pages);
    for (; start + pages <= hi_page; start += align_pages) {
      if (fits(start)) return commit(start);
    }
  }
  return 0;
}

bool PageHeap::AllocFixed(uint32_t address, uint32_t size) {
  std::lock_guard lock(mutex_);
  if (!Contains(address)) return false;
  size = RoundUp(size, page_size_);
  uint32_t start = (address - base_) >> page_shift_;
  uint32_t pages = size >> page_shift_;
  if (start + pages > used_.size()) return false;
  for (uint32_t i = 0; i < pages; i++) used_[start + i] = 1;
  // A fixed allocation that lands inside an existing one extends nothing; the
  // outer allocation keeps ownership (games commit inside their own reserves).
  auto it = allocs_.upper_bound(address);
  if (it != allocs_.begin()) {
    --it;
    if (address < it->first + it->second) return true;
  }
  allocs_[address] = size;
  return true;
}

bool PageHeap::Free(uint32_t address, uint32_t* out_size) {
  std::lock_guard lock(mutex_);
  auto it = allocs_.find(address);
  if (it == allocs_.end()) return false;
  uint32_t start = (address - base_) >> page_shift_;
  uint32_t pages = it->second >> page_shift_;
  for (uint32_t i = 0; i < pages; i++) used_[start + i] = 0;
  if (out_size) *out_size = it->second;
  allocs_.erase(it);
  return true;
}

uint32_t PageHeap::QueryAllocation(uint32_t address, uint32_t* out_base) const {
  std::lock_guard lock(mutex_);
  auto it = allocs_.upper_bound(address);
  if (it == allocs_.begin()) return 0;
  --it;
  if (address >= it->first + it->second) return 0;
  if (out_base) *out_base = it->first;
  return it->second;
}

bool Arena::Map(std::string& error) {
  const uint64_t file_size = kGuestSize + kPhysicalSize;
  fd_ = memfd_create("xenon-native-guest", MFD_CLOEXEC);
  if (fd_ < 0) { error = "memfd_create failed"; return false; }
  if (ftruncate(fd_, off_t(file_size)) != 0) { error = "ftruncate failed"; return false; }
  // Reserve the whole span first so every view lands where we want it.
  void* reserve = mmap(reinterpret_cast<void*>(0x100000000000ull), file_size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (reserve == MAP_FAILED) { error = "reserve mmap failed"; return false; }
  base_ = static_cast<uint8_t*>(reserve);
  struct View { uint64_t guest, size, file_offset; };
  const View views[] = {
      {0x00000000ull, 0x40000000ull, 0x00000000ull},
      {0x40000000ull, 0x3F000000ull, 0x40000000ull},
      {0x7F000000ull, 0x01000000ull, kGuestSize},           // GPU writeback aliases physical 0
      {0x80000000ull, 0x10000000ull, 0x80000000ull},
      {0x90000000ull, 0x10000000ull, 0x90000000ull},
      {0xA0000000ull, kPhysicalSize, kGuestSize},
      {0xC0000000ull, kPhysicalSize, kGuestSize},
      {0xE0000000ull, kPhysicalSize, kGuestSize + 0x1000ull},
      {kGuestSize, kPhysicalSize, kGuestSize},                 // raw physical window
  };
  for (const View& v : views) {
    void* want = base_ + v.guest;
    void* got = mmap(want, v.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd_,
                     off_t(v.file_offset));
    if (got != want) {
      error = std::format("view mmap failed at guest {:#x}", v.guest);
      return false;
    }
  }
  v00_.Init(0x00000000u, 0x40000000u, 0x1000);
  v40_.Init(0x40000000u, 0x3F000000u, 0x10000);
  v80_.Init(0x80000000u, 0x10000000u, 0x10000);
  v90_.Init(0x90000000u, 0x10000000u, 0x1000);
  physical_.Init(0, uint32_t(kPhysicalSize), 0x1000);
  // Reserve the low page (null) and the system heap.
  v00_.AllocFixed(0, 0x10000);
  // Null guest pointers must fault instead of silently modifying the bottom
  // of the arena. The GPU physical address zero has a separate mapping.
  if (mprotect(base_, 0x10000, PROT_NONE)) {
    error = "protect guest null range failed";
    return false;
  }
  v40_.AllocFixed(kSystemHeapBase, kSystemHeapSize);
  system_bump_ = kSystemHeapBase;
  // Physical page 0 stays out of the allocator: the GPU treats address 0 as "none".
  physical_.AllocFixed(0, 0x1000);
  if (const char* enabled = std::getenv("XN_PHYSICAL_WRITE_WATCH"); enabled && std::strcmp(enabled,"0"))
    EnablePhysicalWriteWatch();
  REXLOG_INFO("arena mapped at {} ({} GB guest + {} MB physical)", static_cast<void*>(base_),
              kGuestSize >> 30, kPhysicalSize >> 20);
  return true;
}

void Arena::EnablePhysicalWriteWatch() {
  const char* enabled = std::getenv("XN_PHYSICAL_WRITE_WATCH");
  if (!base_ || (enabled && !std::strcmp(enabled,"0")) ||
      PhysicalWriteWatch::Find(physical_base(),kPhysicalSize)) return;
  const PhysicalWriteWatch::Alias aliases[] = {
      {physical_base(),kPhysicalSize,0},
      {base_+0xA0000000ull,kPhysicalSize,0},
      {base_+0xC0000000ull,kPhysicalSize,0},
      {base_+0xE0000000ull,kPhysicalSize-0x1000,0x1000},
      {base_+0x7F000000ull,0x01000000,0},
  };
  // Arena mappings already live until process exit. Keep their signal
  // metadata alive for the same lifetime, including detached guest threads.
  auto* watch = new PhysicalWriteWatch(physical_base(),kPhysicalSize,aliases);
  if (!watch->Enable()) { delete watch; REXLOG_WARN("physical write watch unavailable; using hashes"); }
  else REXLOG_INFO("physical write watch enabled for all five CPU aliases");
}

PageHeap* Arena::HeapFor(uint32_t address) {
  if (address < 0x40000000u) return &v00_;
  if (address < 0x7F000000u) return &v40_;
  if (address < 0x80000000u) return nullptr;
  if (address < 0x90000000u) return &v80_;
  if (address < 0xA0000000u) return &v90_;
  return nullptr;
}
const PageHeap* Arena::HeapFor(uint32_t address) const {
  return const_cast<Arena*>(this)->HeapFor(address);
}

uint32_t Arena::AllocVirtual(uint32_t size, uint32_t page_size, bool top_down) {
  PageHeap& heap = page_size > 0x1000 ? v40_ : v00_;
  return heap.Alloc(size, page_size, top_down);
}

bool Arena::AllocVirtualFixed(uint32_t address, uint32_t size) {
  PageHeap* heap = HeapFor(address);
  return heap && heap->AllocFixed(address, size);
}

bool Arena::FreeVirtual(uint32_t address, uint32_t* out_size) {
  PageHeap* heap = HeapFor(address);
  return heap && heap->Free(address, out_size);
}

uint32_t Arena::AllocPhysical(uint32_t size, uint32_t page_size, uint32_t alignment,
                              uint32_t min_phys, uint32_t max_phys) {
  alignment = std::max(alignment, page_size);
  size = RoundUp(size, page_size);
  uint32_t phys = physical_.Alloc(size, alignment, true, min_phys, max_phys);
  if (!phys) return 0;
  if (page_size >= 16 * 1024 * 1024) return 0xC0000000u + phys;
  if (page_size >= 64 * 1024) return 0xA0000000u + phys;
  return 0xE0000000u + phys - 0x1000u;
}

bool Arena::FreePhysical(uint32_t address) {
  uint32_t phys = GetPhysicalAddress(address);
  if (phys == UINT32_MAX) return false;
  return physical_.Free(phys, nullptr);
}

uint32_t Arena::GetPhysicalAddress(uint32_t address) const {
  if (address >= 0xE0000000u) return address - 0xE0000000u + 0x1000u;
  if (address >= 0xC0000000u) return address - 0xC0000000u;
  if (address >= 0xA0000000u) return address - 0xA0000000u;
  return UINT32_MAX;
}

uint32_t Arena::QueryAllocationSize(uint32_t address) const {
  if (IsPhysicalAlias(address)) {
    uint32_t phys = GetPhysicalAddress(address);
    return physical_.QueryAllocation(phys, nullptr);
  }
  const PageHeap* heap = HeapFor(address);
  return heap ? heap->QueryAllocation(address, nullptr) : 0;
}

uint32_t Arena::SystemAlloc(uint32_t size, uint32_t alignment) {
  std::lock_guard lock(system_mutex_);
  size = RoundUp(std::max<uint32_t>(size, 16), 16);
  alignment = std::max<uint32_t>(alignment, 16);
  for (auto it = system_free_.lower_bound(size); it != system_free_.end(); ++it) {
    if (it->first == size && (it->second & (alignment - 1)) == 0) {
      uint32_t address = it->second;
      system_free_.erase(it);
      system_sizes_[address] = size;
      std::memset(Translate(address), 0, size);
      return address;
    }
  }
  uint32_t address = RoundUp(system_bump_, alignment);
  if (address + size > kSystemHeapBase + kSystemHeapSize) {
    xn::Fatal("system heap exhausted");
  }
  system_bump_ = address + size;
  system_sizes_[address] = size;
  std::memset(Translate(address), 0, size);
  return address;
}

void Arena::SystemFree(uint32_t address) {
  if (!address) return;
  std::lock_guard lock(system_mutex_);
  auto it = system_sizes_.find(address);
  if (it == system_sizes_.end()) return;
  system_free_.emplace(it->second, address);
  system_sizes_.erase(it);
}

void Arena::Fill(uint32_t address, uint32_t size, uint8_t value) {
  std::memset(Translate(address), value, size);
}

}  // namespace xn
