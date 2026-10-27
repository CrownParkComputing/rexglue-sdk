#include "runtime/gfx/vulkan/prepared_index_cache.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Cache = xn::gfx::PreparedIndexCache;
using Range = xn::gfx::VertexIndexRange;
using Endian = rex::graphics::xenos::Endian;

void Check(bool result, int line) {
  if (!result) throw std::runtime_error("prepared index probe line " + std::to_string(line));
}
#define CHECK(x) Check(bool(x), __LINE__)

struct Memory {
  std::array<uint8_t, Cache::kPageSize * 4> source{};
  std::array<uint64_t, 4> versions{1, 1, 1, 1};
  uint64_t Token(uint32_t page) const { return versions.at(page); }
  void Write(size_t address, const void* bytes, size_t size) {
    std::memcpy(source.data() + address, bytes, size);
    for (size_t page = address / Cache::kPageSize;
         page <= (address + size - 1) / Cache::kPageSize; ++page) ++versions[page];
  }
};

bool Convert(Cache& cache, Cache::Key key, Memory& memory, std::vector<uint8_t>& output,
             Range& range) {
  const auto token = [&](uint32_t page) { return memory.Token(page); };
  output.resize(size_t(key.count) * key.width);
  if (cache.TryCopy(key, output.data(), range, token)) return true;
  std::vector<uint64_t> before;
  const bool tracked = cache.SnapshotTokens(key, before, token);
  std::vector<uint32_t> staged;
  if (tracked) staged.resize((output.size() + 3) / 4);
  uint8_t* destination = tracked ? reinterpret_cast<uint8_t*>(staged.data()) : output.data();
  const uint8_t* src = memory.source.data() + key.address;
  if (key.width == 2)
    range = xn::gfx::CopyVertexIndices<uint16_t, true>(
        reinterpret_cast<uint16_t*>(destination), src, key.count, Endian(key.endian));
  else
    range = xn::gfx::CopyVertexIndices<uint32_t, true>(
        reinterpret_cast<uint32_t*>(destination), src, key.count, Endian(key.endian));
  if (tracked) {
    std::memcpy(output.data(), staged.data(), output.size());
    cache.Store(key, std::move(staged), range, std::move(before), token);
  }
  return false;
}

std::vector<uint32_t> WordsFrom(const std::vector<uint8_t>& bytes) {
  std::vector<uint32_t> words((bytes.size() + 3) / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  return words;
}

void EndianAndMutation() {
  Cache cache;
  Memory memory;
  const std::array<uint8_t, 8> bytes{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
  constexpr uint32_t address = Cache::kPageSize - 4;
  memory.Write(address, bytes.data(), bytes.size());
  std::vector<uint8_t> output;
  Range range;
  for (uint32_t width : {2u, 4u}) for (uint32_t endian = 0; endian < 4; ++endian) {
    const Cache::Key key{address, 8 / width, width, endian, memory.source.data(), &memory};
    CHECK(!Convert(cache, key, memory, output, range));
    const auto original = output;
    CHECK(Convert(cache, key, memory, output, range));
    CHECK(output == original);
    CHECK(range.first <= range.last);
  }
  CHECK(cache.size() == 8 && cache.byte_count() == 8 * 8);
  const Cache::Key key{address, 4, 2, 0, memory.source.data(), &memory};
  CHECK(Convert(cache, key, memory, output, range));
  const auto original = output;
  const uint16_t changed = 0xabcd;
  memory.Write(address + 4, &changed, sizeof(changed)); // second touched page only
  CHECK(!Convert(cache, key, memory, output, range));
  CHECK(Convert(cache, key, memory, output, range));
  CHECK(output != original);
  CHECK(range.last == changed);
  CHECK(cache.size() == 8); // stale key replaced, other endian keys remain
}

void AdmissionAndIdentity() {
  Cache cache;
  Memory memory;
  const auto token = [&](uint32_t page) { return memory.Token(page); };
  const Cache::Key key{10, 3, 2, 0, memory.source.data(), &memory};
  std::vector<uint8_t> output;
  Range range;
  CHECK(!Convert(cache, key, memory, output, range));
  CHECK(Convert(cache, key, memory, output, range));
  auto different_base = key;
  different_base.source_base = output.data();
  CHECK(!cache.TryCopy(different_base, output.data(), range, token));
  auto different_watch = key;
  different_watch.watch_identity = &cache;
  CHECK(!cache.TryCopy(different_watch, output.data(), range, token));
  auto different_count = key;
  ++different_count.count;
  CHECK(!cache.TryCopy(different_count, output.data(), range, token));
  std::vector<uint64_t> before;
  CHECK(cache.SnapshotTokens(key, before, token));
  memory.versions[0] = 0; // pinned or untracked page
  CHECK(!cache.TryCopy(key, output.data(), range, token));
  CHECK(cache.size() == 0);
  CHECK(!cache.Store(key, WordsFrom(output), range, std::move(before), token));
  CHECK(!cache.SnapshotTokens(key, before, token));
  memory.versions[0] = 5;
  CHECK(!Convert(cache, key, memory, output, range));
  CHECK(cache.SnapshotTokens(key, before, token));
  ++memory.versions[0]; // write during conversion
  CHECK(!cache.Store(key, WordsFrom(output), range, std::move(before), token));
  CHECK(cache.size() == 1); // previous immutable version, now stale
  CHECK(!cache.TryCopy(key, output.data(), range, token));
  CHECK(cache.size() == 0);
  CHECK(!Convert(cache, key, memory, output, range));
  unsigned checks = 0;
  CHECK(!cache.TryCopy(key, output.data(), range, [&](uint32_t page) {
    if (++checks == 2) ++memory.versions[page]; // write observed after host copy
    return memory.Token(page);
  }));
  CHECK(cache.size() == 0);
  CHECK(!cache.SnapshotTokens({uint32_t(Cache::kPhysicalSize)-1, 1, 2, 0,
                                memory.source.data(), &memory}, before, token));
  CHECK(!cache.SnapshotTokens({0, uint32_t(Cache::kMaxEntryBytes / 2 + 1), 2, 0,
                                memory.source.data(), &memory}, before, token));
}

void BoundedEvictionAndReset() {
  Cache cache;
  Memory memory;
  std::vector<uint8_t> output;
  Range range;
  // Repeated unique keys in the same watched page exercise entry eviction.
  for (uint32_t count = 0; count < Cache::kMaxEntries + 32; ++count) {
    const Cache::Key key{count * 2, 1, 2, 0, memory.source.data(), &memory};
    CHECK(!Convert(cache, key, memory, output, range));
    CHECK(cache.size() <= Cache::kMaxEntries);
    CHECK(cache.byte_count() <= Cache::kMaxBytes);
    CHECK(cache.token_count() <= Cache::kMaxTokens);
  }
  CHECK(cache.size() == Cache::kMaxEntries);
  const Cache::Key first{0, 1, 2, 0, memory.source.data(), &memory};
  CHECK(!cache.TryCopy(first, output.data(), range,
                       [&](uint32_t page) { return memory.Token(page); }));
  cache.Reset();
  CHECK(cache.size() == 0 && cache.byte_count() == 0 && cache.token_count() == 0);

  // Large immutable snapshots displace older entries at the byte limit too.
  std::vector<uint32_t> megabyte(Cache::kMaxEntryBytes / 4, 0x5a5a5a5a);
  const auto stable = [](uint32_t) { return uint64_t{7}; };
  for (uint32_t i = 0; i < Cache::kMaxBytes / Cache::kMaxEntryBytes + 1; ++i) {
    const Cache::Key key{i * 2, uint32_t(Cache::kMaxEntryBytes / 2), 2, 0,
                         memory.source.data(), &memory};
    std::vector<uint64_t> before;
    CHECK(cache.SnapshotTokens(key, before, stable));
    CHECK(cache.Store(key, std::vector<uint32_t>(megabyte), {0, 1},
                      std::move(before), stable));
    CHECK(cache.byte_count() <= Cache::kMaxBytes);
  }
  CHECK(cache.byte_count() == Cache::kMaxBytes);
  CHECK(cache.size() == Cache::kMaxBytes / Cache::kMaxEntryBytes);
}
} // namespace

int main() {
  try {
    EndianAndMutation();
    AdmissionAndIdentity();
    BoundedEvictionAndReset();
    std::puts("prepared-index cache probe passed");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
