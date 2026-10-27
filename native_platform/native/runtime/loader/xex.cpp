#include "xex.h"

#include <cstring>
#include <fstream>

extern "C" {
#include "rijndael-alg-fst.h"
}
#include "TinySHA1.hpp"
extern "C" {
#include <mspack.h>
#include <lzx.h>
}

namespace xn {

uint32_t FindXexExport(const XexImage& image, uint32_t ordinal) {
  auto valid = [&](uint32_t address, uint64_t size) {
    return address >= image.base_address &&
        uint64_t(address) - image.base_address + size <= image.image.size();
  };
  if (!image.export_table || !valid(image.export_table, 0x2C)) return 0;
  const auto* table = image.image.data() + (image.export_table - image.base_address);
  auto read = [](const uint8_t* bytes) { uint32_t value; std::memcpy(&value, bytes, 4); return __builtin_bswap32(value); };
  auto count = read(table + 0x24), first = read(table + 0x28);
  if (ordinal < first || ordinal - first >= count || !valid(image.export_table, 0x2Cull + uint64_t(count) * 4)) return 0;
  uint64_t address = uint64_t(read(table + 0x20)) * 0x10000 + read(table + 0x2C + (ordinal - first) * 4);
  return address <= UINT32_MAX && valid(uint32_t(address), 1) ? uint32_t(address) : 0;
}
namespace {

inline uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

const uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
const uint8_t kDevkitKey[16] = {0};

void AesCbcDecrypt(const uint8_t* key, const uint8_t* in, size_t size, uint8_t* out) {
  uint32_t rk[4 * (MAXNR + 1)];
  uint8_t iv[16] = {0};
  int nr = rijndaelKeySetupDec(rk, key, 128);
  for (size_t n = 0; n + 16 <= size; n += 16, in += 16, out += 16) {
    rijndaelDecrypt(rk, nr, in, out);
    for (int i = 0; i < 16; i++) {
      out[i] ^= iv[i];
      iv[i] = in[i];
    }
  }
}

// mspack in-memory I/O.
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
                   uint32_t window_size) {
  mspack_system sys{};
  sys.read = MemRead;
  sys.write = MemWrite;
  sys.alloc = MemAlloc;
  sys.free = MemFree;
  sys.copy = MemCopy;
  MemFile in{src, nullptr, src_len, 0};
  MemFile out{nullptr, dst, dst_len, 0};
  int window_bits = __builtin_ctz(window_size);
  lzxd_stream* lzx = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&in),
                               reinterpret_cast<mspack_file*>(&out), window_bits, 0, 0x8000,
                               off_t(dst_len), 0);
  if (!lzx) return false;
  int rc = lzxd_decompress(lzx, off_t(dst_len));
  lzxd_free(lzx);
  return rc == MSPACK_ERR_OK;
}

}  // namespace

bool XexGetOptHeader(const uint8_t* header, size_t header_size, uint32_t key,
                     uint32_t* out) {
  if (header_size < 0x18) return false;
  uint32_t count = be32(header + 0x14);
  for (uint32_t i = 0; i < count; i++) {
    size_t at = 0x18 + i * 8;
    if (at + 8 > header_size) return false;
    if (be32(header + at) == key) {
      *out = be32(header + at + 4);
      return true;
    }
  }
  return false;
}

bool LoadXexFile(const std::string& path, XexImage& out, std::string& error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { error = "cannot open " + path; return false; }
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (file.size() < 0x18 || std::memcmp(file.data(), "XEX2", 4) != 0) {
    error = "not an XEX2 file";
    return false;
  }
  const uint8_t* d = file.data();
  uint32_t header_size = be32(d + 0x08);
  uint32_t security_offset = be32(d + 0x10);
  if (header_size > file.size() || security_offset + 0x184 > file.size()) {
    error = "truncated header";
    return false;
  }
  out.header.assign(d, d + header_size);
  const uint8_t* sec = d + security_offset;
  out.image_size = be32(sec + 0x04);
  out.base_address = be32(sec + 0x110);
  out.export_table = be32(sec + 0x160);
  const uint8_t* aes_key = sec + 0x150;

  uint32_t v;
  if (XexGetOptHeader(d, header_size, 0x00010201, &v)) out.base_address = v;
  if (XexGetOptHeader(d, header_size, 0x00010100, &v)) out.entry_point = v;
  if (XexGetOptHeader(d, header_size, 0x00020200, &v)) out.default_stack_size = v;
  if (XexGetOptHeader(d, header_size, 0x00030000, &v)) out.system_flags = v;
  if (XexGetOptHeader(d, header_size, 0x00040006, &v)) {
    out.execution_info_offset = v;
    if (v + 0x18 <= header_size) out.title_id = be32(d + v + 0x0C);
  }
  if (XexGetOptHeader(d, header_size, 0x00020104, &v) && v + 0x10 <= header_size) {
    out.tls.slot_count = be32(d + v + 0);
    out.tls.raw_data_address = be32(d + v + 4);
    out.tls.data_size = be32(d + v + 8);
    out.tls.raw_data_size = be32(d + v + 12);
  }

  uint32_t fmt_off = 0;
  if (!XexGetOptHeader(d, header_size, 0x000003FF, &fmt_off)) {
    error = "no file format info";
    return false;
  }
  const uint8_t* fmt = d + fmt_off;
  uint16_t encryption = be16(fmt + 4);
  uint16_t compression = be16(fmt + 6);

  const uint8_t* exe = d + header_size;
  size_t exe_len = file.size() - header_size;

  for (int key_try = 0; key_try < 2; key_try++) {
    uint8_t session_key[16];
    AesCbcDecrypt(key_try == 0 ? kRetailKey : kDevkitKey, aes_key, 16, session_key);
    std::vector<uint8_t> plain(exe_len);
    if (encryption == 1) {
      AesCbcDecrypt(session_key, exe, exe_len & ~size_t(15), plain.data());
    } else {
      std::memcpy(plain.data(), exe, exe_len);
    }
    out.image.assign(out.image_size, 0);
    bool ok = false;
    if (compression == 0) {
      std::memcpy(out.image.data(), plain.data(), std::min<size_t>(exe_len, out.image_size));
      ok = true;
    } else if (compression == 1) {
      // Basic: runs of (data_size, zero_size).
      uint32_t info_size = be32(fmt);
      uint32_t blocks = (info_size - 8) / 8;
      const uint8_t* p = plain.data();
      uint8_t* q = out.image.data();
      ok = true;
      for (uint32_t i = 0; i < blocks; i++) {
        uint32_t data_size = be32(fmt + 8 + i * 8);
        uint32_t zero_size = be32(fmt + 12 + i * 8);
        if (q + data_size + zero_size > out.image.data() + out.image_size ||
            p + data_size > plain.data() + exe_len) { ok = false; break; }
        std::memcpy(q, p, data_size);
        p += data_size;
        q += data_size + zero_size;
      }
    } else if (compression == 2) {
      // Normal: SHA1-chained blocks of 2-byte-length chunks, then one LZX stream.
      uint32_t window_size = be32(fmt + 8);
      uint32_t block_size = be32(fmt + 12);
      const uint8_t* block_hash = fmt + 16;
      std::vector<uint8_t> lzx;
      lzx.reserve(exe_len);
      const uint8_t* p = plain.data();
      const uint8_t* end = plain.data() + exe_len;
      ok = true;
      uint8_t digest[20];
      while (block_size) {
        if (p + block_size > end) { ok = false; break; }
        sha1::SHA1 s;
        s.processBytes(p, block_size);
        s.finalize(digest);
        if (std::memcmp(digest, block_hash, 20) != 0) { ok = false; break; }
        const uint8_t* next = p + block_size;
        uint32_t next_size = be32(p);
        const uint8_t* next_hash = p + 4;
        p += 24;
        while (true) {
          uint32_t chunk = be16(p);
          p += 2;
          if (!chunk) break;
          lzx.insert(lzx.end(), p, p + chunk);
          p += chunk;
        }
        // The next block's hash lives in the current block; copy before p moves on.
        static uint8_t hash_copy[20];
        std::memcpy(hash_copy, next_hash, 20);
        block_hash = hash_copy;
        block_size = next_size;
        p = next;
      }
      if (ok) ok = LzxDecompress(lzx.data(), lzx.size(), out.image.data(), out.image_size,
                                 window_size);
    }
    if (ok && out.image.size() >= 2 && out.image[0] == 'M' && out.image[1] == 'Z') break;
    if (key_try == 1) {
      error = "image decode failed with both keys";
      return false;
    }
  }

  // Import libraries: string table then per-library record lists. Record values
  // (ordinal | type << 24) live inside the image at each slot address.
  uint32_t imp_off = 0;
  if (XexGetOptHeader(d, header_size, 0x000103FF, &imp_off)) {
    const uint8_t* imp = d + imp_off;
    uint32_t total = be32(imp);
    uint32_t str_size = be32(imp + 4);
    uint32_t str_count = be32(imp + 8);
    std::vector<std::string> names;
    for (uint32_t i = 0, o = 12; names.size() < str_count && o < 12 + str_size;) {
      const char* s = reinterpret_cast<const char*>(imp + o);
      names.emplace_back(s);
      o += uint32_t(names.back().size() + 1);
      if (o % 4) o += 4 - (o % 4);
      (void)i;
    }
    uint32_t lib_off = 12 + str_size;
    while (lib_off + 0x28 <= total) {
      const uint8_t* lib = imp + lib_off;
      uint32_t lib_size = be32(lib);
      if (!lib_size) break;
      uint16_t name_index = be16(lib + 0x24) & 0xFF;
      uint16_t count = be16(lib + 0x26);
      XexImportLibrary library;
      library.name = name_index < names.size() ? names[name_index] : "?";
      for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = be32(lib + 0x28 + i * 4);
        if (!slot) continue;
        uint32_t rel = slot - out.base_address;
        if (rel + 4 > out.image_size) continue;
        uint32_t value = be32(out.image.data() + rel);
        library.records.push_back({slot, uint16_t(value & 0xFFFF), uint8_t(value >> 24)});
      }
      out.imports.push_back(std::move(library));
      lib_off += lib_size;
    }
  }
  return true;
}

}  // namespace xn
