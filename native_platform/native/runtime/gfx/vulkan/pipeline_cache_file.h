#pragma once

#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <xxhash.h>

namespace xn::gfx {
// Driver blobs are optional accelerators. Reject incomplete, damaged or foreign
// data before handing it to Vulkan; a cache miss always compiles normally.
inline constexpr size_t kMaxPipelineCacheBytes = 256u << 20;
inline bool PipelineCacheMatches(std::span<const uint8_t> bytes,
                                 const VkPhysicalDeviceProperties& device) {
  VkPipelineCacheHeaderVersionOne header{};
  if (bytes.size() < sizeof(header) || bytes.size() > kMaxPipelineCacheBytes) return false;
  std::memcpy(&header, bytes.data(), sizeof(header));
  return header.headerSize >= sizeof(header) && header.headerSize <= bytes.size() &&
      header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
      header.vendorID == device.vendorID && header.deviceID == device.deviceID &&
      !std::memcmp(header.pipelineCacheUUID, device.pipelineCacheUUID, VK_UUID_SIZE);
}

inline std::vector<uint8_t> ReadPipelineCache(const std::filesystem::path& path,
                                            const VkPhysicalDeviceProperties& device) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const auto length = file.tellg();
  if (length < std::streamoff(16) || length > std::streamoff(kMaxPipelineCacheBytes + 16)) return {};
  file.seekg(0);
  uint64_t magic = 0, checksum = 0;
  file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  file.read(reinterpret_cast<char*>(&checksum), sizeof(checksum));
  if (magic != 0x3145484341434E58ull) return {}; // XNCACHE1
  std::vector<uint8_t> bytes(size_t(length) - 16);
  if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())) ||
      !PipelineCacheMatches(bytes,device) || XXH3_64bits(bytes.data(),bytes.size()) != checksum) return {};
  return bytes;
}

inline bool WritePipelineCache(const std::filesystem::path& path, std::span<const uint8_t> bytes,
                               const VkPhysicalDeviceProperties& device) {
  if (!PipelineCacheMatches(bytes,device)) return false;
  std::error_code error;
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(),error);
  if (error) return false;
  const auto temporary = std::filesystem::path(path.string()+".tmp-"+std::to_string(getpid()));
  std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
  const uint64_t magic = 0x3145484341434E58ull, checksum = XXH3_64bits(bytes.data(),bytes.size());
  file.write(reinterpret_cast<const char*>(&magic),sizeof(magic));
  file.write(reinterpret_cast<const char*>(&checksum),sizeof(checksum));
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  file.close();
  if (file) std::filesystem::rename(temporary,path,error);
  if (!file || error) { std::filesystem::remove(temporary,error); return false; }
  return true;
}
} // namespace xn::gfx
