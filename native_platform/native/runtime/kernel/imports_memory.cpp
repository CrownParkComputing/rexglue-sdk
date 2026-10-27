// Virtual and physical memory imports.
#include <rex/hook.h>
#include <map>
#include <mutex>
#include <vector>

#include "kernel.h"

using namespace xn;

// XDK heap helper used by PGR4. Guest callers request system-pool bytes and
// release them through the existing ExFreePool import.
u32 ExAllocatePool_entry(u32 size) {
  uint32_t address = K().arena.SystemAlloc(size, 16);
  if (!address) REXKRNL_WARN("ExAllocatePool({:#x}) failed", size);
  return address;
}
XN_IMPORT(__imp__ExAllocatePool, ExAllocatePool_entry)

static uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

namespace {
struct VirtualRegion {
  uint32_t size, allocation_protect;
  // Zero means reserved; otherwise this is the committed page's protection.
  std::vector<uint32_t> pages;
};
std::mutex virtual_mutex;
std::map<uint32_t, VirtualRegion> virtual_regions;
auto FindRegion(uint32_t address) {
  auto it = virtual_regions.upper_bound(address);
  if (it == virtual_regions.begin()) return virtual_regions.end();
  --it;
  return uint64_t(address) < uint64_t(it->first) + it->second.size ? it : virtual_regions.end();
}
}

u32 NtAllocateVirtualMemory_entry(mapped_u32 base_addr_ptr, mapped_u32 region_size_ptr,
                                  u32 alloc_type, u32 protect_bits, u32 debug_memory) {
  if (!base_addr_ptr || !region_size_ptr || !*region_size_ptr) return X_STATUS_INVALID_PARAMETER;
  if (!(alloc_type & (X_MEM_COMMIT | X_MEM_RESET | X_MEM_RESERVE))) return X_STATUS_INVALID_PARAMETER;
  Arena& arena = K().arena;
  uint32_t base = *base_addr_ptr;
  uint32_t page_size = (alloc_type & X_MEM_LARGE_PAGES) ? 0x10000 : 0x1000;
  if (base) page_size = base >= 0x40000000u ? 0x10000 : 0x1000;
  uint32_t size = uint32_t(*region_size_ptr);
  if (int32_t(size) < 0) size = uint32_t(-int32_t(size));
  uint32_t adjusted_base = base - (base % page_size);
  uint32_t adjusted_size = RoundUp(size, adjusted_base ? page_size : 0x10000);
  if (!adjusted_size || uint64_t(adjusted_base) + adjusted_size > 0x100000000ull)
    return X_STATUS_INVALID_PARAMETER;
  std::lock_guard guard(virtual_mutex);
  auto region = adjusted_base ? FindRegion(adjusted_base) : virtual_regions.end();
  if (region != virtual_regions.end() &&
      uint64_t(adjusted_base) + adjusted_size > uint64_t(region->first) + region->second.size)
    return X_STATUS_NO_MEMORY;
  uint32_t address = 0;
  if (adjusted_base) {
    if (arena.AllocVirtualFixed(adjusted_base, adjusted_size)) address = adjusted_base;
  } else {
    address = arena.AllocVirtual(adjusted_size, page_size, (alloc_type & X_MEM_TOP_DOWN) != 0);
  }
  if (!address) {
    // Name the caller and both in/out slots: a nonsense size is almost always
    // the guest handing us the wrong variable, and the two guest pointers say
    // so immediately (equal pointers, or a size slot holding an address).
    PPCContext* ctx = CurrentContext();
    GuestThread* t = CurrentThread();
    REXKRNL_ERROR("NtAllocateVirtualMemory: out of memory. base={:#x}@{:#010x} size={:#x}@{:#010x} "
                  "adjusted={:#x} type={:#x} protect={:#x} lr={:#010x} tid={:#x}",
                  base, base_addr_ptr.guest_address(), size, region_size_ptr.guest_address(),
                  adjusted_size, alloc_type, protect_bits, ctx ? uint32_t(ctx->lr) : 0,
                  t ? t->thread_id() : 0);
    return X_STATUS_NO_MEMORY;
  }
  if (region == virtual_regions.end()) {
    region = virtual_regions.emplace(address, VirtualRegion{adjusted_size, protect_bits,
        std::vector<uint32_t>(adjusted_size / 0x1000, 0)}).first;
  }
  if (alloc_type & X_MEM_COMMIT) {
    const auto first = (address - region->first) / 0x1000;
    for (uint32_t page = first; page < first + adjusted_size / 0x1000; ++page) {
      // A commit inside a reservation is new memory too. Recommitting after
      // decommit must zero old heap headers; a repeated commit must preserve
      // live bytes. Reservation ownership alone cannot answer this question.
      if (!region->second.pages[page] && !(alloc_type & X_MEM_NOZERO))
        arena.Fill(region->first + page * 0x1000, 0x1000, 0);
      region->second.pages[page] = protect_bits ? uint32_t(protect_bits) : X_PAGE_READWRITE;
    }
  }
  *base_addr_ptr = address;
  *region_size_ptr = adjusted_size;
  REXKRNL_DEBUG("NtAllocateVirtualMemory base={:#x} size={:#x} type={:#x} -> {:#010x}+{:#x}", base,
                size, alloc_type, address, adjusted_size);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtAllocateVirtualMemory, NtAllocateVirtualMemory_entry)

u32 NtFreeVirtualMemory_entry(mapped_u32 base_addr_ptr, mapped_u32 region_size_ptr, u32 free_type,
                              u32 debug_memory) {
  if (!base_addr_ptr || !region_size_ptr) return X_STATUS_INVALID_PARAMETER;
  uint32_t base = *base_addr_ptr;
  if (!base) return X_STATUS_MEMORY_NOT_ALLOCATED;
  std::lock_guard guard(virtual_mutex);
  auto region = FindRegion(base);
  uint32_t size = 0;
  if (free_type == X_MEM_DECOMMIT) {
    if (region == virtual_regions.end()) return X_STATUS_MEMORY_NOT_ALLOCATED;
    const auto first = (base - region->first) / 0x1000;
    size = *region_size_ptr ? RoundUp(uint32_t(*region_size_ptr), 0x1000) : region->second.size;
    if (!size || uint64_t(first) + size / 0x1000 > region->second.pages.size())
      return X_STATUS_INVALID_PARAMETER;
    std::fill_n(region->second.pages.begin() + first, size / 0x1000, 0);
    *base_addr_ptr = region->first + first * 0x1000;
    *region_size_ptr = size;
    return X_STATUS_SUCCESS;
  }
  if (free_type != X_MEM_RELEASE || (region != virtual_regions.end() && region->first != base))
    return X_STATUS_INVALID_PARAMETER;
  if (!K().arena.FreeVirtual(base, &size)) return X_STATUS_UNSUCCESSFUL;
  if (region != virtual_regions.end()) virtual_regions.erase(region);
  *region_size_ptr = size;
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtFreeVirtualMemory, NtFreeVirtualMemory_entry)

u32 NtQueryVirtualMemory_entry(u32 base_address, u32 info_ptr) {
  if (!info_ptr) return X_STATUS_INVALID_PARAMETER;
  std::lock_guard guard(virtual_mutex);
  auto region = FindRegion(base_address);
  if (region != virtual_regions.end()) {
    const auto page = (base_address - region->first) / 0x1000;
    const auto protect = region->second.pages[page];
    auto end = page + 1;
    while (end < region->second.pages.size() && region->second.pages[end] == protect) ++end;
    Store32(info_ptr, base_address & ~0xFFFu);
    Store32(info_ptr + 4, region->first);
    Store32(info_ptr + 8, region->second.allocation_protect);
    Store32(info_ptr + 12, (end - page) * 0x1000);
    Store32(info_ptr + 16, protect ? X_MEM_COMMIT : X_MEM_RESERVE);
    Store32(info_ptr + 20, protect);
    Store32(info_ptr + 24, 0x20000);
    return X_STATUS_SUCCESS;
  }
  // MEMORY_BASIC_INFORMATION: base, allocation base, alloc protect, region size, state, protect, type
  Arena& arena = K().arena;
  uint32_t size = arena.QueryAllocationSize(base_address);
  REXKRNL_DEBUG("NtQueryVirtualMemory address={:#x} allocation_size={:#x} lr={:#x}",
               base_address, size, CurrentContext() ? uint32_t(CurrentContext()->lr) : 0);
  Store32(info_ptr + 0x00, base_address & ~0xFFFu);
  Store32(info_ptr + 0x04, base_address & ~0xFFFu);
  Store32(info_ptr + 0x08, X_PAGE_READWRITE);
  Store32(info_ptr + 0x0C, size ? size : 0x1000);
  Store32(info_ptr + 0x10, size ? X_MEM_COMMIT : 0x10000 /* MEM_FREE */);
  Store32(info_ptr + 0x14, size ? X_PAGE_READWRITE : 0);
  Store32(info_ptr + 0x18, 0x20000);  // MEM_PRIVATE
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtQueryVirtualMemory, NtQueryVirtualMemory_entry)

u32 MmAllocatePhysicalMemoryEx_entry(u32 flags, u32 region_size, u32 protect_bits,
                                     u32 min_addr_range, u32 max_addr_range, u32 alignment) {
  if (!(protect_bits & (X_PAGE_READONLY | X_PAGE_READWRITE))) return 0;
  uint32_t page_size = 0x1000;
  if (protect_bits & X_MEM_LARGE_PAGES) page_size = 0x10000;
  else if (protect_bits & X_MEM_16MB_PAGES) page_size = 16 * 1024 * 1024;
  uint32_t address = K().arena.AllocPhysical(region_size, page_size, alignment, min_addr_range,
                                             max_addr_range);
  REXKRNL_DEBUG("MmAllocatePhysicalMemoryEx size={:#x} protect={:#x} align={:#x} -> {:#010x}",
                region_size, protect_bits, alignment, address);
  if (!address) REXKRNL_ERROR("MmAllocatePhysicalMemoryEx: out of physical memory ({:#x})", region_size);
  return address;
}
XN_IMPORT(__imp__MmAllocatePhysicalMemoryEx, MmAllocatePhysicalMemoryEx_entry)

void MmFreePhysicalMemory_entry(u32 type, u32 base_address) { K().arena.FreePhysical(base_address); }
XN_IMPORT(__imp__MmFreePhysicalMemory, MmFreePhysicalMemory_entry)

u32 MmGetPhysicalAddress_entry(u32 base_address) {
  uint32_t p = K().arena.GetPhysicalAddress(base_address);
  return p == UINT32_MAX ? 0 : p;
}
XN_IMPORT(__imp__MmGetPhysicalAddress, MmGetPhysicalAddress_entry)

void MmSetAddressProtect_entry(u32 base_address, u32 size, u32 flags) {
  // The arena starts read/write. Reapplying that permission must preserve
  // physical write-watch traps: mprotect(RW) here would silently disarm them.
  // Guest execute permission concerns PPC data; the host executes compiled C++.
  const uint32_t access = flags & 0xFF;
  if (access == 0x04 || access == 0x40) return;
  Fatal(std::format("Native memory protection {:#x} at {:#010x}+{:#x} is not implemented",
                    flags, base_address, size));
}
XN_IMPORT(__imp__MmSetAddressProtect, MmSetAddressProtect_entry)

u32 MmQueryAddressProtect_entry(u32 base_address) {
  return K().arena.QueryAllocationSize(base_address) ? X_PAGE_READWRITE : 0;
}
XN_IMPORT(__imp__MmQueryAddressProtect, MmQueryAddressProtect_entry)

u32 MmQueryAllocationSize_entry(u32 base_address) {
  return K().arena.QueryAllocationSize(base_address);
}
XN_IMPORT(__imp__MmQueryAllocationSize, MmQueryAllocationSize_entry)

u32 KeLockL2_entry() { return 0; }
XN_IMPORT(__imp__KeLockL2, KeLockL2_entry)
void KeUnlockL2_entry() {}
XN_IMPORT(__imp__KeUnlockL2, KeUnlockL2_entry)

// MmQueryStatistics(stats*). The caller fills in the structure's own Length
// first and the kernel refuses a size it does not recognise, so this checks it
// the way the real one does. Boom Boom Rocket reaches it through xapilib's
// GlobalMemoryStatus (0x820E8030), which passes Length = 0x68 and then reads
// four fields to build a Win32 MEMORYSTATUS:
//
//   dwTotalPhys  = stats[0x04] << 12      (total physical pages)
//   dwAvailPhys  = stats[0x0C] << 12      (available pages)
//   dwTotalVirtual = stats[0x10]
//   dwAvailVirtual = stats[0x10] - stats[0x14]
//
// so the two page counts must be pages and the two virtual figures bytes, or a
// title that budgets against them budgets against nonsense. The console this
// title shipped on has 512 MB, of which a title may address 256 MB.
u32 MmQueryStatistics_entry(mapped_void stats_ptr) {
  constexpr uint32_t kStatisticsBytes = 0x68;
  if (!stats_ptr) return X_STATUS_INVALID_PARAMETER;
  const uint32_t base = stats_ptr.guest_address();
  const uint32_t length = Load32(base);
  if (length != kStatisticsBytes) {
    REXKRNL_WARN("MmQueryStatistics: caller asked for {} bytes, we answer {}", length,
                 kStatisticsBytes);
    // Answering means writing the buffer even when refusing: leave the caller
    // a zeroed block with the size we do support rather than stale memory.
    for (uint32_t o = 4; o < kStatisticsBytes; o += 4) Store32(base + o, 0);
    Store32(base, kStatisticsBytes);
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }
  constexpr uint32_t kPhysicalPages = 512u * 1024 * 1024 / 4096;   // 131072
  constexpr uint32_t kKernelPages = 16u * 1024 * 1024 / 4096;      // 4096
  constexpr uint32_t kAvailablePages = kPhysicalPages - kKernelPages;
  constexpr uint32_t kTotalVirtual = 256u * 1024 * 1024;
  // We do not track committed virtual bytes, so this field stays 0 rather than
  // carrying a guess: the caller subtracts it, so 0 means "all of it free".
  // That errs towards a title trying an allocation and finding out, instead of
  // refusing up front on a number we invented.
  constexpr uint32_t kUsedVirtual = 0;
  for (uint32_t o = 0; o < kStatisticsBytes; o += 4) Store32(base + o, 0);
  Store32(base + 0x00, kStatisticsBytes);
  Store32(base + 0x04, kPhysicalPages);
  Store32(base + 0x08, kKernelPages);
  Store32(base + 0x0C, kAvailablePages);
  Store32(base + 0x10, kTotalVirtual);
  Store32(base + 0x14, kUsedVirtual);  // the caller subtracts this from the total
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__MmQueryStatistics, MmQueryStatistics_entry)
