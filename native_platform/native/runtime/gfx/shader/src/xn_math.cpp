// xenon-native: the out-of-line bit helpers <rex/math.h> declares.
#include <bit>
#include <cstdint>
#include <rex/math.h>
namespace rex {
uint8_t lzcnt(uint8_t v) { return uint8_t(std::countl_zero(v)); }
uint8_t lzcnt(uint16_t v) { return uint8_t(std::countl_zero(v)); }
uint8_t lzcnt(uint32_t v) { return uint8_t(std::countl_zero(v)); }
uint8_t lzcnt(uint64_t v) { return uint8_t(std::countl_zero(v)); }
uint8_t tzcnt(uint8_t v) { return uint8_t(std::countr_zero(v)); }
uint8_t tzcnt(uint16_t v) { return uint8_t(std::countr_zero(v)); }
uint8_t tzcnt(uint32_t v) { return uint8_t(std::countr_zero(v)); }
uint8_t tzcnt(uint64_t v) { return uint8_t(std::countr_zero(v)); }
bool bit_scan_forward(uint32_t v, uint32_t* out_first_set_index) {
  if (!v) return false;
  *out_first_set_index = uint32_t(std::countr_zero(v));
  return true;
}
bool bit_scan_forward(uint64_t v, uint32_t* out_first_set_index) {
  if (!v) return false;
  *out_first_set_index = uint32_t(std::countr_zero(v));
  return true;
}
}  // namespace rex
