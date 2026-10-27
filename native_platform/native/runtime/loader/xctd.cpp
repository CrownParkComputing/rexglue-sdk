#include "xctd.h"

#include <sys/types.h>

#include <cstdlib>
#include <cstring>

#include <mspack.h>
#include <lzx.h>

namespace xn {
namespace {

constexpr uint32_t kXctdMagic = 0x0FF512ED;

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t Be16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }

// mspack in-memory I/O, same pattern as the XEX loader's.
struct MemFile {
  const uint8_t* data;
  uint8_t* wdata;
  size_t size;
  size_t offset;
};
int MemRead(mspack_file* f, void* buffer, int bytes) {
  auto* m = reinterpret_cast<MemFile*>(f);
  size_t n = std::min<size_t>(size_t(bytes), m->size - m->offset);
  std::memcpy(buffer, m->data + m->offset, n);
  m->offset += n;
  return int(n);
}
int MemWrite(mspack_file* f, void* buffer, int bytes) {
  auto* m = reinterpret_cast<MemFile*>(f);
  size_t n = std::min<size_t>(size_t(bytes), m->size - m->offset);
  std::memcpy(m->wdata + m->offset, buffer, n);
  m->offset += n;
  return int(n);
}
void* MemAlloc(mspack_system*, size_t bytes) { return std::calloc(bytes, 1); }
void MemFree(void* p) { std::free(p); }
void MemCopy(void* src, void* dst, size_t bytes) { std::memcpy(dst, src, bytes); }

bool LzxDecompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len,
                   uint32_t window_bits) {
  mspack_system sys{};
  sys.read = MemRead;
  sys.write = MemWrite;
  sys.alloc = MemAlloc;
  sys.free = MemFree;
  sys.copy = MemCopy;
  MemFile in{src, nullptr, src_len, 0};
  MemFile out{nullptr, dst, dst_len, 0};
  lzxd_stream* lzx =
      lzxd_init(&sys, reinterpret_cast<mspack_file*>(&in),
                reinterpret_cast<mspack_file*>(&out), int(window_bits), 0, 0x8000,
                off_t(dst_len), 0);
  if (!lzx) return false;
  int rc = lzxd_decompress(lzx, off_t(dst_len));
  bool produced_all = out.offset == dst_len;
  lzxd_free(lzx);
  return rc == MSPACK_ERR_OK && produced_all;
}

}  // namespace

bool XctdIsCompressed(const void* data, size_t size) {
  return size >= 16 && Be32(reinterpret_cast<const uint8_t*>(data)) == kXctdMagic;
}

bool XctdDecompress(const void* data, size_t size, std::vector<uint8_t>& out) {
  const uint8_t* d = reinterpret_cast<const uint8_t*>(data);
  if (!XctdIsCompressed(data, size)) return false;
  const uint32_t flags = Be32(d + 12);
  const uint32_t segments = (flags >> 6) & 0xFFFF;
  static const uint32_t kBitsPerSize[4] = {20, 32, 0, 0};
  const uint32_t bits = kBitsPerSize[(flags >> 22) & 3];
  const uint64_t stride = uint64_t(0x8000) << ((flags >> 4) & 3);
  const uint32_t window_bits = (flags & 0xF) + 0xF;
  if (!segments || !bits || !stride) return false;
  const uint64_t table_words = (uint64_t(bits) * segments + 31) / 32;
  const uint64_t payload_start = 0x10 + table_words * 4;
  if (payload_start > size) return false;

  // MSB-first bit-packed uncompressed sizes (straight u32s when bits == 32).
  std::vector<uint32_t> unc(segments);
  const uint8_t* table = d + 0x10;
  uint64_t bitpos = 0;
  for (uint32_t i = 0; i < segments; i++) {
    uint32_t v = 0;
    for (uint32_t b = 0; b < bits; b++) {
      uint64_t byte = bitpos >> 3;
      v = (v << 1) | ((table[byte] >> (7 - (bitpos & 7))) & 1);
      bitpos++;
    }
    unc[i] = v;
  }

  uint64_t total = 0;
  for (uint32_t u : unc) total += u;
  if (total > uint64_t(2) * 1024 * 1024 * 1024) return false;  // sanity
  out.resize(total);

  std::vector<uint8_t> stream;
  stream.reserve(stride);
  uint64_t produced = 0;
  for (uint32_t i = 0; i < segments; i++) {
    uint64_t region_start = i == 0 ? payload_start : uint64_t(i) * stride;
    uint64_t region_end = std::min<uint64_t>((uint64_t(i) + 1) * stride, size);
    if (region_start >= region_end) return false;
    stream.clear();
    uint64_t o = region_start;
    while (o + 2 <= region_end) {
      uint32_t chunk = Be16(d + o);
      o += 2;
      if (!chunk) break;
      if (o + chunk > region_end) return false;
      stream.insert(stream.end(), d + o, d + o + chunk);
      o += chunk;
    }
    if (stream.empty() || !unc[i]) {
      if (unc[i]) return false;
      continue;
    }
    if (!LzxDecompress(stream.data(), stream.size(), out.data() + produced, unc[i],
                       window_bits)) {
      return false;
    }
    produced += unc[i];
  }
  return produced == total;
}

}  // namespace xn
