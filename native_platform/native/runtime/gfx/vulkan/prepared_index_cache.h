#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

#include "vertex_index_range.h"

namespace xn::gfx {

// Host-owned converted DMA indices. Unlike DmaIndexCache, these entries have
// no GPU ring offsets and may survive a command batch. Every draw copies a
// validated snapshot into its own current-batch allocation. The caller must
// serialize cache operations and keep ordinary guest-buffer ownership rules.
class PreparedIndexCache {
 public:
  static constexpr size_t kMaxEntries = 4096;
  static constexpr size_t kMaxBytes = 16 * 1024 * 1024;
  static constexpr size_t kMaxEntryBytes = 1024 * 1024;
  static constexpr size_t kMaxTokens = 64 * 1024;
  static constexpr uint32_t kPageSize = 4096;
  static constexpr uint64_t kPhysicalSize = uint64_t{1} << 29;

  struct Key {
    uint32_t address = 0, count = 0, width = 0, endian = 0;
    // A physical address alone does not identify a guest mapping after rebind.
    const void* source_base = nullptr;
    const void* watch_identity = nullptr;
    bool operator==(const Key&) const = default;
  };

  void Reset() {
    entries_.clear();
    lookup_.clear();
    byte_count_ = token_count_ = 0;
  }
  size_t size() const { return entries_.size(); }
  size_t byte_count() const { return byte_count_; }
  size_t token_count() const { return token_count_; }

  // Copy only while all touched pages still have the recorded nonzero token.
  // A concurrent write seen during the copy invalidates this hit; the caller
  // then converts the guest source into the same fresh destination instead.
  template <typename TokenFn>
  bool TryCopy(const Key& key, uint8_t* destination, VertexIndexRange& range,
               TokenFn&& token) {
    auto found = lookup_.find(key);
    if (found == lookup_.end()) return false;
    auto entry = found->second;
    if (!TokensMatch(*entry, token)) {
      Erase(entry);
      return false;
    }
    std::memcpy(destination, entry->words.data(), ByteCount(key));
    if (!TokensMatch(*entry, token)) {
      Erase(entry);
      return false;
    }
    range = entry->range;
    entries_.splice(entries_.begin(), entries_, entry);
    return true;
  }

  // Snapshot immediately before guest conversion. A zero token means a
  // pinned, untracked, or transitioning page and forbids admission.
  template <typename TokenFn>
  bool SnapshotTokens(const Key& key, std::vector<uint64_t>& tokens, TokenFn&& token) const {
    tokens.clear();
    const size_t count = PageCount(key);
    if (!count || count > kMaxTokens) return false;
    tokens.reserve(count);
    const uint32_t first = key.address / kPageSize;
    for (size_t i = 0; i < count; ++i) {
      const uint64_t current = token(first + uint32_t(i));
      if (!current) { tokens.clear(); return false; }
      tokens.push_back(current);
    }
    return true;
  }

  // Converted words are immutable and moved from CPU staging, never read back
  // from a mapped GPU upload ring. Validate after conversion and upload, then
  // evict least recently used entries to maintain hard limits.
  template <typename TokenFn>
  bool Store(const Key& key, std::vector<uint32_t>&& converted, VertexIndexRange range,
             std::vector<uint64_t>&& tokens, TokenFn&& token) {
    const size_t bytes = ByteCount(key);
    if (!bytes || converted.size() != (bytes + 3) / 4 || tokens.empty() ||
        tokens.size() != PageCount(key)) return false;
    Entry entry{key, std::move(converted), range, std::move(tokens)};
    if (!TokensMatch(entry, token)) return false;
    if (auto found = lookup_.find(key); found != lookup_.end()) Erase(found->second);
    while (!entries_.empty() && (entries_.size() >= kMaxEntries ||
           bytes > kMaxBytes - byte_count_ ||
           entry.tokens.size() > kMaxTokens - token_count_))
      Erase(std::prev(entries_.end()));
    if (bytes > kMaxBytes - byte_count_ || entry.tokens.size() > kMaxTokens - token_count_)
      return false;
    entries_.push_front(std::move(entry));
    lookup_.emplace(key, entries_.begin());
    byte_count_ += bytes;
    token_count_ += entries_.front().tokens.size();
    return true;
  }

 private:
  struct Entry {
    Key key;
    std::vector<uint32_t> words;
    VertexIndexRange range;
    std::vector<uint64_t> tokens;
  };
  struct KeyHash {
    size_t operator()(const Key& key) const {
      uint64_t hash = (uint64_t(key.address) << 32) | key.count;
      for (uint64_t value : {uint64_t(key.width), uint64_t(key.endian),
                             uint64_t(reinterpret_cast<uintptr_t>(key.source_base)),
                             uint64_t(reinterpret_cast<uintptr_t>(key.watch_identity))}) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
      }
      return size_t(hash);
    }
  };
  static size_t ByteCount(const Key& key) {
    if (!key.count || (key.width != 2 && key.width != 4) || key.endian > 3 ||
        !key.source_base || !key.watch_identity) return 0;
    const uint64_t bytes = uint64_t(key.count) * key.width;
    if (bytes > kMaxEntryBytes || uint64_t(key.address) + bytes > kPhysicalSize) return 0;
    return size_t(bytes);
  }
  static size_t PageCount(const Key& key) {
    const size_t bytes = ByteCount(key);
    return bytes ? (size_t(key.address % kPageSize) + bytes + kPageSize - 1) / kPageSize : 0;
  }
  template <typename TokenFn>
  static bool TokensMatch(const Entry& entry, TokenFn&& token) {
    const uint32_t first = entry.key.address / kPageSize;
    for (size_t i = 0; i < entry.tokens.size(); ++i) {
      const uint64_t current = token(first + uint32_t(i));
      if (!current || current != entry.tokens[i]) return false;
    }
    return true;
  }
  void Erase(std::list<Entry>::iterator entry) {
    byte_count_ -= ByteCount(entry->key);
    token_count_ -= entry->tokens.size();
    lookup_.erase(entry->key);
    entries_.erase(entry);
  }
  std::list<Entry> entries_;
  std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> lookup_;
  size_t byte_count_ = 0;
  size_t token_count_ = 0;
};

} // namespace xn::gfx
