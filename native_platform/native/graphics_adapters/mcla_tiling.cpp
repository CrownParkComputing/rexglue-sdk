#include "mcla_tiling_policy.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <time.h>

namespace {
constexpr uint32_t kTileCount = 0x827D42A4;
struct State {
  bool enabled = false, known_mapping = false;
  uint32_t capacity = 2048;
  FILE* trace = nullptr;
  FILE* allocations = nullptr;
  std::mutex mutex;
  std::atomic<uint64_t> main_calls{0}, offscreen_calls{0}, original_tiles{0}, selected_tiles{0}, applied{0}, extended_surfaces{0};
};
State state;
uint32_t Read(uint8_t* base, uint32_t address) {
  uint32_t value; std::memcpy(&value, base + address, 4); return __builtin_bswap32(value);
}
bool SceneTarget(uint8_t* base, uint32_t target, uint32_t& slot0, uint32_t& slot1) {
  const uint32_t owner = Read(base, 0x8287E064);
  slot0 = slot1 = 0;
  if (owner && owner < UINT32_MAX - 400) {
    slot0 = Read(base, owner + 396); slot1 = Read(base, owner + 400);
  }
  return !target || (target && (target == slot0 || target == slot1));
}
uint64_t Clock() {
  timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000000000 + uint64_t(t.tv_nsec);
}
}
void MclaNativeConfigureTiling(uint8_t* base, uint32_t capacity, bool default_enabled) {
  // Validate original mapped instruction words independently of codegen's
  // strict source anchors. Other game revisions retain the normal path.
  const bool signature = base && Read(base, 0x8217A700) == 0x3967009F &&
      Read(base, 0x8217A704) == 0x3948001F && Read(base, 0x82410E34) == 0x2B0B0800;
  const char* value = std::getenv("XN_MCLA_SINGLE_TILE");
  const bool requested = value ? !std::strcmp(value, "1") : default_enabled;
  state.capacity = capacity;
  state.known_mapping = signature;
  state.enabled = requested && (capacity == 4096 || capacity == 8192) && signature;
  if (const char* path = std::getenv("XN_MCLA_TILE_TRACE"); path && *path) {
    state.trace = std::fopen(path, "w");
    const auto alloc_path = std::string(path) + ".alloc.csv";
    state.allocations = std::fopen(alloc_path.c_str(), "w");
    if (state.trace) { std::setvbuf(state.trace, nullptr, _IOLBF, 0); std::fprintf(state.trace,"monotonic_ns,target,width,height,mode,tile_width,tile_height,original_count,selected_count,applied,required_tiles,capacity,scene_target,scene_slot0,scene_slot1\n"); }
    if (state.allocations) { std::setvbuf(state.allocations, nullptr, _IOLBF, 0); std::fprintf(state.allocations,"monotonic_ns,base,size,end,extended,capacity\n"); }
  }
  std::fprintf(stderr,"mcla-tiling: requested=%u enabled=%u mapped_signature=%u capacity=%u\n", requested, state.enabled, signature, capacity);
}
bool MclaNativePrepareTile(uint32_t target, uint32_t width, uint32_t height,
    uint32_t mode, uint32_t tile_width, uint32_t tile_height, uint8_t* base) {
  if (!state.known_mapping || (!state.enabled && !state.trace)) return false;
  const uint32_t original = Read(base, kTileCount);
  uint32_t slot0, slot1;
  const bool scene = SceneTarget(base, target, slot0, slot1);
  const bool apply = mcla::native::UseSingleTile(state.enabled, state.capacity,
      scene, width, height, mode, original);
  if (apply) { const uint32_t one = __builtin_bswap32(1u); std::memcpy(base + kTileCount, &one, 4); }
  const uint32_t selected = apply ? 1 : original;
  if (scene) {
    ++state.main_calls; state.original_tiles += original; state.selected_tiles += selected;
    if (apply) ++state.applied;
  } else ++state.offscreen_calls;
  if (state.trace) {
    std::lock_guard lock(state.mutex);
    std::fprintf(state.trace,"%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", (unsigned long long)Clock(),target,width,height,mode,tile_width,tile_height,original,selected,apply,mcla::native::MainSceneTileDemand(width,height,mode),state.capacity,scene,slot0,slot1);
  }
  return apply;
}
bool MclaNativeAllowSurface(uint32_t base, uint32_t size, uint32_t end) {
  if (!state.enabled && !state.allocations) return false;
  const bool allow = state.enabled && mcla::native::SurfaceFits(state.capacity,base,size,end);
  if (allow && end > 2048) ++state.extended_surfaces;
  if (state.allocations) {
    std::lock_guard lock(state.mutex);
    std::fprintf(state.allocations,"%llu,%u,%u,%u,%u,%u\n",(unsigned long long)Clock(),base,size,end,allow && end>2048,state.capacity);
  }
  return allow;
}
void MclaNativeReportTiling() {
  std::fprintf(stderr,"mcla-tiling: main=%llu offscreen=%llu original_tiles=%llu selected_tiles=%llu single=%llu extended_surfaces=%llu\n",
      (unsigned long long)state.main_calls.load(), (unsigned long long)state.offscreen_calls.load(),
      (unsigned long long)state.original_tiles.load(), (unsigned long long)state.selected_tiles.load(),
      (unsigned long long)state.applied.load(), (unsigned long long)state.extended_surfaces.load());
  std::lock_guard lock(state.mutex);
  if (state.trace) std::fflush(state.trace);
  if (state.allocations) std::fflush(state.allocations);
}
