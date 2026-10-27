// xenon-native: XACT wave bank reader (see xwb.h).
#include "audio/xwb.h"

#include <cstdio>
#include <cstring>

namespace xn::audio {
namespace {

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

constexpr uint32_t kMagicBigEndian = 0x444E4257;  // "DNBW": 'WBND' written by a big-endian tool
constexpr uint32_t kSegmentCount = 5;
enum Segment { kBankData = 0, kEntryMetadata, kSeekTables, kEntryNames, kEntryWaveData };

}  // namespace

bool XwbParse(std::vector<uint8_t> file, XwbBank& out, std::string& error) {
  out = XwbBank{};
  if (file.size() < 8 + kSegmentCount * 8) {
    error = "file too small";
    return false;
  }
  const uint8_t* p = file.data();
  if (Be32(p) != kMagicBigEndian) {
    error = "not a big-endian XACT wave bank";
    return false;
  }
  out.version = Be32(p + 4);
  struct Seg { uint32_t offset, length; } seg[kSegmentCount];
  for (uint32_t i = 0; i < kSegmentCount; i++) {
    seg[i].offset = Be32(p + 8 + i * 8);
    seg[i].length = Be32(p + 12 + i * 8);
    if (uint64_t(seg[i].offset) + seg[i].length > file.size()) {
      error = "segment " + std::to_string(i) + " out of range";
      return false;
    }
  }
  // WAVEBANKDATA (0x60 bytes): flags, entry count, name[64], metadata element
  // size, name element size, alignment, compact format, build time.
  if (seg[kBankData].length < 0x60) {
    error = "bank data segment too small";
    return false;
  }
  const uint8_t* bd = p + seg[kBankData].offset;
  out.flags = Be32(bd);
  const uint32_t entry_count = Be32(bd + 4);
  out.name.assign(reinterpret_cast<const char*>(bd + 8), strnlen(reinterpret_cast<const char*>(bd + 8), 64));
  const uint32_t entry_size = Be32(bd + 0x48);
  const uint32_t name_size = Be32(bd + 0x4C);
  out.alignment = Be32(bd + 0x50);
  if (entry_size < 24) {
    error = "unsupported entry size " + std::to_string(entry_size);
    return false;
  }
  if (uint64_t(entry_count) * entry_size > seg[kEntryMetadata].length) {
    error = "entry table truncated";
    return false;
  }
  out.wave_data_offset = seg[kEntryWaveData].offset;
  out.wave_data_length = seg[kEntryWaveData].length;
  out.entries.resize(entry_count);
  for (uint32_t i = 0; i < entry_count; i++) {
    const uint8_t* e = p + seg[kEntryMetadata].offset + i * entry_size;
    XwbEntry& d = out.entries[i];
    const uint32_t fd = Be32(e);
    d.flags = fd & 0xF;
    d.duration = fd >> 4;
    d.format_raw = Be32(e + 4);
    // Version 23 mini-format: tag:1 (0 PCM, 1 XMA), channels:3, rate:18,
    // block align:8, bits:1 (bit 31). Verified against the GW banks: music is
    // XMA 2ch 48000, effects are PCM 1ch 48000.
    d.tag = d.format_raw & 1;
    d.channels = (d.format_raw >> 1) & 7;
    d.sample_rate = (d.format_raw >> 4) & 0x3FFFF;
    d.block_align = (d.format_raw >> 22) & 0xFF;
    d.bits_per_sample = (d.format_raw >> 31) ? 16 : 8;
    d.play_offset = Be32(e + 8);
    d.play_length = Be32(e + 12);
    d.loop_a = Be32(e + 16);
    d.loop_b = Be32(e + 20);
    if (uint64_t(d.play_offset) + d.play_length > out.wave_data_length) {
      error = "entry " + std::to_string(i) + " play region out of range";
      return false;
    }
    if (seg[kEntryNames].length >= (i + 1) * name_size && name_size) {
      const char* n = reinterpret_cast<const char*>(p + seg[kEntryNames].offset + i * name_size);
      d.name.assign(n, strnlen(n, name_size));
    }
  }
  out.file = std::move(file);
  return true;
}

bool XwbLoad(const std::string& path, XwbBank& out, std::string& error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  std::vector<uint8_t> data;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
  std::fclose(f);
  return XwbParse(std::move(data), out, error);
}

}  // namespace xn::audio
