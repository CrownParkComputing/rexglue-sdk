#include "runtime/memory/physical_write_watch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
using Watch = xn::PhysicalWriteWatch;
constexpr size_t kPage = Watch::kPageSize;

void Check(bool ok, const char* expression, int line) {
  if (!ok) throw std::runtime_error("line "+std::to_string(line)+": "+expression);
}
#define CHECK(expression) Check(bool(expression),#expression,__LINE__)

struct Mapping {
  explicit Mapping(size_t pages) : size(pages*kPage) {
    fd = int(syscall(SYS_memfd_create,"write-watch-trace-probe",0));
    CHECK(fd >= 0);
    CHECK(ftruncate(fd,off_t(size)) == 0);
    for (size_t i=0; i<bases.size(); ++i) {
      const size_t offset = i == 2 ? kPage : 0;
      bases[i] = static_cast<uint8_t*>(mmap(nullptr,size-offset,PROT_READ|PROT_WRITE,
                                          MAP_SHARED,fd,off_t(offset)));
      CHECK(bases[i] != MAP_FAILED);
      aliases.push_back({bases[i],size-offset,offset});
    }
  }
  ~Mapping() {
    for (const auto& alias : aliases) munmap(alias.base,alias.size);
    if (fd >= 0) close(fd);
  }
  uint8_t* At(size_t alias, size_t page) const {
    return bases[alias]+page*kPage-(alias == 2 ? kPage : 0);
  }
  int fd = -1;
  size_t size;
  std::array<uint8_t*,3> bases{};
  std::vector<Watch::Alias> aliases;
};

bool Contains(const std::vector<uint32_t>& pages, size_t page) {
  return std::find(pages.begin(),pages.end(),page) != pages.end();
}

void CheckUnique(const std::vector<uint32_t>& pages) {
  auto sorted = pages;
  std::sort(sorted.begin(),sorted.end());
  CHECK(std::adjacent_find(sorted.begin(),sorted.end()) == sorted.end());
}

void ArmAll(Watch& watch, size_t count) {
  for (size_t page=0; page<count; ++page) CHECK(watch.Token(page) != 0);
}

void BeginAndArm(Watch& watch, size_t count) {
  CHECK(watch.BeginTrace());
  std::vector<uint32_t> pages;
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages.size() == count);
  for (size_t page=0; page<count; ++page) CHECK(pages[page] == page);
  ArmAll(watch,count);
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages.empty());
}

// Real shared mappings ensure protections/faults on every CPU alias refer to
// the same physical page; an offset alias also exercises physical translation.
void AliasesAndLifecycle() {
  constexpr size_t count = 140;
  Mapping memory(count);
  {
    Watch watch(memory.bases[0],memory.size,memory.aliases);
    CHECK(!watch.BeginTrace());
    CHECK(watch.Enable());
    CHECK(Watch::Find(memory.bases[0],memory.size) == &watch);
    std::vector<uint32_t> pages{99};
    watch.TakeTraceDirtyPages(pages);
    CHECK(pages.empty());
    const auto before = watch.Token(1);
    CHECK(before);
    *memory.At(1,1) = 3;
    CHECK(watch.Token(1) > before);
    CHECK(watch.fault_count() == 1);
    watch.TakeTraceDirtyPages(pages);
    CHECK(pages.empty());

    BeginAndArm(watch,count);
    CHECK(!watch.BeginTrace());
    for (size_t alias=0; alias<memory.bases.size(); ++alias) {
      const size_t page = 63+alias;
      const auto token = watch.Token(page);
      *memory.At(alias,page) = uint8_t(10+alias);
      CHECK(*memory.At(0,page) == 10+alias);
      watch.TakeTraceDirtyPages(pages);
      CHECK(pages.size() == 1 && pages[0] == page);
      CHECK(watch.Token(page) > token);
      watch.TakeTraceDirtyPages(pages);
      CHECK(pages.empty());
    }
    // Many writes coalesce, while separate bitmap words retain their bits.
    for (size_t page : {size_t(0),size_t(63),size_t(64),size_t(139)}) {
      *memory.At(0,page) = 5;
      *memory.At(1,page) = 6;
    }
    watch.TakeTraceDirtyPages(pages);
    CHECK(pages == (std::vector<uint32_t>{0,63,64,139}));
    ArmAll(watch,count);
    // The initial all-page report must survive a duplicate BeginTrace call.
    watch.EndTrace();
    CHECK(watch.BeginTrace());
    CHECK(!watch.BeginTrace());
    watch.TakeTraceDirtyPages(pages);
    CHECK(pages.size() == count);
    ArmAll(watch,count);
    watch.EndTrace();
    *memory.At(2,4) = 9;
    CHECK(watch.Token(4));
    watch.TakeTraceDirtyPages(pages);
    CHECK(pages.empty());
    BeginAndArm(watch,count);
    watch.EndTrace();
    watch.EndTrace();
  }
  // Destruction restores write access even on pages last armed by a trace.
  for (size_t alias=0; alias<memory.bases.size(); ++alias) *memory.At(alias,2) = 7;
  CHECK(Watch::Find(memory.bases[0],memory.size) == nullptr);
}

void PinnedKernelWrites() {
  constexpr size_t count = 5;
  Mapping memory(count);
  Watch watch(memory.bases[0],memory.size,memory.aliases);
  CHECK(watch.Enable());
  BeginAndArm(watch,count);
  std::vector<uint32_t> pages;
  const auto faults = watch.fault_count();
  const auto token = watch.Token(2);
  {
    // Cross a page boundary through the offset alias, just as a kernel I/O
    // destination can. Nested pins must hold protection until the final exit.
    xn::GuestWriteScope write(memory.At(2,2)+kPage-4,8);
    CHECK(watch.Token(2) == 0 && watch.Token(3) == 0);
    std::memset(memory.At(2,2)+kPage-4,0x4a,8);
    for (int take=0; take<3; ++take) {
      watch.TakeTraceDirtyPages(pages);
      CHECK(pages == (std::vector<uint32_t>{2,3}));
    }
    watch.Pin(memory.At(1,2),1);
    watch.Unpin(memory.At(1,2),1);
    CHECK(watch.Token(2) == 0);
    watch.TakeTraceDirtyPages(pages);
    CHECK(Contains(pages,2) && Contains(pages,3));
  }
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages == (std::vector<uint32_t>{2,3}));
  CHECK(watch.Token(2) > token && watch.Token(3));
  CHECK(*memory.At(0,2) == 0); // only the end of page 2 was written
  CHECK(memory.At(0,2)[kPage-1] == 0x4a && *memory.At(0,3) == 0x4a);
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages.empty());
  CHECK(watch.fault_count() == faults);

  // A real kernel read(2), unlike userspace writes, cannot be rescued by our
  // signal handler. GuestWriteScope must make the destination writable first.
  int pipes[2];
  CHECK(pipe(pipes) == 0);
  const uint8_t payload = 0x7b;
  CHECK(write(pipes[1],&payload,1) == 1);
  {
    xn::GuestWriteScope write(memory.At(1,1),1);
    CHECK(read(pipes[0],memory.At(1,1),1) == 1);
    watch.TakeTraceDirtyPages(pages);
    CHECK(pages == (std::vector<uint32_t>{1}));
  }
  close(pipes[0]);
  close(pipes[1]);
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages == (std::vector<uint32_t>{1}));
  CHECK(*memory.At(0,1) == payload && watch.Token(1));
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages.empty());
  watch.Pin(memory.At(0,0),0);
  watch.Unpin(memory.At(0,0),0);
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages.empty());
}

void DisabledPageStaysPending() {
  constexpr size_t count = 4;
  Mapping memory(count);
  Watch watch(memory.bases[0],memory.size,memory.aliases);
  CHECK(watch.Enable());
  BeginAndArm(watch,count);
  *memory.At(0,2) = 1;
  std::vector<uint32_t> pages;
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages == (std::vector<uint32_t>{2}));
  // A formerly valid alias disappearing makes protection unreliable. Such a
  // page must keep being copied rather than silently disappear from a trace.
  CHECK(munmap(memory.At(1,2),kPage) == 0);
  CHECK(watch.Token(2) == 0);
  for (int take=0; take<3; ++take) {
    watch.TakeTraceDirtyPages(pages);
    CHECK(pages == (std::vector<uint32_t>{2}));
    CHECK(watch.Token(2) == 0);
  }
  *memory.At(0,2) = 2;
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages == (std::vector<uint32_t>{2}));
  // Restore the hole so teardown also checks the ordinary alias cleanup path.
  CHECK(mmap(memory.At(1,2),kPage,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,
             memory.fd,off_t(2*kPage)) == memory.At(1,2));
}

void HierarchicalSummary() {
  // Dirty word 64 uses the second summary word. Only a few pages are armed;
  // this exercises the hierarchy without faulting/protecting the entire base.
  constexpr size_t count = 4163;
  Mapping memory(count);
  Watch watch(memory.bases[0],memory.size,memory.aliases);
  CHECK(watch.Enable());
  CHECK(watch.BeginTrace());
  std::vector<uint32_t> pages;
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages.size() == count);
  CHECK(pages.front() == 0 && pages.back() == count-1);
  const std::array<size_t,5> selected{0,4095,4096,4160,4162};
  for (auto page : selected) CHECK(watch.Token(page));
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages.empty());
  for (auto page : selected) *memory.At(1,page) = 1;
  watch.TakeTraceDirtyPages(pages);
  CHECK(std::vector<uint32_t>(selected.begin(),selected.end()) == pages);
  for (auto page : selected) CHECK(watch.Token(page));
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages.empty());
}

void ConcurrentFaultsAndRearming(bool shared_page) {
  constexpr size_t count = 130;
  constexpr size_t writers = 4;
  constexpr size_t rounds = 2000;
  const std::array<size_t,writers> selected = shared_page ?
      std::array<size_t,writers>{2,2,2,2} : std::array<size_t,writers>{1,63,64,129};
  Mapping memory(count);
  Watch watch(memory.bases[0],memory.size,memory.aliases);
  CHECK(watch.Enable());
  BeginAndArm(watch,count);
  std::atomic<size_t> start{0};
  std::array<std::atomic<size_t>,writers> completed{};
  std::array<std::thread,writers> threads;
  for (size_t i=0; i<writers; ++i) {
    threads[i] = std::thread([&,i] {
      for (size_t round=1; round<=rounds; ++round) {
        while (start.load(std::memory_order_acquire) < round) std::this_thread::yield();
        // Each writer has its own byte, but every alias is involved. Volatile
        // keeps this a real store which must encounter mprotect/SIGSEGV.
        *reinterpret_cast<volatile uint8_t*>(memory.At(i%3,selected[i])+i) = uint8_t(round);
        completed[i].store(round,std::memory_order_release);
      }
    });
  }
  std::vector<uint32_t> pages;
  std::array<uint8_t,writers> captured{};
  const auto capture = [&] {
    watch.TakeTraceDirtyPages(pages);
    CheckUnique(pages);
    for (auto page : pages) {
      watch.Token(page);
      for (size_t i=0; i<writers; ++i) {
        if (page == selected[i])
          captured[i] = *reinterpret_cast<volatile uint8_t*>(memory.At(0,page)+i);
      }
    }
  };
  bool valid = true;
  for (size_t round=1; round<=rounds; ++round) {
    for (auto page : selected) if (!watch.Token(page)) valid = false;
    start.store(round,std::memory_order_release);
    std::array<bool,writers> seen{};
    // Consume and rearm while faults and pending fault retries run on other
    // CPUs. The last drain after completion must retain every generation.
    for (;;) {
      capture();
      for (size_t i=0; i<writers; ++i) seen[i] |= Contains(pages,selected[i]);
      bool done = true;
      for (size_t i=0; i<writers; ++i)
        done &= completed[i].load(std::memory_order_acquire) == round;
      if (done) break;
      std::this_thread::yield();
    }
    capture();
    for (size_t i=0; i<writers; ++i) seen[i] |= Contains(pages,selected[i]);
    for (size_t i=0; i<writers; ++i) valid &= seen[i];
    // The captured copy is updated only for delivered dirty pages, including
    // takes before a faulting store retried. A later store must trigger another
    // publication; reading live memory directly here would miss that failure.
    for (size_t i=0; i<writers; ++i) {
      valid &= watch.Token(selected[i]) != 0;
      valid &= captured[i] == uint8_t(round);
    }
  }
  for (auto& thread : threads) thread.join();
  CHECK(valid);
  CHECK(watch.fault_count() >= (shared_page ? rounds : writers*rounds));
  watch.TakeTraceDirtyPages(pages);
  for (auto page : pages) CHECK(watch.Token(page));
  watch.TakeTraceDirtyPages(pages);
  CHECK(pages.empty());
}
} // namespace

int main() {
  try {
    AliasesAndLifecycle();
    PinnedKernelWrites();
    DisabledPageStaysPending();
    HierarchicalSummary();
    ConcurrentFaultsAndRearming(false);
    ConcurrentFaultsAndRearming(true);
    std::puts("write-watch trace probe passed: aliases, lifecycle, kernel writes, disabled pages, hierarchy, concurrent rearming");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr,"write-watch trace probe failed: %s\n",error.what());
    return 1;
  }
}
