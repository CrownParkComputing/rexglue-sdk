// xenon-native: the 4 GB guest address space and its allocators.
//
// One memfd backs everything. The guest virtual ranges are MAP_FIXED views of
// it, so the three physical aliases (0xA0000000 / 0xC0000000 / 0xE0000000)
// and the raw physical window all resolve to the same bytes - the technique
// Xenia/ReXGlue use, written here from scratch. The 0xE0000000 alias is offset
// by 0x1000 like the console (physical p is visible at 0xE0000000 + p - 0x1000).
//
// Layout (guest):
//   0x00000000-0x3FFFFFFF  virtual, 4 KB pages
//   0x40000000-0x7EFFFFFF  virtual, 64 KB pages   (0x7E000000+ reserved: system heap)
//   0x7F000000-0x7FFFFFFF  GPU writeback / MMIO   (memory aliases physical 0)
//   0x80000000-0x8FFFFFFF  XEX image, 64 KB pages
//   0x90000000-0x9FFFFFFF  XEX image, 4 KB pages
//   0xA0000000-0xBFFFFFFF  physical alias, 64 KB pages
//   0xC0000000-0xDFFFFFFF  physical alias, 16 MB pages
//   0xE0000000-0xFFFFFFFF  physical alias, 4 KB pages (+0x1000 skew)
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace xn {

class PageHeap {
 public:
  void Init(uint32_t base, uint32_t size, uint32_t page_size);
  // Returns 0 on failure. size/alignment are rounded up to the page size.
  uint32_t Alloc(uint32_t size, uint32_t alignment, bool top_down, uint32_t low = 0,
                 uint32_t high = 0xFFFFFFFFu);
  bool AllocFixed(uint32_t address, uint32_t size);
  bool Free(uint32_t address, uint32_t* out_size);
  bool Contains(uint32_t address) const { return address >= base_ && address - base_ < size_; }
  uint32_t QueryAllocation(uint32_t address, uint32_t* out_base) const;  // size or 0
  uint32_t base() const { return base_; }
  uint32_t size() const { return size_; }
  uint32_t page_size() const { return page_size_; }

 private:
  uint32_t base_ = 0, size_ = 0, page_size_ = 0, page_shift_ = 0;
  std::vector<uint8_t> used_;              // per page
  std::map<uint32_t, uint32_t> allocs_;    // address -> size
  mutable std::mutex mutex_;
};

class Arena {
 public:
  static constexpr uint64_t kGuestSize = 0x100000000ull;
  static constexpr uint64_t kPhysicalSize = 0x20000000ull;  // 512 MB
  static constexpr uint32_t kSystemHeapBase = 0x7E000000u;
  static constexpr uint32_t kSystemHeapSize = 0x01000000u;

  bool Map(std::string& error);
  void EnablePhysicalWriteWatch();
  uint8_t* base() const { return base_; }
  uint8_t* physical_base() const { return base_ + kGuestSize; }
  uint8_t* Translate(uint32_t guest) const { return base_ + guest; }
  uint8_t* TranslatePhysical(uint32_t phys) const { return physical_base() + (phys & 0x1FFFFFFFu); }

  // Virtual allocations (NtAllocateVirtualMemory, stacks, ...).
  uint32_t AllocVirtual(uint32_t size, uint32_t page_size, bool top_down);
  bool AllocVirtualFixed(uint32_t address, uint32_t size);
  bool FreeVirtual(uint32_t address, uint32_t* out_size);
  // Physical allocations (MmAllocatePhysicalMemoryEx): returns the virtual alias
  // for the requested page size. min/max bound the physical address.
  uint32_t AllocPhysical(uint32_t size, uint32_t page_size, uint32_t alignment, uint32_t min_phys,
                         uint32_t max_phys);
  bool FreePhysical(uint32_t address);
  // UINT32_MAX when the address is not in a physical alias.
  uint32_t GetPhysicalAddress(uint32_t address) const;
  // Size of the allocation containing address (any heap), 0 if unknown.
  uint32_t QueryAllocationSize(uint32_t address) const;
  bool IsPhysicalAlias(uint32_t address) const { return address >= 0xA0000000u; }

  // Kernel-side objects that must live in guest memory (KPCR, TLS, events...).
  uint32_t SystemAlloc(uint32_t size, uint32_t alignment = 16);
  void SystemFree(uint32_t address);

  void Fill(uint32_t address, uint32_t size, uint8_t value);

 private:
  PageHeap* HeapFor(uint32_t address);
  const PageHeap* HeapFor(uint32_t address) const;

  uint8_t* base_ = nullptr;
  int fd_ = -1;
  PageHeap v00_, v40_, v80_, v90_, physical_;
  std::mutex system_mutex_;
  uint32_t system_bump_ = 0;
  std::multimap<uint32_t, uint32_t> system_free_;  // size -> address
  std::map<uint32_t, uint32_t> system_sizes_;     // address -> size
};

}  // namespace xn
