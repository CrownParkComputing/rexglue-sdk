// xenon-native: XACT wave bank (.xwb) reader, big-endian (Xbox 360) layout.
//
// Written from the on-disk structure of the Geometry Wars banks (version 23,
// XACT2). Used by the offline decode test today; the XACT-level replacement
// (Option X in docs/AUDIO.md) would build on it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace xn::audio {

struct XwbEntry {
  uint32_t flags = 0;         // low 4 bits of dwFlagsAndDuration
  uint32_t duration = 0;      // samples (high 28 bits), 0 when the bank does not store it
  uint32_t format_raw = 0;    // WAVEBANKMINIWAVEFORMAT as stored
  uint32_t tag = 0;           // 0 = PCM, 1 = XMA (version 23 layout)
  uint32_t channels = 0;
  uint32_t sample_rate = 0;
  uint32_t block_align = 0;
  uint32_t bits_per_sample = 0;  // 8 or 16 (PCM)
  uint32_t play_offset = 0;   // bytes from the start of the wave data segment
  uint32_t play_length = 0;   // bytes
  uint32_t loop_a = 0;        // last two dwords of the entry, meaning depends on the codec
  uint32_t loop_b = 0;
  std::string name;           // empty unless the bank carries an ENTRYNAMES segment
};

struct XwbBank {
  uint32_t version = 0;
  uint32_t flags = 0;
  uint32_t alignment = 0;
  std::string name;
  std::vector<XwbEntry> entries;
  uint32_t wave_data_offset = 0;  // file offset of the ENTRYWAVEDATA segment
  uint32_t wave_data_length = 0;
  std::vector<uint8_t> file;      // the whole bank (entries index into it)

  const uint8_t* wave_data(const XwbEntry& e) const {
    return file.data() + wave_data_offset + e.play_offset;
  }
};

// Parses a bank from memory. Returns false and fills error on malformed input.
bool XwbParse(std::vector<uint8_t> file, XwbBank& out, std::string& error);
bool XwbLoad(const std::string& path, XwbBank& out, std::string& error);

}  // namespace xn::audio
