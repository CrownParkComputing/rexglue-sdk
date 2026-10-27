#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace xn::gfx::trace {

// The container stores bytes, never native pointers or structure padding.
// Scalar event fields are explicitly little-endian through these helpers;
// opaque guest-memory/shader/image blobs retain their original byte order.
class ByteWriter {
 public:
  void U32(uint32_t value);
  void U64(uint64_t value);
  void Bytes(std::span<const uint8_t> bytes);
  void String(const std::string& value);
  const std::vector<uint8_t>& Data() const { return data_; }
  std::vector<uint8_t> Take() { return std::move(data_); }

 private:
  std::vector<uint8_t> data_;
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const uint8_t> data) : data_(data) {}
  bool U32(uint32_t& value);
  bool U64(uint64_t& value);
  bool Bytes(size_t count, std::span<const uint8_t>& bytes);
  bool String(std::string& value, size_t maximum = 4096);
  bool Done() const { return valid_ && offset_ == data_.size(); }
  bool Valid() const { return valid_; }
  size_t Remaining() const { return valid_ ? data_.size() - offset_ : 0; }

 private:
  std::span<const uint8_t> data_;
  size_t offset_ = 0;
  bool valid_ = true;
};

enum class EventKind : uint32_t {
  kCaptureInfo = 1,
  kMemoryPage,
  kRegisterFile,
  kRegisterWrite,
  kRegisterBatch,
  kShaderLoad,
  kDraw,
  kGpuEvent,
  kSwap,
  kFrameReference,
  kTextureRevision,
  kTextureRead,
  kTextureDraw,
  kTextureSync,
  kEndCapture,
};

struct Event {
  EventKind kind = EventKind::kCaptureInfo;
  uint64_t sequence = 0;  // sequence among all container records
  std::vector<uint8_t> payload;
};

struct Limits {
  uint64_t max_file_bytes = UINT64_C(32) << 30;
  uint64_t max_blob_bytes = UINT64_C(64) << 20;
  uint64_t max_event_bytes = UINT64_C(16) << 20;
  uint64_t max_blobs = 1'000'000;
  uint64_t max_events = 5'000'000;
};

struct Statistics {
  uint64_t blobs = 0, events = 0, records = 0, file_bytes = 0;
  uint64_t unique_blob_bytes = 0, deduplicated_blob_bytes = 0;
};

// Optional hash selector exists for collision regression tests. Actual on-disk
// integrity hashes always use XXH3, independently of the deduplication bucket.
using DedupHasher = uint64_t (*)(std::span<const uint8_t>);

class Writer {
 public:
  explicit Writer(std::filesystem::path path, Limits limits = {}, DedupHasher bucket_hash = nullptr);
  ~Writer();
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  bool Open(std::string& error);
  // 0 means failure. Empty blobs receive a valid nonzero ID. Equal hashes
  // alone never deduplicate: exact existing bytes are compared on disk.
  uint64_t PutBlob(std::span<const uint8_t> bytes, std::string& error);
  bool WriteEvent(EventKind kind, std::span<const uint8_t> payload, std::string& error);
  // Required: destruction without Finish leaves an incomplete, rejected file.
  bool Finish(std::string& error);
  Statistics GetStatistics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Reader {
 public:
  explicit Reader(std::filesystem::path path, Limits limits = {});
  ~Reader();
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  // Validates the ENTIRE file (footer, every checksum, bounds and IDs) before
  // exposing events. No partial/truncated capture can begin a replay.
  bool Open(std::string& error);
  // false + empty error means the verified footer was reached.
  bool Next(Event& event, std::string& error);
  bool ReadBlob(uint64_t id, std::vector<uint8_t>& bytes, std::string& error);
  bool Rewind(std::string& error);
  bool Finished() const;
  Statistics GetStatistics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xn::gfx::trace
