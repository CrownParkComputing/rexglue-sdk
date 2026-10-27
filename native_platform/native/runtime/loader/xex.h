// xenon-native: XEX2 loader. Produces the decrypted, decompressed image plus the
// handful of header facts the runtime needs. No guest memory involved.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace xn {

struct XexImportRecord {
  uint32_t slot_address;  // guest address of the 4-byte import slot
  uint16_t ordinal;
  uint8_t type;           // 0 = value/variable slot, 1 = function thunk
};
struct XexImportLibrary {
  std::string name;  // "xboxkrnl.exe", "xam.xex"
  std::vector<XexImportRecord> records;
};
struct XexTlsInfo {
  uint32_t slot_count = 0;
  uint32_t raw_data_address = 0;
  uint32_t data_size = 0;
  uint32_t raw_data_size = 0;
};

struct XexImage {
  std::vector<uint8_t> header;  // raw XEX header bytes (header_size), big-endian as on disk
  std::vector<uint8_t> image;   // decrypted+decompressed PE image, image_size bytes
  uint32_t base_address = 0;
  uint32_t image_size = 0;
  uint32_t entry_point = 0;
  uint32_t default_stack_size = 0x40000;
  uint32_t system_flags = 0;
  uint32_t execution_info_offset = 0;  // offset of the execution-info struct inside header
  uint32_t title_id = 0;
  uint32_t export_table = 0;
  XexTlsInfo tls;
  std::vector<XexImportLibrary> imports;
};

// Reads the raw XEX2 file at path. On failure returns false and fills error.
bool LoadXexFile(const std::string& path, XexImage& out, std::string& error);
// Resolves the security-header ordinal table against the decoded image.
// Returns zero for absent, truncated, or out-of-image entries.
uint32_t FindXexExport(const XexImage& image, uint32_t ordinal);

// Optional-header lookup on a raw big-endian XEX header (used by the loader and by
// RtlImageXexHeaderField on the guest copy). Returns true and the value/offset.
bool XexGetOptHeader(const uint8_t* header, size_t header_size, uint32_t key,
                     uint32_t* out_value_or_offset);

}  // namespace xn
