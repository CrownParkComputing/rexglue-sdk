#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "vertex_index_range.h"

namespace xn::gfx {

// Converted DMA indices are immutable allocations in the current batch's
// index ring. Reset only after BeginFrameIfNeeded has retired the batch fence,
// before reusing a ring segment. Never reuse entries across batch boundaries.
class DmaIndexCache {
 public:
  static constexpr size_t kMaxEntries = 4096;
  static constexpr size_t kMaxTokens = 64*1024;
  static constexpr uint32_t kPageSize = 4096;
  static constexpr uint64_t kPhysicalSize = uint64_t{1}<<29;
  struct Key {
    uint32_t address = 0, count = 0, width = 0, endian = 0;
    bool operator==(const Key&) const = default;
  };
  struct Entry {
    uint64_t offset = 0;
    // Raw extrema, before the draw's current vertex base/minimum/maximum.
    VertexIndexRange range;
    std::vector<uint64_t> tokens;
  };

  void Reset() {
    entries_.clear();
    token_count_ = 0;
    ++generation_;
  }
  uint64_t generation() const { return generation_; }
  size_t size() const { return entries_.size(); }
  size_t token_count() const { return token_count_; }

  // TokenFn(page) must use the same all-alias write watch as the renderer's
  // physical-memory mirror. Zero tokens (including pinned kernel writes) are
  // never accepted as evidence that converted bytes remain valid.
  template <typename TokenFn>
  const Entry* Find(const Key& key, TokenFn&& token) {
    auto found = entries_.find(key);
    if (found == entries_.end()) return nullptr;
    const uint32_t first = key.address/kPageSize;
    for (size_t i=0; i<found->second.tokens.size(); ++i) {
      const uint64_t current = token(first+uint32_t(i));
      if (!current || current != found->second.tokens[i]) {
        token_count_ -= found->second.tokens.size();
        entries_.erase(found);
        return nullptr;
      }
    }
    return &found->second;
  }

  // Call immediately before the source-to-ring conversion. The caller must
  // still perform the ordinary conversion if tracking/admission is unavailable.
  template <typename TokenFn>
  bool SnapshotTokens(const Key& key, std::vector<uint64_t>& tokens, TokenFn&& token) const {
    tokens.clear();
    const size_t count = PageCount(key);
    if (!count || entries_.size() >= kMaxEntries || count > kMaxTokens-token_count_)
      return false;
    tokens.reserve(count);
    const uint32_t first = key.address/kPageSize;
    for (size_t i=0; i<count; ++i) {
      const uint64_t current = token(first+uint32_t(i));
      if (!current) { tokens.clear(); return false; }
      tokens.push_back(current);
    }
    return true;
  }

  // Admit only when the pre-conversion tokens are still nonzero and unchanged
  // after conversion. A mutation never overwrites an earlier ring allocation.
  template <typename TokenFn>
  bool Store(const Key& key, uint64_t offset, VertexIndexRange range,
             std::vector<uint64_t>&& tokens, TokenFn&& token) {
    if (tokens.empty() || tokens.size() != PageCount(key) ||
        entries_.size() >= kMaxEntries || tokens.size() > kMaxTokens-token_count_)
      return false;
    const uint32_t first = key.address/kPageSize;
    for (size_t i=0; i<tokens.size(); ++i) {
      const uint64_t current = token(first+uint32_t(i));
      if (!current || current != tokens[i]) return false;
    }
    const size_t count = tokens.size();
    const auto [entry, inserted] = entries_.try_emplace(key,Entry{offset,range,std::move(tokens)});
    if (!inserted) return false;
    token_count_ += count;
    return true;
  }

 private:
  struct KeyHash {
    size_t operator()(const Key& key) const {
      uint64_t hash = (uint64_t(key.address)<<32)|key.count;
      hash ^= uint64_t(key.width)<<7;
      hash ^= uint64_t(key.endian)<<3;
      hash ^= hash>>33;
      hash *= 0xff51afd7ed558ccdull;
      hash ^= hash>>33;
      return size_t(hash);
    }
  };
  static size_t PageCount(const Key& key) {
    if (!key.count || (key.width != 2 && key.width != 4) || key.endian > 3) return 0;
    const uint64_t bytes = uint64_t(key.count)*key.width;
    if (uint64_t(key.address)+bytes > kPhysicalSize) return 0;
    return size_t((uint64_t(key.address%kPageSize)+bytes+kPageSize-1)/kPageSize);
  }
  std::unordered_map<Key,Entry,KeyHash> entries_;
  size_t token_count_ = 0;
  uint64_t generation_ = 0;
};

} // namespace xn::gfx
