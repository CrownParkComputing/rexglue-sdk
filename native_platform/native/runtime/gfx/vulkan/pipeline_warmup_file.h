#pragma once

#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <type_traits>
#include <vector>
#include <utility>
#include <unistd.h>
#include <xxhash.h>

namespace xn::gfx {
inline constexpr uint32_t kMaxWarmupRecords = 4096;
inline constexpr uint32_t kMaxWarmupShaderWords = 16384;
inline constexpr size_t kMaxWarmupBytes = 64u << 20;

template <typename State> struct PipelineWarmupRecord {
  State state{};
  uint64_t vertex_hash = 0, pixel_hash = 0;
  uint64_t vertex_modification = 0, pixel_modification = 0;
  std::vector<uint32_t> vertex_code, pixel_code;
};

template <typename T> inline void WarmupAppend(std::vector<uint8_t>& bytes, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const auto* p = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), p, p + sizeof(value));
}
template <typename T> inline bool WarmupTake(std::span<const uint8_t>& bytes, T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (bytes.size() < sizeof(value)) return false;
  std::memcpy(&value, bytes.data(), sizeof(value));
  bytes = bytes.subspan(sizeof(value));
  return true;
}

// The catalogue contains guest microcode and pipeline state, never Vulkan
// handles or translated binaries. The current translator prepares it again.
template <typename State>
std::vector<PipelineWarmupRecord<State>> ReadPipelineWarmup(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const auto length = file.tellg();
  if (length < 24 || length > std::streamoff(kMaxWarmupBytes)) return {};
  file.seekg(0);
  std::vector<uint8_t> storage(static_cast<size_t>(length));
  if (!file.read(reinterpret_cast<char*>(storage.data()), length)) return {};
  std::span<const uint8_t> bytes(storage);
  uint64_t magic = 0, checksum = 0;
  uint32_t state_size = 0, count = 0;
  if (!WarmupTake(bytes, magic) || !WarmupTake(bytes, checksum) ||
      magic != 0x3150554D5241574Eull || XXH3_64bits(bytes.data(), bytes.size()) != checksum ||
      !WarmupTake(bytes, state_size) || state_size != sizeof(State) ||
      !WarmupTake(bytes, count) || count > kMaxWarmupRecords) return {};
  std::vector<PipelineWarmupRecord<State>> records;
  records.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    PipelineWarmupRecord<State> record;
    uint32_t vertex_words = 0, pixel_words = 0;
    if (!WarmupTake(bytes, record.state) || !WarmupTake(bytes, record.vertex_hash) ||
        !WarmupTake(bytes, record.pixel_hash) || !WarmupTake(bytes, record.vertex_modification) ||
        !WarmupTake(bytes, record.pixel_modification) || !WarmupTake(bytes, vertex_words) ||
        !WarmupTake(bytes, pixel_words) || !vertex_words || vertex_words > kMaxWarmupShaderWords ||
        pixel_words > kMaxWarmupShaderWords || !record.vertex_hash ||
        (pixel_words && !record.pixel_hash)) return {};
    const size_t size = size_t(vertex_words + pixel_words) * sizeof(uint32_t);
    if (size > bytes.size()) return {};
    record.vertex_code.resize(vertex_words);
    record.pixel_code.resize(pixel_words);
    std::memcpy(record.vertex_code.data(), bytes.data(), size_t(vertex_words) * 4);
    if (pixel_words)
      std::memcpy(record.pixel_code.data(), bytes.data() + size_t(vertex_words) * 4,
                  size_t(pixel_words) * 4);
    bytes = bytes.subspan(size);
    records.push_back(std::move(record));
  }
  return bytes.empty() ? std::move(records) : std::vector<PipelineWarmupRecord<State>>{};
}

template <typename State>
bool WritePipelineWarmup(const std::filesystem::path& path,
                         std::span<const PipelineWarmupRecord<State>> records) {
  if (records.size() > kMaxWarmupRecords) return false;
  std::vector<uint8_t> payload;
  WarmupAppend(payload, uint32_t(sizeof(State)));
  WarmupAppend(payload, uint32_t(records.size()));
  for (const auto& record : records) {
    if (record.vertex_code.empty() || record.vertex_code.size() > kMaxWarmupShaderWords ||
        record.pixel_code.size() > kMaxWarmupShaderWords || !record.vertex_hash ||
        (!record.pixel_code.empty() && !record.pixel_hash)) return false;
    WarmupAppend(payload, record.state);
    WarmupAppend(payload, record.vertex_hash);
    WarmupAppend(payload, record.pixel_hash);
    WarmupAppend(payload, record.vertex_modification);
    WarmupAppend(payload, record.pixel_modification);
    WarmupAppend(payload, uint32_t(record.vertex_code.size()));
    WarmupAppend(payload, uint32_t(record.pixel_code.size()));
    for (const auto& code : {&record.vertex_code, &record.pixel_code}) {
      const auto* p = reinterpret_cast<const uint8_t*>(code->data());
      if (!code->empty()) payload.insert(payload.end(), p, p + code->size() * 4);
    }
    if (payload.size() + 16 > kMaxWarmupBytes) return false;
  }
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return false;
  const auto temporary = std::filesystem::path(path.string() + ".tmp-" + std::to_string(getpid()));
  std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
  const uint64_t magic = 0x3150554D5241574Eull;
  const uint64_t checksum = XXH3_64bits(payload.data(), payload.size());
  file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
  file.write(reinterpret_cast<const char*>(&checksum), sizeof(checksum));
  file.write(reinterpret_cast<const char*>(payload.data()), payload.size());
  file.close();
  if (file) std::filesystem::rename(temporary, path, error);
  if (!file || error) { std::filesystem::remove(temporary, error); return false; }
  return true;
}
} // namespace xn::gfx
