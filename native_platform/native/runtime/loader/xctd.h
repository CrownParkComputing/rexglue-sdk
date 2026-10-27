// XCTD - the XCOMPRESS "LZXTDECODE" compressed file format (magic 0x0FF512ED).
// On a retail console the kernel's file cache transparently decompresses reads
// on files with this header; titles see the plain bytes. Ion Assault's
// data_xbox360.pak is such a file.
//
// Layout (all big-endian):
//   u32 magic, u16 version, u16 reserved, u32 crc, u32 flags
//   flags: segments = (f >> 6) & 0xffff
//          entry bits = 20 or 32 per (f >> 22) & 3 -> { 20, 32 }
//          compressed block stride = 0x8000 << ((f >> 4) & 3)
//          LZX window = 1 << ((f & 0xf) + 0xf)
//   table: `segments` uncompressed block sizes, MSB-first bit-packed
//   payload: one fixed-stride region per segment; region 0 starts right after
//     the table, region i at file offset i * stride. Inside a region the LZX
//     stream is framed as u16-BE-length chunks (a zero length ends it).
//     Each segment is an independent LZX stream decoding to its table size.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xn {

bool XctdIsCompressed(const void* data, size_t size);

// Decodes the whole file into `out`. Returns false on malformed input.
bool XctdDecompress(const void* data, size_t size, std::vector<uint8_t>& out);

}  // namespace xn
