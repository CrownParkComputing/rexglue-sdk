#include "forza4_tiling_policy.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <time.h>

namespace {

constexpr uint32_t kSignature0 = 0x828837A0;
constexpr uint32_t kSignature1 = 0x828837B0;
constexpr uint32_t kSignature2 = 0x828837B4;
constexpr uint32_t kExpected0 = 0x7D8802A6;
constexpr uint32_t kExpected1 = 0x1D4404D0;
constexpr uint32_t kExpected2 = 0x396B83C0;

constexpr uint32_t kWidth = 4;
constexpr uint32_t kHeight = 8;
constexpr uint32_t kMode = 12;
constexpr uint32_t kCount = 16;
constexpr uint32_t kRect0 = 32;
constexpr uint32_t kRect1 = 48;

struct State {
  uint8_t* base = nullptr;
  uint32_t capacity = 0;
  bool enabled = false;
  bool signature = false;
  FILE* trace = nullptr;
  std::mutex trace_mutex;
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> applied{0};
  std::atomic<uint64_t> rejected{0};
  std::atomic<uint64_t> already{0};
};
State state;

uint32_t Read(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

void Write(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

bool SignatureMatches(uint8_t* base) {
  return base && Read(base, kSignature0) == kExpected0 &&
         Read(base, kSignature1) == kExpected1 &&
         Read(base, kSignature2) == kExpected2;
}

uint64_t MonotonicNanoseconds() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000000000 + uint64_t(t.tv_nsec);
}

struct Scene {
  uint32_t word0, word20, word24, word28;
  uint32_t width, height, mode, count;
  uint32_t x0, y0, x1, y1;
  uint32_t next_x0, next_y0, next_x1, next_y1;
};

Scene ReadScene(uint8_t* base, uint32_t address) {
  return {Read(base, address), Read(base, address + 20),
          Read(base, address + 24), Read(base, address + 28),
          Read(base, address + kWidth), Read(base, address + kHeight),
          Read(base, address + kMode), Read(base, address + kCount),
          Read(base, address + kRect0), Read(base, address + kRect0 + 4),
          Read(base, address + kRect0 + 8), Read(base, address + kRect0 + 12),
          Read(base, address + kRect1), Read(base, address + kRect1 + 4),
          Read(base, address + kRect1 + 8), Read(base, address + kRect1 + 12)};
}

void Trace(uint32_t index, const Scene& s, const char* decision) {
  if (!state.trace) return;
  std::lock_guard lock(state.trace_mutex);
  std::fprintf(state.trace,
      "%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s\n",
      static_cast<unsigned long long>(MonotonicNanoseconds()), index,
      state.capacity, s.word0, s.width, s.height, s.mode, s.count,
      s.word20, s.word24, s.word28,
      s.x0, s.y0, s.x1, s.y1,
      s.next_x0, s.next_y0, s.next_x1, s.next_y1, decision);
}

}  // namespace

void Forza4NativeConfigureTiling(uint8_t* base, uint32_t capacity,
                                 bool default_enabled) {
  // The signature is checked at configuration and again at use. An unknown
  // executable revision keeps its unmodified guest table.
  const bool signature = SignatureMatches(base);
  const char* override_value = std::getenv("XN_FORZA4_SINGLE_TILE");
  bool requested = default_enabled;
  if (override_value && !std::strcmp(override_value, "0")) requested = false;
  if (override_value && !std::strcmp(override_value, "1")) requested = true;
  state.base = base;
  state.capacity = capacity;
  state.signature = signature;
  state.enabled = requested && signature && (capacity == 4096 || capacity == 8192);
  state.calls = 0;
  state.applied = 0;
  state.rejected = 0;
  state.already = 0;
  {
    std::lock_guard lock(state.trace_mutex);
    if (state.trace) {
      std::fclose(state.trace);
      state.trace = nullptr;
    }
    if (const char* path = std::getenv("XN_FORZA4_SCENE_TRACE"); path && *path) {
      state.trace = std::fopen(path, "w");
      if (state.trace) {
        std::setvbuf(state.trace, nullptr, _IOLBF, 0);
        std::fprintf(state.trace,
            "monotonic_ns,index,capacity,word0,width,height,mode,count,"
            "word20,word24,word28,"
            "rect0_x0,rect0_y0,rect0_x1,rect0_y1,"
            "rect1_x0,rect1_y0,rect1_x1,rect1_y1,decision\n");
      }
    }
  }
  std::fprintf(stderr,
      "forza4-tiling: requested=%u enabled=%u mapped_signature=%u capacity=%u\n",
      requested, state.enabled, signature, capacity);
}

extern "C" void Forza4NativePrepareTiling(uint32_t index, uint8_t* base) {
  if (!state.signature || (!state.enabled && !state.trace)) return;
  if (!base || base != state.base || index >= forza4::native::kSceneTableEntries ||
      !SignatureMatches(base)) {
    ++state.rejected;
    return;
  }
  const uint32_t address = forza4::native::kSceneTableAddress +
                           index * forza4::native::kSceneTableStride;
  const Scene scene = ReadScene(base, address);
  ++state.calls;
  const bool eligible = scene.word20 == 1 && scene.word24 == 2 &&
      forza4::native::EligibleScene(
      state.enabled, state.capacity, index, scene.width, scene.height,
      scene.mode, scene.count, scene.x0, scene.y0, scene.x1, scene.y1,
      scene.next_x0, scene.next_y0, scene.next_x1, scene.next_y1);
  if (eligible) {
    // The title reads this same table for drawing and resolving. Preserve
    // the observed one-column grid's row/count agreement as well. The old
    // second rectangle and every unrelated field remain intact.
    Write(base, address + kRect0 + 12, 720);
    Write(base, address + 24, 1);
    Write(base, address + kCount, 1);
    ++state.applied;
    Trace(index, scene, "applied");
    return;
  }
  if (state.enabled && scene.width == 1280 && scene.height == 720 && scene.mode == 1 &&
      scene.count == 1 && scene.word20 == 1 && scene.word24 == 1 &&
      scene.x0 == 0 && scene.y0 == 0 &&
      scene.x1 == 1280 && scene.y1 == 720 &&
      scene.next_x0 == 0 && scene.next_y0 == 512 &&
      scene.next_x1 == 1280 && scene.next_y1 == 720) {
    ++state.already;
    Trace(index, scene, "already");
    return;
  }
  ++state.rejected;
  Trace(index, scene, "fallback");
}

void Forza4NativeReportTiling() {
  std::fprintf(stderr,
      "forza4-tiling: calls=%llu applied=%llu already=%llu fallback=%llu\n",
      static_cast<unsigned long long>(state.calls.load()),
      static_cast<unsigned long long>(state.applied.load()),
      static_cast<unsigned long long>(state.already.load()),
      static_cast<unsigned long long>(state.rejected.load()));
  std::lock_guard lock(state.trace_mutex);
  if (state.trace) std::fflush(state.trace);
}
