#include "runtime/gfx/vulkan/edram_layout.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
void Check(bool condition, int line) {
  if (!condition) {
    std::fprintf(stderr, "EDRAM layout probe failed at line %d\n", line);
    std::exit(1);
  }
}
#define CHECK(condition) Check(bool(condition), __LINE__)
}  // namespace

int main() {
  using namespace xn::gfx;
  CHECK(kEdramTileBytes == 5120);
  CHECK(VirtualEdramBase(0x800, 2048) == 0);
  CHECK(VirtualEdramBase(0x800, 4096) == 2048);
  CHECK(VirtualEdramBase(0x800, 8192) == 2048);
  CHECK(VirtualEdramBase(0xABC00FFF, 2048) == 2047);
  CHECK(VirtualEdramBase(0xABC00FFF, 4096) == 4095);
  CHECK(VirtualEdramBase(0xABC00FFF, 8192) == 4095);
  CHECK(VirtualEdramBase(0x1000, 8192) == 0);
  CHECK(uint64_t(kDefaultEdramTileCapacity) * kEdramTileBytes == (10ull << 20));
  CHECK(uint64_t(VirtualEdramTileCapacity(20)) * kEdramTileBytes == (20ull << 20));
  CHECK(uint64_t(kMaxVirtualEdramTileCapacity) * kEdramTileBytes == (40ull << 20));

  CHECK(SelectVirtualEdramMiB(10, nullptr) == 10);
  CHECK(SelectVirtualEdramMiB(20, nullptr) == 20);
  CHECK(SelectVirtualEdramMiB(40, nullptr) == 40);
  CHECK(SelectVirtualEdramMiB(30, nullptr) == 10);
  CHECK(SelectVirtualEdramMiB(20, "0") == 10);
  CHECK(SelectVirtualEdramMiB(10, "1") == 20);
  CHECK(SelectVirtualEdramMiB(10, "40") == 40);
  CHECK(SelectVirtualEdramMiB(40, "0") == 10);
  CHECK(SelectVirtualEdramMiB(40, "1") == 20);
  CHECK(SelectVirtualEdramMiB(10, "040") == 10);
  CHECK(SelectVirtualEdramMiB(20, "01") == 20);
  CHECK(SelectVirtualEdramMiB(10, "true") == 10);
  CHECK(VirtualEdramTileCapacity(10) == 2048);
  CHECK(VirtualEdramTileCapacity(20) == 4096);
  CHECK(VirtualEdramTileCapacity(40) == 8192);
  CHECK(VirtualEdramTileCapacity(30) == 2048);

  for (uint32_t capacity : {kDefaultEdramTileCapacity, uint32_t(4096),
                            kMaxVirtualEdramTileCapacity}) {
    CHECK(EdramTileRangeFits(0, 80, 16, capacity));
    CHECK(EdramTileRangeFits(capacity - 1, 80, 16, capacity));
    CHECK(!EdramTileRangeFits(capacity, 80, 16, capacity));
    CHECK(!EdramTileRangeFits(UINT32_MAX, 80, 16, capacity));
    CHECK(!EdramTileRangeFits(capacity - 1, 80, 17, capacity));
    CHECK(!EdramTileRangeFits(capacity - 1, 81, 16, capacity));
    CHECK(EdramTileRangeFits(capacity - 4, 81, 17, capacity));
    CHECK(!EdramTileRangeFits(capacity - 3, 81, 17, capacity));
    CHECK(!EdramTileRangeFits(0, 0, 16, capacity));
    CHECK(!EdramTileRangeFits(0, 80, 0, capacity));
    CHECK(!EdramTileRangeFits(0, UINT32_MAX, UINT32_MAX, capacity));
    CHECK(!EdramTileRangeFits(0, UINT32_MAX, 1, capacity));
    CHECK(!EdramTileRangeFits(0, 80, UINT32_MAX, capacity));
  }
  CHECK(!EdramTileRangeFits(0, 80, 16, 0));
  CHECK(!EdramTileRangeFits(2047, 80, 32, 2048));
  CHECK(EdramTileRangeFits(2047, 80, 32, 4096));
  CHECK(EdramTileRangeFits(2048, 80, 16, 4096));
  CHECK(EdramTileRangeFits(4095, 80, 16, 4096));
  CHECK(!EdramTileRangeFits(4095, 80, 32, 4096));
  CHECK(EdramTileRangeFits(4095, 80, 32, 8192));
  CHECK(EdramTileRangeFits(4095, 80, 4097 * 16, 8192));
  CHECK(!EdramTileRangeFits(4095, 80, 4098 * 16, 8192));
  std::puts("PASS: virtual EDRAM selection and bounded tile ranges");
}
