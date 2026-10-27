#include "graphics_adapters/forza4_tiling_policy.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr size_t kMappedBytes = 0x83000000ull;
constexpr uint32_t kEntry = forza4::native::kSceneTableAddress;
constexpr size_t kSnapshotBytes = 96;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "forza4 tiling probe: %s\n", message);
  std::abort();
}

void Write(uint8_t* memory, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(memory + address, &value, sizeof(value));
}

uint32_t Read(uint8_t* memory, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, memory + address, sizeof(value));
  return __builtin_bswap32(value);
}

using Snapshot = std::array<uint8_t, kSnapshotBytes>;
Snapshot Take(uint8_t* memory, uint32_t address = kEntry) {
  Snapshot result;
  std::memcpy(result.data(), memory + address, result.size());
  return result;
}

void Seed(uint8_t* memory, uint32_t address = kEntry) {
  std::memset(memory + address, 0xA5, kSnapshotBytes);
  Write(memory, address + 4, 1280);
  Write(memory, address + 8, 720);
  Write(memory, address + 12, 1);
  Write(memory, address + 16, 2);
  Write(memory, address + 20, 1);
  Write(memory, address + 24, 2);
  Write(memory, address + 32, 0);
  Write(memory, address + 36, 0);
  Write(memory, address + 40, 1280);
  Write(memory, address + 44, 512);
  Write(memory, address + 48, 0);
  Write(memory, address + 52, 512);
  Write(memory, address + 56, 1280);
  Write(memory, address + 60, 720);
}

void ExpectFallback(uint8_t* memory, const char* message, uint32_t index = 0) {
  const Snapshot before = Take(memory);
  Forza4NativePrepareTiling(index, memory);
  Check(before == Take(memory), message);
}
}  // namespace

int main() {
  using forza4::native::EligibleScene;
  static_assert(EligibleScene(true, 4096, 0, 1280, 720, 1, 2,
                              0, 0, 1280, 512, 0, 512, 1280, 720));
  static_assert(EligibleScene(true, 8192, 4, 1280, 720, 1, 2,
                              0, 0, 1280, 512, 0, 512, 1280, 720));
  static_assert(!EligibleScene(true, 2048, 0, 1280, 720, 1, 2,
                               0, 0, 1280, 512, 0, 512, 1280, 720));
  static_assert(!EligibleScene(true, 4096, 5, 1280, 720, 1, 2,
                               0, 0, 1280, 512, 0, 512, 1280, 720));
  static_assert(!EligibleScene(true, 4096, 0, 1280, 720, 1, 3,
                               0, 0, 1280, 512, 0, 512, 1280, 720));

  auto* memory = static_cast<uint8_t*>(mmap(nullptr, kMappedBytes,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  Check(memory != MAP_FAILED, "mmap failed");
  Write(memory, 0x828837A0, 0x7D8802A6);
  Write(memory, 0x828837B0, 0x1D4404D0);
  Write(memory, 0x828837B4, 0x396B83C0);
  Seed(memory);

  unsetenv("XN_FORZA4_SINGLE_TILE");
  unsetenv("XN_FORZA4_SCENE_TRACE");
  Forza4NativeConfigureTiling(memory, 4096);
  ExpectFallback(memory, "disabled by default changed the table");

  setenv("XN_FORZA4_SINGLE_TILE", "0", 1);
  Forza4NativeConfigureTiling(memory, 4096, true);
  ExpectFallback(memory, "explicit off changed the table");

  setenv("XN_FORZA4_SINGLE_TILE", "1", 1);
  Forza4NativeConfigureTiling(memory, 2048);
  ExpectFallback(memory, "insufficient capacity changed the table");
  Forza4NativeConfigureTiling(memory, 4096);
  ExpectFallback(memory, "out-of-range index changed the table", 5);
  ExpectFallback(memory, "overflowing index changed the table", UINT32_MAX);
  Forza4NativePrepareTiling(0, nullptr);
  Check(Read(memory, kEntry + 16) == 2, "null base changed the table");

  Write(memory, kEntry + 4, 1279);
  ExpectFallback(memory, "wrong width changed the table");
  Write(memory, kEntry + 4, 1280);
  Write(memory, kEntry + 8, 719);
  ExpectFallback(memory, "wrong height changed the table");
  Write(memory, kEntry + 8, 720);
  Write(memory, kEntry + 12, 2);
  ExpectFallback(memory, "wrong mode changed the table");
  Write(memory, kEntry + 12, 1);
  Write(memory, kEntry + 16, 3);
  ExpectFallback(memory, "wrong count changed the table");
  Write(memory, kEntry + 16, 2);
  Write(memory, kEntry + 44, 511);
  ExpectFallback(memory, "wrong first rectangle changed the table");
  Write(memory, kEntry + 44, 512);
  Write(memory, kEntry + 52, 511);
  ExpectFallback(memory, "wrong second rectangle changed the table");
  Write(memory, kEntry + 52, 512);
  Write(memory, kEntry + 20, 2);
  ExpectFallback(memory, "different grid columns changed the table");
  Write(memory, kEntry + 20, 1);
  Write(memory, kEntry + 24, 3);
  ExpectFallback(memory, "different grid rows changed the table");
  Write(memory, kEntry + 24, 2);

  // A valid-looking entry outside the known 0..4 table range must not be read.
  Seed(memory, kEntry + 5 * forza4::native::kSceneTableStride);
  const Snapshot outside = Take(memory, kEntry + 5 * forza4::native::kSceneTableStride);
  Forza4NativePrepareTiling(5, memory);
  Check(outside == Take(memory, kEntry + 5 * forza4::native::kSceneTableStride),
        "outside entry changed");

  // A different in-range scene is left byte-for-byte intact.
  const uint32_t other = kEntry + forza4::native::kSceneTableStride;
  Seed(memory, other);
  Write(memory, other + 4, 640);
  const Snapshot other_before = Take(memory, other);
  Forza4NativePrepareTiling(1, memory);
  Check(other_before == Take(memory, other), "other scene changed");

  const Snapshot before = Take(memory);
  Forza4NativePrepareTiling(0, memory);
  Check(Read(memory, kEntry + 16) == 1, "count was not collapsed");
  Check(Read(memory, kEntry + 24) == 1, "row count was not collapsed");
  Check(Read(memory, kEntry + 44) == 720, "first rectangle was not extended");
  const Snapshot after = Take(memory);
  for (size_t i = 0; i < before.size(); ++i) {
    if ((i >= 16 && i < 20) || (i >= 24 && i < 28) || (i >= 44 && i < 48)) continue;
    Check(before[i] == after[i], "successful rewrite changed another byte");
  }
  Check(other_before == Take(memory, other), "rewrite changed another entry");
  Forza4NativePrepareTiling(0, memory);
  Check(after == Take(memory), "second call changed an already collapsed entry");

  // A changed executable signature must retain the original path even if it
  // changes after initial configuration.
  Seed(memory);
  Write(memory, 0x828837B4, 0);
  ExpectFallback(memory, "live signature change modified the table");
  Forza4NativeConfigureTiling(memory, 4096);
  ExpectFallback(memory, "unknown signature modified the table");
  Write(memory, 0x828837B4, 0x396B83C0);

  // Check the alternate supported capacity and default-enabled launch.
  unsetenv("XN_FORZA4_SINGLE_TILE");
  Seed(memory);
  Forza4NativeConfigureTiling(memory, 8192, true);
  Forza4NativePrepareTiling(0, memory);
  Check(Read(memory, kEntry + 16) == 1 && Read(memory, kEntry + 44) == 720,
        "8192-tile default-enabled launch failed");
  Forza4NativeReportTiling();

  auto trace_path = (std::filesystem::temp_directory_path() / "forza4-scene-trace-XXXXXX").string();
  const int trace_fd = mkstemp(trace_path.data());
  Check(trace_fd >= 0, "mkstemp failed");
  Check(close(trace_fd) == 0, "close trace placeholder failed");
  setenv("XN_FORZA4_SCENE_TRACE", trace_path.c_str(), 1);
  setenv("XN_FORZA4_SINGLE_TILE", "0", 1);
  Seed(memory);
  Forza4NativeConfigureTiling(memory, 4096);
  ExpectFallback(memory, "trace-only launch changed the table");
  Forza4NativeReportTiling();
  FILE* trace = std::fopen(trace_path.c_str(), "r");
  Check(trace != nullptr, "trace CSV missing");
  char line[512];
  Check(std::fgets(line, sizeof(line), trace) != nullptr &&
            std::strstr(line, "rect0_x0") != nullptr, "trace CSV header missing");
  Check(std::fgets(line, sizeof(line), trace) != nullptr &&
            std::strstr(line, ",fallback") != nullptr, "trace CSV row missing");
  Check(std::fclose(trace) == 0, "close trace CSV failed");
  unsetenv("XN_FORZA4_SCENE_TRACE");
  Forza4NativeConfigureTiling(memory, 4096);
  Check(unlink(trace_path.c_str()) == 0, "unlink trace CSV failed");
  Check(munmap(memory, kMappedBytes) == 0, "munmap failed");
  return 0;
}
