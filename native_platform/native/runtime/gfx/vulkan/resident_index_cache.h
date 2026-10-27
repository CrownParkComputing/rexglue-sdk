#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "vertex_index_range.h"

namespace xn::gfx {

// Manages offsets in one persistently mapped, immutable index-buffer arena.
// The caller serializes access, writes an allocation only before publishing it,
// and retires serials only after the existing queue fence has signalled.
class ResidentIndexCache {
 public:
  static constexpr uint32_t kPageSize = 4096;
  static constexpr uint64_t kPhysicalSize = uint64_t{1} << 29;
  static constexpr size_t kMaxEntryBytes = 4 * 1024 * 1024;
  static constexpr size_t kMaxEntries = 16384;
  static constexpr size_t kMaxTokens = 64 * 1024;
  // Includes unpublished and invalidated spans waiting for a pin or fence.
  static constexpr size_t kMaxAllocatedEntries = 2 * kMaxEntries;
  static constexpr size_t kMaxRetainedTokens = 4 * kMaxTokens;

  struct Key {
    uint32_t address = 0, count = 0, width = 0, endian = 0;
    const void* source_base = nullptr;
    const void* watch_identity = nullptr;
    bool operator==(const Key&) const = default;
  };

 private:
  struct Entry;
  using Iterator = std::list<Entry>::iterator;
  using EntryList = std::list<Iterator>;
  struct Entry {
    Key key;
    uint64_t offset = 0, size = 0, last_use = 0;
    size_t pins = 0;
    bool published = false, evictable = false;
    EntryList::iterator published_position{}, evictable_position{};
    VertexIndexRange range;
    std::vector<uint64_t> tokens;
  };

 public:
  class Lease {
   public:
    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept { *this = std::move(other); }
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        Release();
        cache_ = std::exchange(other.cache_, nullptr);
        entry_ = other.entry_;
      }
      return *this;
    }
    ~Lease() { Release(); }
    explicit operator bool() const { return cache_ != nullptr; }
    uint64_t offset() const { assert(cache_); return entry_->offset; }
    VertexIndexRange range() const { assert(cache_); return entry_->range; }
    bool published() const { assert(cache_); return entry_->published; }
    // Record immediately before vkCmdDrawIndexed. A serial is assigned even
    // when a newly converted snapshot could not be admitted to lookup.
    void MarkUsed(uint64_t serial) {
      assert(cache_ && serial > cache_->completed_serial_);
      cache_->RemoveEvictable(entry_);
      entry_->last_use = std::max(entry_->last_use, serial);
    }
    void Release() {
      if (!cache_) return;
      assert(entry_->pins);
      --entry_->pins;
      if (!cache_->TryCollect(entry_)) cache_->RefreshEvictable(entry_);
      cache_ = nullptr;
    }
   private:
    friend class ResidentIndexCache;
    Lease(ResidentIndexCache* cache, Iterator entry) : cache_(cache), entry_(entry) {
      ++entry_->pins;
    }
    ResidentIndexCache* cache_ = nullptr;
    Iterator entry_{};
  };

  // Init only before first use or after all GPU work and leases have retired.
  void Reset(uint64_t capacity = 0) {
    assert(std::all_of(entries_.begin(), entries_.end(),
                       [](const Entry& e) { return !e.pins; }));
    entries_.clear();
    lookup_.clear();
    published_lru_.clear();
    evictable_.clear();
    inactive_.clear();
    free_.clear();
    if (capacity) free_.emplace(0, capacity);
    capacity_ = capacity;
    completed_serial_ = failed_size_ = 0;
    token_count_ = allocated_entry_count_ = retained_token_count_ = 0;
  }
  uint64_t capacity() const { return capacity_; }
  size_t size() const { return lookup_.size(); }
  uint64_t completed_serial() const { return completed_serial_; }

  // Guest mapping/watch rebinding invalidates every lookup, but submitted
  // spans remain quarantined until their queue serial and pins retire.
  void InvalidateAll() {
    for (auto& entry : entries_) {
      entry.published = false;
      entry.evictable = false;
    }
    lookup_.clear();
    published_lru_.clear();
    evictable_.clear();
    inactive_.clear();
    token_count_ = 0;
    CollectDead();
  }

  void Retire(uint64_t completed_serial) {
    if (completed_serial <= completed_serial_) return;
    failed_size_ = 0;
    completed_serial_ = completed_serial;
    CollectDead();
    RebuildEvictable();
  }

  template <typename TokenFn>
  Lease Lookup(const Key& key, TokenFn&& token) {
    const auto found = lookup_.find(key);
    if (found == lookup_.end()) return {};
    auto entry = found->second;
    if (!TokensMatch(*entry, token)) {
      Invalidate(entry);
      return {};
    }
    RemoveEvictable(entry);
    published_lru_.splice(published_lru_.end(), published_lru_, entry->published_position);
    return Lease(this, entry);
  }

  template <typename TokenFn>
  bool SnapshotTokens(const Key& key, std::vector<uint64_t>& tokens, TokenFn&& token) const {
    tokens.clear();
    const size_t pages = PageCount(key);
    if (!capacity_ || !pages || pages > kMaxTokens) return false;
    tokens.reserve(pages);
    const uint32_t first = key.address / kPageSize;
    for (size_t i = 0; i < pages; ++i) {
      const uint64_t current = token(first + uint32_t(i));
      if (!current) { tokens.clear(); return false; }
      tokens.push_back(current);
    }
    return true;
  }

  // The returned span is private until Publish. If tokens change during the
  // caller's conversion, the lease still owns a drawable snapshot but cannot
  // become a future hit. Its bytes remain fenced after MarkUsed.
  Lease Allocate(const Key& key, const std::vector<uint64_t>& tokens) {
    const uint64_t bytes = ByteCount(key);
    if (!capacity_ || !bytes || bytes > capacity_ || tokens.size() != PageCount(key) ||
        std::any_of(tokens.begin(), tokens.end(), [](uint64_t value) { return !value; })) return {};
    const uint64_t size = (bytes + 3) & ~uint64_t(3);
    if (failed_size_ && size >= failed_size_) return {};
    while (allocated_entry_count_ >= kMaxAllocatedEntries ||
           tokens.size() > kMaxRetainedTokens - retained_token_count_) {
      auto victim = OldestEvictable();
      if (victim == entries_.end()) return {};
      Invalidate(victim);  // Frees only this already retired, unpinned span.
    }
    auto space = FindSpace(size);
    while (space == free_.end()) {
      auto victim = OldestEvictable();
      if (victim == entries_.end()) {
        failed_size_ = failed_size_ ? std::min(failed_size_, size) : size;
        return {};
      }
      Invalidate(victim);
      space = FindSpace(size);
    }
    const uint64_t offset = space->first;
    const uint64_t remaining = space->second - size;
    free_.erase(space);
    if (remaining) free_.emplace(offset + size, remaining);
    entries_.push_back(Entry{key, offset, size, 0, 0, false, false, {}, {}, {}, tokens});
    ++allocated_entry_count_;
    retained_token_count_ += tokens.size();
    return Lease(this, std::prev(entries_.end()));
  }

  template <typename TokenFn>
  bool Publish(Lease& lease, VertexIndexRange range, TokenFn&& token) {
    if (!lease || lease.cache_ != this) return false;
    auto entry = lease.entry_;
    entry->range = range;
    if (!TokensMatch(*entry, token)) return false;
    if (auto old = lookup_.find(entry->key); old != lookup_.end())
      Invalidate(old->second);
    while (lookup_.size() >= kMaxEntries ||
           entry->tokens.size() > kMaxTokens - token_count_) {
      auto victim = OldestPublished(entry);
      if (victim == entries_.end()) return false;
      Invalidate(victim);
    }
    entry->published = true;
    token_count_ += entry->tokens.size();
    lookup_.emplace(entry->key, entry);
    published_lru_.push_back(entry);
    entry->published_position = std::prev(published_lru_.end());
    inactive_.push_back(entry);
    entry->evictable_position = std::prev(inactive_.end());
    RefreshEvictable(entry);
    return true;
  }

 private:
  struct KeyHash {
    size_t operator()(const Key& key) const {
      uint64_t hash = (uint64_t(key.address) << 32) | key.count;
      for (uint64_t value : {uint64_t(key.width), uint64_t(key.endian),
                             uint64_t(reinterpret_cast<uintptr_t>(key.source_base)),
                             uint64_t(reinterpret_cast<uintptr_t>(key.watch_identity))})
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
      return size_t(hash);
    }
  };
  static uint64_t ByteCount(const Key& key) {
    if (!key.count || (key.width != 2 && key.width != 4) || key.endian > 3 ||
        !key.source_base || !key.watch_identity) return 0;
    const uint64_t bytes = uint64_t(key.count) * key.width;
    return bytes <= kMaxEntryBytes && uint64_t(key.address) + bytes <= kPhysicalSize ? bytes : 0;
  }
  static size_t PageCount(const Key& key) {
    const uint64_t bytes = ByteCount(key);
    return bytes ? size_t((key.address % kPageSize + bytes + kPageSize - 1) / kPageSize) : 0;
  }
  template <typename TokenFn>
  static bool TokensMatch(const Entry& entry, TokenFn&& token) {
    if (entry.tokens.empty()) return false;
    const uint32_t first = entry.key.address / kPageSize;
    for (size_t i = 0; i < entry.tokens.size(); ++i) {
      const uint64_t current = token(first + uint32_t(i));
      if (!current || current != entry.tokens[i]) return false;
    }
    return true;
  }
  void Invalidate(Iterator entry) {
    if (!entry->published) return;
    lookup_.erase(entry->key);
    published_lru_.erase(entry->published_position);
    if (entry->evictable) evictable_.erase(entry->evictable_position);
    else inactive_.erase(entry->evictable_position);
    entry->evictable = false;
    token_count_ -= entry->tokens.size();
    entry->published = false;
    TryCollect(entry);
  }
  void FreeSpan(uint64_t offset, uint64_t size) {
    auto next = free_.lower_bound(offset);
    if (next != free_.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == offset) {
        offset = prev->first;
        size += prev->second;
        free_.erase(prev);
      }
    }
    if (next != free_.end() && offset + size == next->first) {
      size += next->second;
      free_.erase(next);
    }
    free_.emplace(offset, size);
    failed_size_ = 0;
  }
  void RemoveEvictable(Iterator entry) {
    if (entry->evictable) {
      inactive_.splice(inactive_.end(), evictable_, entry->evictable_position);
      entry->evictable = false;
    }
  }
  void RefreshEvictable(Iterator entry) {
    if (entry->published && !entry->pins && entry->last_use <= completed_serial_) {
      if (!entry->evictable) {
        evictable_.splice(evictable_.end(), inactive_, entry->evictable_position);
        entry->evictable = true;
        failed_size_ = 0;
      }
    } else if (entry->published) RemoveEvictable(entry);
  }
  bool TryCollect(Iterator entry) {
    if (entry->published || entry->pins || entry->last_use > completed_serial_) return false;
    RemoveEvictable(entry);
    FreeSpan(entry->offset, entry->size);
    --allocated_entry_count_;
    retained_token_count_ -= entry->tokens.size();
    entries_.erase(entry);
    return true;
  }
  void CollectDead() {
    for (auto it = entries_.begin(); it != entries_.end();) {
      auto current = it++;
      TryCollect(current);
    }
  }
  void RebuildEvictable() {
    for (auto entry : published_lru_) {
      if (!entry->pins && entry->last_use <= completed_serial_) {
        if (entry->evictable)
          evictable_.splice(evictable_.end(), evictable_, entry->evictable_position);
        else evictable_.splice(evictable_.end(), inactive_, entry->evictable_position);
        entry->evictable = true;
      } else {
        if (entry->evictable)
          inactive_.splice(inactive_.end(), evictable_, entry->evictable_position);
        else inactive_.splice(inactive_.end(), inactive_, entry->evictable_position);
        entry->evictable = false;
      }
    }
  }
  std::map<uint64_t, uint64_t>::iterator FindSpace(uint64_t size) {
    return std::find_if(free_.begin(), free_.end(),
                        [size](const auto& span) { return span.second >= size; });
  }
  Iterator OldestEvictable() {
    return evictable_.empty() ? entries_.end() : evictable_.front();
  }
  Iterator OldestPublished(Iterator exclude) {
    (void)exclude;  // The new entry is unpublished until budget admission.
    return published_lru_.empty() ? entries_.end() : published_lru_.front();
  }
  uint64_t capacity_ = 0, completed_serial_ = 0;
  size_t token_count_ = 0;
  size_t allocated_entry_count_ = 0, retained_token_count_ = 0;
  uint64_t failed_size_ = 0;
  std::list<Entry> entries_;
  std::unordered_map<Key, Iterator, KeyHash> lookup_;
  EntryList published_lru_, evictable_, inactive_;
  std::map<uint64_t, uint64_t> free_;
};

}  // namespace xn::gfx
