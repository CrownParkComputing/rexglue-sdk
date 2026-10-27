#include "runtime/gfx/vulkan/resident_index_cache.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
bool count_allocations = false;
size_t allocation_count = 0;
}

__attribute__((noinline)) void* operator new(std::size_t size) {
  if (count_allocations) ++allocation_count;
  if (void* result = std::malloc(size ? size : 1)) return result;
  throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* pointer) noexcept { std::free(pointer); }
__attribute__((noinline)) void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}

namespace {
using Cache = xn::gfx::ResidentIndexCache;

void Check(bool value, int line) {
  if (!value) throw std::runtime_error("resident index probe line " + std::to_string(line));
}
#define CHECK(x) Check(bool(x), __LINE__)

struct Source {
  std::array<uint8_t, Cache::kPageSize * 3> bytes{};
  std::array<uint64_t, 3> versions{1, 1, 1};
  uint64_t Token(uint32_t page) const { return versions.at(page); }
  Cache::Key Key(uint32_t address, uint32_t count, uint32_t width = 2) const {
    return {address, count, width, 0, bytes.data(), this};
  }
};

Cache::Lease Admit(Cache& cache, Cache::Key key, Source& source, uint64_t serial = 0) {
  const auto token = [&](uint32_t page) { return source.Token(page); };
  std::vector<uint64_t> before;
  CHECK(cache.SnapshotTokens(key, before, token));
  auto lease = cache.Allocate(key, before);
  CHECK(lease);
  CHECK(cache.Publish(lease, {0, key.count - 1}, token));
  if (serial) lease.MarkUsed(serial);
  return lease;
}

void TokensIdentityAndMutation() {
  Cache cache;
  cache.Reset(64);
  Source source;
  const auto key = source.Key(Cache::kPageSize - 2, 2);
  const auto token = [&](uint32_t page) { return source.Token(page); };
  {
    auto first = Admit(cache, key, source, 1);
    CHECK(first.offset() == 0 && first.range().last == 1);
    auto hit = cache.Lookup(key, token);
    CHECK(hit && hit.offset() == first.offset());
    hit.MarkUsed(1);
  }
  CHECK(cache.size() == 1);
  ++source.versions[1];
  CHECK(!cache.Lookup(key, token));
  CHECK(cache.size() == 0);
  CHECK(!cache.Allocate({uint32_t(Cache::kPhysicalSize - 1), 1, 2, 0,
                         source.bytes.data(), &source}, {1}));

  std::vector<uint64_t> before;
  CHECK(cache.SnapshotTokens(key, before, token));
  auto changing = cache.Allocate(key, before);
  CHECK(changing);
  ++source.versions[0];  // Changed during conversion: drawable, never a future hit.
  CHECK(!cache.Publish(changing, {3, 7}, token));
  CHECK(!cache.Lookup(key, token));
  changing.MarkUsed(2);
  changing.Release();
  cache.Retire(1);
  CHECK(cache.size() == 0);
  cache.Retire(2);

  source.versions[0] = 0;  // Pinned or untracked source cannot be admitted.
  CHECK(!cache.SnapshotTokens(key, before, token));
  CHECK(!cache.Allocate(key, {0, 1}));
  source.versions[0] = 3;
  auto valid = Admit(cache, key, source);
  auto rebound = key;
  rebound.source_base = &cache;
  CHECK(!cache.Lookup(rebound, token));
  rebound = key;
  rebound.watch_identity = &cache;
  CHECK(!cache.Lookup(rebound, token));
  cache.InvalidateAll();
  CHECK(!cache.Lookup(key, token));
  CHECK(valid);  // The invalidated span remains pinned until release.
  valid.Release();
  CHECK(cache.size() == 0);
}

void FencePinsEvictionAndCoalescing() {
  Cache cache;
  cache.Reset(24);
  Source source;
  const auto token = [&](uint32_t page) { return source.Token(page); };
  const auto a = source.Key(0, 4);
  const auto b = source.Key(8, 4);
  const auto c = source.Key(16, 4);
  const auto d = source.Key(24, 4);
  const auto wide = source.Key(32, 8);
  {
    auto la = Admit(cache, a, source, 1);
    auto lb = Admit(cache, b, source, 2);
    auto lc = Admit(cache, c, source, 3);
    CHECK(la.offset() == 0 && lb.offset() == 8 && lc.offset() == 16);
    la.Release(); lb.Release(); lc.Release();
  }
  std::vector<uint64_t> before;
  CHECK(cache.SnapshotTokens(d, before, token));
  CHECK(!cache.Allocate(d, before));  // All spans are still in flight.
  cache.Retire(1);
  auto ld = cache.Allocate(d, before);
  CHECK(ld && ld.offset() == 0);  // Only the first span can be evicted.
  ld.MarkUsed(4);  // A not-yet-submitted serial must still pin the span.
  ld.Release();
  CHECK(!cache.Allocate(wide, std::vector<uint64_t>{1}));
  cache.Retire(3);
  CHECK(cache.SnapshotTokens(wide, before, token));
  auto lw = cache.Allocate(wide, before);
  CHECK(lw && lw.offset() == 8);  // Evict adjacent B/C; free spans coalesce.
  CHECK(cache.Publish(lw, {0, 7}, token));
  lw.MarkUsed(5);
  cache.InvalidateAll();
  CHECK(!cache.Lookup(wide, token));
  CHECK(!cache.Allocate(wide, before)); // Still pinned, despite invalidation.
  lw.Release();
  cache.Retire(4);
  CHECK(!cache.Allocate(wide, before)); // Serial 5 has not completed.
  cache.Retire(5);
  auto reused = cache.Allocate(wide, before);
  CHECK(reused);
  CHECK(reused.offset() == 0);
  reused.Release();

  const auto oversize = source.Key(0, 13); // 26 bytes > arena capacity.
  CHECK(!cache.SnapshotTokens(oversize, before, token) ||
        !cache.Allocate(oversize, before));
}

void RepeatedFailuresAndPinProgress() {
  Cache cache;
  cache.Reset(8);
  Source source;
  const auto a = source.Key(0, 4), b = source.Key(8, 4);
  const auto token = [&](uint32_t page) { return source.Token(page); };
  auto pinned = Admit(cache, a, source);
  std::vector<uint64_t> before;
  CHECK(cache.SnapshotTokens(b, before, token));
  for (unsigned i = 0; i < 1024; ++i) CHECK(!cache.Allocate(b, before));
  pinned.Release();  // Newly evictable entry clears the failed-size guard.
  auto next = cache.Allocate(b, before);
  CHECK(next && next.offset() == 0);
  CHECK(cache.Publish(next, {0, 3}, token));
  next.MarkUsed(1);
  next.Release();
  CHECK(cache.SnapshotTokens(a, before, token));
  for (unsigned i = 0; i < 1024; ++i) CHECK(!cache.Allocate(a, before));
  cache.Retire(1);  // Fence completion clears the guard and makes B evictable.
  auto retired = cache.Allocate(a, before);
  CHECK(retired && retired.offset() == 0);
}

void LruHitAndEviction() {
  Cache cache;
  cache.Reset(16);
  Source source;
  const auto a = source.Key(0, 4), b = source.Key(8, 4), c = source.Key(16, 4);
  const auto token = [&](uint32_t page) { return source.Token(page); };
  auto first = Admit(cache, a, source);
  auto second = Admit(cache, b, source);
  first.Release();
  second.Release();
  auto hit = cache.Lookup(a, token);
  CHECK(hit && hit.offset() == 0);
  hit.Release();  // A is now newer than B in the published LRU.
  std::vector<uint64_t> before;
  CHECK(cache.SnapshotTokens(c, before, token));
  auto replacement = cache.Allocate(c, before);
  CHECK(replacement && replacement.offset() == 8);
  CHECK(!cache.Lookup(b, token));
  CHECK(cache.Lookup(a, token));
}

void TotalAllocationAndTokenBudgets() {
  Source source;
  const auto small = source.Key(0, 2); // Four bytes per span.
  {
    Cache cache;
    cache.Reset((Cache::kMaxAllocatedEntries + 2) * 4);
    std::vector<Cache::Lease> pending;
    pending.reserve(Cache::kMaxAllocatedEntries);
    for (size_t i = 0; i < Cache::kMaxAllocatedEntries; ++i) {
      auto lease = cache.Allocate(small, {1});
      CHECK(lease);
      lease.MarkUsed(1);
      pending.push_back(std::move(lease));
    }
    for (unsigned i = 0; i < 64; ++i) CHECK(!cache.Allocate(small, {1}));
    for (auto& lease : pending) lease.Release();
    CHECK(!cache.Allocate(small, {1})); // Released, but serial 1 is in flight.
    cache.Retire(0); // No progress must not reclaim the spans.
    CHECK(!cache.Allocate(small, {1}));
    cache.Retire(1);
    CHECK(cache.Allocate(small, {1}));
  }
  {
    Cache cache;
    constexpr size_t pages = Cache::kMaxEntryBytes / Cache::kPageSize;
    constexpr size_t count = Cache::kMaxRetainedTokens / pages;
    cache.Reset((count + 2) * Cache::kMaxEntryBytes);
    const auto large = source.Key(0, uint32_t(Cache::kMaxEntryBytes / 2));
    const std::vector<uint64_t> tokens(pages, 1);
    std::vector<Cache::Lease> pending;
    pending.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      auto lease = cache.Allocate(large, tokens);
      CHECK(lease);
      lease.MarkUsed(1);
      pending.push_back(std::move(lease));
    }
    CHECK(!cache.Allocate(large, tokens));
    for (auto& lease : pending) lease.Release();
    CHECK(!cache.Allocate(large, tokens));
    cache.Retire(1);
    CHECK(cache.Allocate(large, tokens));
  }
}

void NoAllocationOnHitOrRetire() {
  Cache cache;
  cache.Reset(16);
  Source source;
  const auto a = source.Key(0, 4), b = source.Key(8, 4);
  const auto token = [&](uint32_t page) { return source.Token(page); };
  auto first = Admit(cache, a, source, 1);
  auto second = Admit(cache, b, source, 1);
  first.Release();
  second.Release();
  cache.Retire(1);
  allocation_count = 0;
  count_allocations = true;
  for (uint64_t serial = 2; serial < 1026; ++serial) {
    auto hit = cache.Lookup(a, token);
    hit.Release();
    cache.Retire(serial);
  }
  count_allocations = false;
  CHECK(allocation_count == 0);
  CHECK(cache.size() == 2);
}
}  // namespace

int main() {
  try {
    TokensIdentityAndMutation();
    FencePinsEvictionAndCoalescing();
    RepeatedFailuresAndPinProgress();
    LruHitAndEviction();
    TotalAllocationAndTokenBudgets();
    NoAllocationOnHitOrRetire();
    std::puts("resident-index cache probe passed");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
