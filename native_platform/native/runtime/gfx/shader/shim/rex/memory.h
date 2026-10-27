// xenon-native shim for <rex/memory.h>: only what the translator and the
// Xenos headers use (Reinterpret, copy_and_swap, fourcc). No arenas, no
// mapped memory, no page tables.
#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/types.h>
#define REX_RESTRICT_VAR __restrict
namespace rex::memory {
template <typename Dst, typename Src>
void Reinterpret(Dst& REX_RESTRICT_VAR dst, const Src& REX_RESTRICT_VAR src) {
  static_assert(sizeof(Dst) == sizeof(Src));
  std::memcpy(&dst, &src, sizeof(Dst));
}
template <typename Dst, typename Src>
Dst Reinterpret(const Src& REX_RESTRICT_VAR src) {
  Dst dst;
  Reinterpret(dst, src);
  return dst;
}
template <typename T>
void copy_and_swap(T* dest, const T* src, size_t count) {
  if constexpr (sizeof(T) == 1) {
    std::memcpy(dest, src, count);
  } else if constexpr (sizeof(T) == 2) {
    for (size_t i = 0; i < count; ++i) {
      uint16_t v;
      std::memcpy(&v, src + i, 2);
      v = uint16_t((v >> 8) | (v << 8));
      std::memcpy(dest + i, &v, 2);
    }
  } else if constexpr (sizeof(T) == 4) {
    for (size_t i = 0; i < count; ++i) {
      uint32_t v;
      std::memcpy(&v, src + i, 4);
      v = __builtin_bswap32(v);
      std::memcpy(dest + i, &v, 4);
    }
  } else if constexpr (sizeof(T) == 8) {
    for (size_t i = 0; i < count; ++i) {
      uint64_t v;
      std::memcpy(&v, src + i, 8);
      v = __builtin_bswap64(v);
      std::memcpy(dest + i, &v, 8);
    }
  } else {
    static_assert(sizeof(T) <= 8, "Invalid memory::copy_and_swap size");
  }
}
template <typename T>
inline T load(const void* mem) {
  T result;
  std::memcpy(&result, mem, sizeof(T));
  return result;
}
template <typename T>
inline void store(void* mem, const T& value) {
  std::memcpy(mem, &value, sizeof(T));
}
using fourcc_t = uint32_t;
constexpr fourcc_t make_fourcc(char a, char b, char c, char d) {
  return (fourcc_t(uint8_t(a)) << 24) | (fourcc_t(uint8_t(b)) << 16) | (fourcc_t(uint8_t(c)) << 8) |
         fourcc_t(uint8_t(d));
}
constexpr fourcc_t make_fourcc(const char (&s)[5]) { return make_fourcc(s[0], s[1], s[2], s[3]); }
}  // namespace rex::memory

namespace rex::memory { class Memory; }
