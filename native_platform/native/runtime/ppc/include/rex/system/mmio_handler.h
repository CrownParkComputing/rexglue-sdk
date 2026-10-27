// xenon-native: MMIO seam for REX_MM_LOAD/STORE (guest addresses 0x7F000000-0x7FFFFFFF).
// The GPU (and anything else with registers) installs a range; unclaimed reads return 0.
#pragma once
#include <cstdint>
#include <vector>
namespace rex::runtime {
using MMIORead = uint32_t (*)(void* context, uint32_t addr);
using MMIOWrite = void (*)(void* context, uint32_t addr, uint32_t value);
class MMIOHandler {
 public:
  static MMIOHandler* global_handler();
  void RegisterRange(uint32_t address, uint32_t mask, uint32_t size, void* context, MMIORead read,
                     MMIOWrite write);
  bool CheckLoad(uint32_t address, uint32_t* out_value);
  bool CheckStore(uint32_t address, uint32_t value);
 private:
  struct Range { uint32_t address, mask, size; void* context; MMIORead read; MMIOWrite write; };
  std::vector<Range> ranges_;
};
}  // namespace rex::runtime
