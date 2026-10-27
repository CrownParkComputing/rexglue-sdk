#include "runtime/gfx/vulkan/dma_index_cache.h"
#include "runtime/memory/physical_write_watch.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
using Cache = xn::gfx::DmaIndexCache;
using Range = xn::gfx::VertexIndexRange;
using Watch = xn::PhysicalWriteWatch;
using Endian = rex::graphics::xenos::Endian;

void Check(bool ok, const char* expression, int line) {
  if (!ok) throw std::runtime_error("line "+std::to_string(line)+": "+expression);
}
#define CHECK(expression) Check(bool(expression),#expression,__LINE__)

struct Memory {
  static constexpr size_t kSize = 4*Watch::kPageSize;
  Memory() {
    fd = int(syscall(SYS_memfd_create,"dma-index-cache-probe",0));
    CHECK(fd >= 0 && ftruncate(fd,kSize) == 0);
    for (auto& base : bases) {
      base = static_cast<uint8_t*>(mmap(nullptr,kSize,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));
      CHECK(base != MAP_FAILED);
      aliases.push_back({base,kSize,0});
    }
  }
  ~Memory() {
    for (auto* base : bases) munmap(base,kSize);
    close(fd);
  }
  int fd = -1;
  std::array<uint8_t*,3> bases{};
  std::vector<Watch::Alias> aliases;
};

struct Ring {
  alignas(4) std::array<uint8_t,16384> bytes{};
  uint64_t head = 0;
};

struct Result { uint64_t offset; Range range; bool hit; };

Result Convert(Cache& cache, const Cache::Key& key, Watch& watch, Memory& memory, Ring& ring) {
  const auto token = [&](uint32_t page) { return watch.Token(page); };
  if (const auto* entry = cache.Find(key,token)) return {entry->offset,entry->range,true};
  std::vector<uint64_t> before;
  const bool tracked = cache.SnapshotTokens(key,before,token);
  const uint64_t offset = (ring.head+key.width-1)&~uint64_t(key.width-1);
  ring.head = offset+uint64_t(key.count)*key.width;
  CHECK(ring.head <= ring.bytes.size());
  const auto endian = Endian(key.endian);
  const auto* source = memory.bases[0]+key.address;
  const auto range = key.width == 4 ?
      xn::gfx::CopyVertexIndices<uint32_t,true>(reinterpret_cast<uint32_t*>(ring.bytes.data()+offset),
                                              source,key.count,endian) :
      xn::gfx::CopyVertexIndices<uint16_t,true>(reinterpret_cast<uint16_t*>(ring.bytes.data()+offset),
                                              source,key.count,endian);
  if (tracked) cache.Store(key,offset,range,std::move(before),token);
  return {offset,range,false};
}

void EndianAndKeySeparation() {
  Memory memory;
  Watch watch(memory.bases[0],Memory::kSize,memory.aliases);
  CHECK(watch.Enable());
  Cache cache;
  Ring ring;
  constexpr uint32_t address = Watch::kPageSize-4;
  const uint8_t input[]{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc};
  std::memcpy(memory.bases[0]+address,input,sizeof(input));
  const std::array<std::array<uint32_t,3>,4> expected{{
      {0x44332211,0x88776655,0xccbbaa99},
      {0x33441122,0x77885566,0xbbcc99aa},
      {0x11223344,0x55667788,0x99aabbcc},
      {0x22114433,0x66558877,0xaa99ccbb}}};
  for (uint32_t width : {2u,4u}) {
    for (uint32_t endian=0; endian<4; ++endian) {
      const Cache::Key key{address,3,width,endian};
      const auto first = Convert(cache,key,watch,memory,ring);
      CHECK(!first.hit);
      const auto before = ring.head;
      const auto second = Convert(cache,key,watch,memory,ring);
      CHECK(second.hit && second.offset == first.offset && ring.head == before);
      for (size_t i=0; i<3; ++i) {
        uint32_t value;
        if (width == 4) {
          std::memcpy(&value,ring.bytes.data()+first.offset+i*4,4);
          CHECK(value == expected[endian][i]);
        } else {
          uint16_t short_value;
          std::memcpy(&short_value,ring.bytes.data()+first.offset+i*2,2);
          value = short_value;
          const std::array<uint16_t,3> raw{0x2211,0x4433,0x6655};
          const std::array<uint16_t,3> swapped{0x1122,0x3344,0x5566};
          CHECK(value == (endian ? swapped[i] : raw[i]));
        }
      }
      CHECK(first.range.first == (width == 4 ? expected[endian][0] : endian ? 0x1122u : 0x2211u));
      CHECK(first.range.last == (width == 4 ? expected[endian][2] : endian ? 0x5566u : 0x6655u));
    }
  }
  CHECK(cache.size() == 8);
  // Count and physical start are also part of identity, even when a token's
  // physical page and source prefix happen to match.
  CHECK(!Convert(cache,{address,2,2,0},watch,memory,ring).hit);
  CHECK(!Convert(cache,{address+2,2,2,0},watch,memory,ring).hit);
}

void MutationsPinsAndBatches() {
  Memory memory;
  Watch watch(memory.bases[0],Memory::kSize,memory.aliases);
  CHECK(watch.Enable());
  Cache cache;
  Ring ring;
  const auto token = [&](uint32_t page) { return watch.Token(page); };
  constexpr uint32_t address = Watch::kPageSize-2;
  const Cache::Key key{address,3,2,0};
  const std::array<uint16_t,3> original{1,5,9};
  std::memcpy(memory.bases[0]+address,original.data(),sizeof(original));
  const auto first = Convert(cache,key,watch,memory,ring);
  CHECK(!first.hit && cache.size() == 1 && cache.token_count() == 2);
  std::array<uint8_t,6> immutable;
  std::memcpy(immutable.data(),ring.bytes.data()+first.offset,immutable.size());
  for (size_t alias=0; alias<memory.bases.size(); ++alias) {
    const uint16_t value = uint16_t(20+alias);
    std::memcpy(memory.bases[alias]+address,&value,sizeof(value));
    const auto changed = Convert(cache,key,watch,memory,ring);
    CHECK(!changed.hit && changed.offset != first.offset && changed.range.last == value);
    CHECK(Convert(cache,key,watch,memory,ring).hit);
    CHECK(std::memcmp(immutable.data(),ring.bytes.data()+first.offset,immutable.size()) == 0);
  }
  {
    xn::GuestWriteScope pin(memory.bases[1]+address,6);
    CHECK(!cache.Find(key,token));
    CHECK(cache.size() == 0 && cache.token_count() == 0);
    const uint16_t value = 33;
    std::memcpy(memory.bases[1]+address,&value,sizeof(value));
    const auto untracked = Convert(cache,key,watch,memory,ring);
    CHECK(!untracked.hit && untracked.range.last == value && cache.size() == 0);
    CHECK(!Convert(cache,key,watch,memory,ring).hit);
  }
  CHECK(!Convert(cache,key,watch,memory,ring).hit);
  CHECK(Convert(cache,key,watch,memory,ring).hit);

  // A write between the pre/post conversion checks refuses admission. The
  // next draw copies again, while the previous draw's snapshot stays intact.
  cache.Reset();
  std::vector<uint64_t> before;
  CHECK(cache.SnapshotTokens(key,before,token));
  const uint16_t later = 44;
  std::memcpy(memory.bases[2]+address,&later,sizeof(later));
  CHECK(!cache.Store(key,100,{5,33},std::move(before),token));
  CHECK(cache.size() == 0 && !Convert(cache,key,watch,memory,ring).hit);
  CHECK(Convert(cache,key,watch,memory,ring).hit);

  // Ring segments are reused by command batches, including batches inside a
  // guest frame. Reset's generation invalidates any preflight hit pointer.
  const auto generation = cache.generation();
  cache.Reset();
  CHECK(cache.generation() != generation && cache.size() == 0 && cache.token_count() == 0);
  ring.head = 0;
  const auto next_batch = Convert(cache,key,watch,memory,ring);
  CHECK(!next_batch.hit && next_batch.offset == 0 && next_batch.range.last == later);
}

void PerDrawVertexRemapping() {
  Memory memory;
  Watch watch(memory.bases[0],Memory::kSize,memory.aliases);
  CHECK(watch.Enable());
  Cache cache;
  Ring ring;
  const std::array<uint16_t,3> input{1,5,9};
  std::memcpy(memory.bases[0]+64,input.data(),sizeof(input));
  const Cache::Key key{64,3,2,0};
  CHECK(!Convert(cache,key,watch,memory,ring).hit);
  const auto reused = Convert(cache,key,watch,memory,ring);
  CHECK(reused.hit && reused.range.first == 1 && reused.range.last == 9);
  auto remapped = xn::gfx::RemapVertexIndices(reused.range,8,10,15);
  CHECK(remapped.first == 10 && remapped.last == 15);
  remapped = xn::gfx::RemapVertexIndices(reused.range,2,0,100);
  CHECK(remapped.first == 3 && remapped.last == 11);
  remapped = xn::gfx::RemapVertexIndices(reused.range,0xffffff,0,0xffffff);
  CHECK(remapped.first == 0 && remapped.last == 8);
  CHECK(Convert(cache,key,watch,memory,ring).offset == reused.offset);
}

void AdmissionLimits() {
  Cache cache;
  const auto token = [](uint32_t) { return uint64_t{1}; };
  std::vector<uint64_t> tokens;
  const Cache::Key invalid{0,1,1,0};
  CHECK(!cache.SnapshotTokens(invalid,tokens,token));
  CHECK(!cache.SnapshotTokens({uint32_t(Cache::kPhysicalSize)-2,1,4,0},tokens,token));
  CHECK(!cache.SnapshotTokens({0,uint32_t((Cache::kMaxTokens+1)*Cache::kPageSize/4),4,0},tokens,token));
  for (uint32_t i=0; i<Cache::kMaxEntries; ++i) {
    const Cache::Key key{i*2,1,2,0};
    CHECK(cache.SnapshotTokens(key,tokens,token));
    CHECK(cache.Store(key,i*2,{i,i},std::move(tokens),token));
  }
  CHECK(cache.size() == Cache::kMaxEntries && cache.token_count() == Cache::kMaxEntries);
  CHECK(!cache.SnapshotTokens({100000,1,2,0},tokens,token));
  cache.Reset();
  const Cache::Key maximum{0,uint32_t(Cache::kMaxTokens*Cache::kPageSize/4),4,0};
  CHECK(cache.SnapshotTokens(maximum,tokens,token));
  CHECK(cache.Store(maximum,0,{0,1},std::move(tokens),token));
  CHECK(cache.token_count() == Cache::kMaxTokens);
  CHECK(!cache.SnapshotTokens({0,1,2,0},tokens,token));
  const auto zero = [](uint32_t) { return uint64_t{0}; };
  CHECK(!cache.Find(maximum,zero));
  CHECK(cache.token_count() == 0 && cache.size() == 0);
}
} // namespace

int main() {
  try {
    EndianAndKeySeparation();
    MutationsPinsAndBatches();
    PerDrawVertexRemapping();
    AdmissionLimits();
    std::puts("DMA index cache probe passed: endian/width keys, alias mutations, pinned writes, admission races, batch reset, per-draw bounds, caps");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr,"DMA index cache probe failed: %s\n",error.what());
    return 1;
  }
}
