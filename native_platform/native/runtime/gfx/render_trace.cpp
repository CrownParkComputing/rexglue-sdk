#include "gfx/render_trace.h"

#include <xxhash.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>

namespace xn::gfx::trace {
namespace {

constexpr uint32_t kVersion = 1;
constexpr size_t kFileHeaderBytes = 64;
constexpr size_t kRecordHeaderBytes = 40;
constexpr size_t kBlobMetadataBytes = 24;
constexpr size_t kFooterBytes = 32;
constexpr uint32_t kBlobRecord = 1, kEventRecord = 2, kFooterRecord = 3;
constexpr std::array<uint8_t, 8> kMagic{'X', 'N', 'R', 'T', 'R', 'C', '0', '1'};
constexpr size_t kChunkBytes = 64 * 1024;

void Store32(uint8_t* out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) out[i] = uint8_t(value >> (i * 8));
}
void Store64(uint8_t* out, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) out[i] = uint8_t(value >> (i * 8));
}
uint32_t Load32(const uint8_t* in) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= uint32_t(in[i]) << (i * 8);
  return value;
}
uint64_t Load64(const uint8_t* in) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= uint64_t(in[i]) << (i * 8);
  return value;
}
uint64_t Hash(std::span<const uint8_t> bytes) {
  return XXH3_64bits(bytes.data(), bytes.size());
}
class Hasher {
 public:
  Hasher() : state_(XXH3_createState()) {
    if (state_) XXH3_64bits_reset(state_);
  }
  ~Hasher() { XXH3_freeState(state_); }
  explicit operator bool() const { return state_ != nullptr; }
  void Update(std::span<const uint8_t> bytes) { XXH3_64bits_update(state_, bytes.data(), bytes.size()); }
  uint64_t Digest() const { return XXH3_64bits_digest(state_); }
 private:
  XXH3_state_t* state_;
};

bool KnownKind(uint32_t kind) {
  return kind >= uint32_t(EventKind::kCaptureInfo) && kind <= uint32_t(EventKind::kEndCapture);
}
bool GoodLimits(const Limits& limits, std::string& error) {
  if (limits.max_file_bytes < kFileHeaderBytes + kRecordHeaderBytes + kFooterBytes ||
      limits.max_file_bytes > uint64_t(std::numeric_limits<std::streamoff>::max()) ||
      limits.max_blob_bytes > uint64_t(std::numeric_limits<size_t>::max()) - kRecordHeaderBytes - kBlobMetadataBytes ||
      limits.max_blob_bytes > uint64_t(std::numeric_limits<std::streamsize>::max()) - kRecordHeaderBytes - kBlobMetadataBytes ||
      limits.max_event_bytes > uint64_t(std::numeric_limits<size_t>::max()) - kRecordHeaderBytes ||
      limits.max_event_bytes > uint64_t(std::numeric_limits<std::streamsize>::max()) - kRecordHeaderBytes ||
      limits.max_blobs > uint64_t(std::numeric_limits<size_t>::max())) {
    error = "invalid trace resource limits";
    return false;
  }
  return true;
}
bool ReadExact(std::istream& stream, std::span<uint8_t> bytes) {
  if (bytes.empty()) return true;
  stream.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
  return bool(stream);
}
bool WriteExact(std::ostream& stream, std::span<const uint8_t> bytes) {
  if (bytes.empty()) return true;
  stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  return bool(stream);
}
bool Seek(std::istream& stream, uint64_t offset) {
  stream.clear();
  stream.seekg(std::streamoff(offset));
  return bool(stream);
}

struct BlobInfo {
  uint64_t id = 0, offset = 0, size = 0, hash = 0;
};
struct RecordHeader {
  uint32_t type = 0, subtype = 0;
  uint64_t sequence = 0, bytes = 0, hash = 0;
};
using HeaderBytes = std::array<uint8_t, kRecordHeaderBytes>;
HeaderBytes EncodeHeader(uint32_t type, uint32_t subtype, uint64_t sequence,
                         uint64_t bytes, uint64_t hash) {
  HeaderBytes header{};
  Store32(header.data(), type);
  Store32(header.data() + 4, subtype);
  Store64(header.data() + 8, sequence);
  Store64(header.data() + 16, bytes);
  Store64(header.data() + 24, hash);
  Store64(header.data() + 32, Hash(std::span(header).first(32)));
  return header;
}
bool DecodeHeader(const HeaderBytes& bytes, RecordHeader& header, std::string& error) {
  if (Load64(bytes.data() + 32) != Hash(std::span(bytes).first(32))) {
    error = "trace record header checksum mismatch";
    return false;
  }
  header = {Load32(bytes.data()), Load32(bytes.data() + 4), Load64(bytes.data() + 8),
            Load64(bytes.data() + 16), Load64(bytes.data() + 24)};
  if ((header.type != kBlobRecord && header.type != kEventRecord && header.type != kFooterRecord) ||
      (header.type == kEventRecord ? !KnownKind(header.subtype) : header.subtype != 0)) {
    error = "unknown trace record/event kind";
    return false;
  }
  return true;
}
uint64_t Chain(uint64_t previous, const HeaderBytes& header) {
  return XXH3_64bits_withSeed(header.data(), header.size(), previous);
}

}  // namespace

void ByteWriter::U32(uint32_t value) {
  const size_t offset = data_.size();
  data_.resize(offset + 4);
  Store32(data_.data() + offset, value);
}
void ByteWriter::U64(uint64_t value) {
  const size_t offset = data_.size();
  data_.resize(offset + 8);
  Store64(data_.data() + offset, value);
}
void ByteWriter::Bytes(std::span<const uint8_t> bytes) {
  data_.insert(data_.end(), bytes.begin(), bytes.end());
}
void ByteWriter::String(const std::string& value) {
  if (value.size() > UINT32_MAX) throw std::length_error("trace string exceeds uint32 length");
  U32(uint32_t(value.size()));
  Bytes({reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}
bool ByteReader::Bytes(size_t count, std::span<const uint8_t>& bytes) {
  if (!valid_ || count > data_.size() - offset_) {
    bytes = {};
    valid_ = false;
    return false;
  }
  bytes = data_.subspan(offset_, count);
  offset_ += count;
  return true;
}
bool ByteReader::U32(uint32_t& value) {
  std::span<const uint8_t> bytes;
  if (!Bytes(4, bytes)) return false;
  value = Load32(bytes.data());
  return true;
}
bool ByteReader::U64(uint64_t& value) {
  std::span<const uint8_t> bytes;
  if (!Bytes(8, bytes)) return false;
  value = Load64(bytes.data());
  return true;
}
bool ByteReader::String(std::string& value, size_t maximum) {
  uint32_t count;
  std::span<const uint8_t> bytes;
  if (!U32(count) || count > maximum || !Bytes(count, bytes)) {
    valid_ = false;
    value.clear();
    return false;
  }
  value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

struct Writer::Impl {
  std::filesystem::path path;
  Limits limits;
  DedupHasher bucket_hash;
  std::fstream stream;
  Statistics stats;
  uint64_t chain = 0;
  bool opened = false, finished = false, failed = false;
  bool needs_write_seek = false;
  std::map<std::pair<uint64_t, uint64_t>, std::vector<BlobInfo>> buckets;

  Impl(std::filesystem::path p, Limits l, DedupHasher h)
      : path(std::move(p)), limits(l), bucket_hash(h) {}
  bool Fail(std::string& error, const char* text) {
    failed = true;
    error = text;
    return false;
  }
  bool Ready(std::string& error) {
    if (!opened || finished || failed) return Fail(error, "trace writer is not open for appending");
    return true;
  }
  bool Capacity(uint64_t bytes, bool footer, std::string& error) {
    const uint64_t reserve = footer ? 0 : kRecordHeaderBytes + kFooterBytes;
    if (stats.file_bytes > limits.max_file_bytes || reserve > limits.max_file_bytes - stats.file_bytes ||
        bytes > limits.max_file_bytes - stats.file_bytes - reserve)
      return Fail(error, "trace exceeds file byte limit");
    return true;
  }
  bool Append(const HeaderBytes& header, std::span<const uint8_t> first,
              std::span<const uint8_t> second, bool footer, std::string& error) {
    if (!Capacity(kRecordHeaderBytes + first.size() + second.size(), footer, error)) return false;
    // Keep forward writes buffered. Seeking for every record implicitly
    // flushes fstream and turns small trace records into tiny syscalls.
    if (needs_write_seek) {
      stream.clear();
      stream.seekp(std::streamoff(stats.file_bytes));
      needs_write_seek = false;
    }
    if (!stream || !WriteExact(stream, header) || !WriteExact(stream, first) || !WriteExact(stream, second))
      return Fail(error, "could not append trace record");
    stats.file_bytes += kRecordHeaderBytes + first.size() + second.size();
    if (!footer) {
      ++stats.records;
      chain = Chain(chain, header);
    }
    return true;
  }
  // A hash match only chooses candidates. Compare the full stored bytes before reusing an ID.
  bool Equal(const BlobInfo& candidate, std::span<const uint8_t> bytes, bool& equal, std::string& error) {
    equal = false;
    if (bytes.empty()) { equal = true; return true; }
    stream.flush();
    needs_write_seek = true;
    if (!stream || !Seek(stream, candidate.offset)) return Fail(error, "could not read deduplication candidate");
    std::array<uint8_t, kChunkBytes> chunk;
    size_t consumed = 0;
    while (consumed < bytes.size()) {
      const size_t count = std::min(chunk.size(), bytes.size() - consumed);
      if (!ReadExact(stream, std::span(chunk).first(count))) return Fail(error, "could not read deduplication bytes");
      if (std::memcmp(chunk.data(), bytes.data() + consumed, count) != 0) return true;
      consumed += count;
    }
    equal = true;
    return true;
  }
};

Writer::Writer(std::filesystem::path path, Limits limits, DedupHasher bucket_hash)
    : impl_(std::make_unique<Impl>(std::move(path), limits, bucket_hash)) {}
Writer::~Writer() = default;
bool Writer::Open(std::string& error) {
  error.clear();
  auto& self = *impl_;
  if (self.opened || self.failed) return self.Fail(error, "trace writer may only be opened once");
  if (!GoodLimits(self.limits, error)) { self.failed = true; return false; }
  self.stream.open(self.path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  if (!self.stream) return self.Fail(error, "could not create trace file");
  std::array<uint8_t, kFileHeaderBytes> header{};
  std::copy(kMagic.begin(), kMagic.end(), header.begin());
  Store32(header.data() + 8, kVersion);
  Store32(header.data() + 12, kFileHeaderBytes);
  Store64(header.data() + 56, Hash(std::span(header).first(56)));
  if (!WriteExact(self.stream, header)) return self.Fail(error, "could not write trace file header");
  self.stats.file_bytes = header.size();
  self.opened = true;
  return true;
}
uint64_t Writer::PutBlob(std::span<const uint8_t> bytes, std::string& error) {
  error.clear();
  auto& self = *impl_;
  if (!self.Ready(error)) return 0;
  if (bytes.size() > self.limits.max_blob_bytes) { self.Fail(error, "trace blob exceeds byte limit"); return 0; }
  const uint64_t hash = Hash(bytes);
  const auto key = std::make_pair(self.bucket_hash ? self.bucket_hash(bytes) : hash, uint64_t(bytes.size()));
  const auto found = self.buckets.find(key);
  if (found != self.buckets.end()) {
    for (const auto& candidate : found->second) {
      bool equal;
      if (!self.Equal(candidate, bytes, equal, error)) return 0;
      if (equal) {
        const uint64_t available = UINT64_MAX - self.stats.deduplicated_blob_bytes;
        self.stats.deduplicated_blob_bytes += std::min<uint64_t>(bytes.size(), available);
        return candidate.id;
      }
    }
  }
  if (self.stats.blobs >= self.limits.max_blobs) { self.Fail(error, "trace exceeds blob count limit"); return 0; }
  const uint64_t id = self.stats.blobs + 1;
  std::array<uint8_t, kBlobMetadataBytes> metadata{};
  Store64(metadata.data(), id);
  Store64(metadata.data() + 8, bytes.size());
  Store64(metadata.data() + 16, hash);
  Hasher payload_hash;
  if (!payload_hash) { self.Fail(error, "could not allocate trace checksum state"); return 0; }
  payload_hash.Update(metadata);
  payload_hash.Update(bytes);
  const auto header = EncodeHeader(kBlobRecord, 0, self.stats.records,
                                   metadata.size() + bytes.size(), payload_hash.Digest());
  const uint64_t offset = self.stats.file_bytes + kRecordHeaderBytes + metadata.size();
  if (!self.Append(header, metadata, bytes, false, error)) return 0;
  try {
    self.buckets[key].push_back({id, offset, uint64_t(bytes.size()), hash});
  } catch (const std::bad_alloc&) {
    self.Fail(error, "could not allocate trace blob index");
    return 0;
  }
  ++self.stats.blobs;
  self.stats.unique_blob_bytes += bytes.size();
  return id;
}
bool Writer::WriteEvent(EventKind kind, std::span<const uint8_t> payload, std::string& error) {
  error.clear();
  auto& self = *impl_;
  if (!self.Ready(error)) return false;
  if (!KnownKind(uint32_t(kind))) return self.Fail(error, "unknown trace event kind");
  if (payload.size() > self.limits.max_event_bytes) return self.Fail(error, "trace event exceeds byte limit");
  if (self.stats.events >= self.limits.max_events) return self.Fail(error, "trace exceeds event count limit");
  const auto header = EncodeHeader(kEventRecord, uint32_t(kind), self.stats.records, payload.size(), Hash(payload));
  if (!self.Append(header, payload, {}, false, error)) return false;
  ++self.stats.events;
  return true;
}
bool Writer::Finish(std::string& error) {
  error.clear();
  auto& self = *impl_;
  if (!self.Ready(error)) return false;
  std::array<uint8_t, kFooterBytes> footer{};
  Store64(footer.data(), self.stats.records);
  Store64(footer.data() + 8, self.stats.blobs);
  Store64(footer.data() + 16, self.stats.events);
  Store64(footer.data() + 24, self.chain);
  const auto header = EncodeHeader(kFooterRecord, 0, self.stats.records, footer.size(), Hash(footer));
  if (!self.Append(header, footer, {}, true, error)) return false;
  self.stream.flush();
  if (!self.stream) return self.Fail(error, "could not flush completed trace");
  self.finished = true;
  return true;
}
Statistics Writer::GetStatistics() const { return impl_->stats; }

struct Reader::Impl {
  std::filesystem::path path;
  Limits limits;
  std::ifstream stream, blob_stream;
  Statistics stats;
  std::vector<BlobInfo> blobs;
  uint64_t cursor = kFileHeaderBytes, sequence = 0, footer_offset = 0;
  uint64_t validated_chain = 0, iteration_chain = 0;
  bool opened = false, finished = false, failed = false;

  Impl(std::filesystem::path p, Limits l) : path(std::move(p)), limits(l) {}
  bool Fail(std::string& error, const char* text) {
    failed = true;
    error = text;
    return false;
  }
  bool Header(uint64_t offset, uint64_t expected_sequence, HeaderBytes& raw,
              RecordHeader& header, std::string& error) {
    if (offset > stats.file_bytes || stats.file_bytes - offset < raw.size() ||
        !ReadExact(stream, raw)) return Fail(error, "truncated trace record header");
    if (!DecodeHeader(raw, header, error)) { failed = true; return false; }
    if (header.sequence != expected_sequence) return Fail(error, "trace record sequence mismatch");
    if (header.bytes > stats.file_bytes - offset - raw.size()) return Fail(error, "truncated trace record payload");
    if (header.type == kBlobRecord) {
      if (header.bytes < kBlobMetadataBytes || header.bytes - kBlobMetadataBytes > limits.max_blob_bytes)
        return Fail(error, "trace blob exceeds byte limit");
    } else if (header.type == kEventRecord) {
      if (header.bytes > limits.max_event_bytes) return Fail(error, "trace event exceeds byte limit");
    } else if (header.bytes != kFooterBytes) {
      return Fail(error, "invalid trace footer length");
    }
    return true;
  }
  // Scan a payload using a fixed buffer; whole captures never need to fit in RAM.
  bool Payload(uint64_t size, Hasher& hash, Hasher* content_hash, std::string& error) {
    std::array<uint8_t, kChunkBytes> chunk;
    while (size) {
      const size_t count = size_t(std::min<uint64_t>(chunk.size(), size));
      auto bytes = std::span(chunk).first(count);
      if (!ReadExact(stream, bytes)) return Fail(error, "truncated trace payload");
      hash.Update(bytes);
      if (content_hash) content_hash->Update(bytes);
      size -= count;
    }
    return true;
  }
};

Reader::Reader(std::filesystem::path path, Limits limits)
    : impl_(std::make_unique<Impl>(std::move(path), limits)) {}
Reader::~Reader() = default;
bool Reader::Open(std::string& error) {
  error.clear();
  auto& self = *impl_;
  if (self.opened || self.failed) return self.Fail(error, "trace reader may only be opened once");
  if (!GoodLimits(self.limits, error)) { self.failed = true; return false; }
  std::error_code ec;
  const uint64_t file_bytes = std::filesystem::file_size(self.path, ec);
  if (ec) return self.Fail(error, "could not stat trace file");
  if (file_bytes > self.limits.max_file_bytes) return self.Fail(error, "trace exceeds file byte limit");
  if (file_bytes < kFileHeaderBytes + kRecordHeaderBytes + kFooterBytes)
    return self.Fail(error, "trace is incomplete: missing header or footer");
  self.stats.file_bytes = file_bytes;
  self.stream.open(self.path, std::ios::binary);
  self.blob_stream.open(self.path, std::ios::binary);
  if (!self.stream || !self.blob_stream) return self.Fail(error, "could not open trace file");
  std::array<uint8_t, kFileHeaderBytes> file_header{};
  if (!ReadExact(self.stream, file_header)) return self.Fail(error, "truncated trace file header");
  if (!std::equal(kMagic.begin(), kMagic.end(), file_header.begin()) ||
      Load32(file_header.data() + 8) != kVersion || Load32(file_header.data() + 12) != kFileHeaderBytes)
    return self.Fail(error, "unsupported trace magic/version/header size");
  if (Load64(file_header.data() + 56) != Hash(std::span(file_header).first(56)))
    return self.Fail(error, "trace file header checksum mismatch");
  for (size_t i = 16; i < 56; ++i)
    if (file_header[i]) return self.Fail(error, "unsupported trace header flags");
  uint64_t offset = kFileHeaderBytes, chain = 0;
  while (offset < file_bytes) {
    HeaderBytes raw;
    RecordHeader header;
    if (!self.Header(offset, self.stats.records, raw, header, error)) return false;
    Hasher payload_hash;
    if (!payload_hash) return self.Fail(error, "could not allocate trace checksum state");
    if (header.type == kFooterRecord) {
      std::array<uint8_t, kFooterBytes> footer;
      if (!ReadExact(self.stream, footer)) return self.Fail(error, "truncated trace footer");
      if (Hash(footer) != header.hash) return self.Fail(error, "trace footer checksum mismatch");
      if (Load64(footer.data()) != self.stats.records || Load64(footer.data() + 8) != self.stats.blobs ||
          Load64(footer.data() + 16) != self.stats.events || Load64(footer.data() + 24) != chain)
        return self.Fail(error, "trace footer record counts/checksum mismatch");
      if (offset + kRecordHeaderBytes + header.bytes != file_bytes)
        return self.Fail(error, "unexpected data after trace footer");
      self.footer_offset = offset;
      self.validated_chain = chain;
      self.opened = true;
      return Rewind(error);
    }
    if (header.type == kBlobRecord) {
      if (self.stats.blobs >= self.limits.max_blobs) return self.Fail(error, "trace exceeds blob count limit");
      std::array<uint8_t, kBlobMetadataBytes> metadata;
      if (!ReadExact(self.stream, metadata)) return self.Fail(error, "truncated trace blob metadata");
      payload_hash.Update(metadata);
      BlobInfo blob{Load64(metadata.data()), offset + kRecordHeaderBytes + metadata.size(),
                    Load64(metadata.data() + 8), Load64(metadata.data() + 16)};
      if (blob.id != self.stats.blobs + 1) return self.Fail(error, "invalid or repeated trace blob ID");
      if (blob.size != header.bytes - metadata.size()) return self.Fail(error, "trace blob size mismatch");
      Hasher content_hash;
      if (!content_hash) return self.Fail(error, "could not allocate trace checksum state");
      if (!self.Payload(blob.size, payload_hash, &content_hash, error)) return false;
      if (content_hash.Digest() != blob.hash) return self.Fail(error, "trace blob content checksum mismatch");
      try { self.blobs.push_back(blob); }
      catch (const std::bad_alloc&) { return self.Fail(error, "could not allocate trace blob index"); }
      ++self.stats.blobs;
      self.stats.unique_blob_bytes += blob.size;
    } else {
      if (self.stats.events >= self.limits.max_events) return self.Fail(error, "trace exceeds event count limit");
      if (!self.Payload(header.bytes, payload_hash, nullptr, error)) return false;
      ++self.stats.events;
    }
    if (payload_hash.Digest() != header.hash) return self.Fail(error, "trace payload checksum mismatch");
    ++self.stats.records;
    chain = Chain(chain, raw);
    offset += kRecordHeaderBytes + header.bytes;
  }
  return self.Fail(error, "trace is incomplete: missing footer");
}
bool Reader::Rewind(std::string& error) {
  error.clear();
  auto& self = *impl_;
  if (!self.opened || self.failed) return self.Fail(error, "trace reader is not open");
  if (!Seek(self.stream, kFileHeaderBytes)) return self.Fail(error, "could not rewind trace file");
  self.cursor = kFileHeaderBytes;
  self.sequence = 0;
  self.iteration_chain = 0;
  self.finished = false;
  return true;
}
bool Reader::Next(Event& event, std::string& error) {
  error.clear();
  auto& self = *impl_;
  event.payload.clear();
  if (!self.opened || self.failed) return self.Fail(error, "trace reader is not open");
  if (self.finished) return false;
  while (self.cursor <= self.footer_offset) {
    HeaderBytes raw;
    RecordHeader header;
    if (!self.Header(self.cursor, self.sequence, raw, header, error)) return false;
    if (header.type == kFooterRecord) {
      std::array<uint8_t, kFooterBytes> footer;
      if (self.cursor != self.footer_offset || !ReadExact(self.stream, footer) || Hash(footer) != header.hash ||
          Load64(footer.data()) != self.stats.records || Load64(footer.data() + 8) != self.stats.blobs ||
          Load64(footer.data() + 16) != self.stats.events ||
          Load64(footer.data() + 24) != self.validated_chain ||
          self.iteration_chain != self.validated_chain)
        return self.Fail(error, "trace footer changed after validation");
      self.finished = true;
      return false;
    }
    const uint64_t next = self.cursor + kRecordHeaderBytes + header.bytes;
    if (next > self.footer_offset) return self.Fail(error, "trace record changed after validation");
    self.cursor = next;
    ++self.sequence;
    self.iteration_chain = Chain(self.iteration_chain, raw);
    if (header.type == kBlobRecord) {
      if (!Seek(self.stream, next)) return self.Fail(error, "could not skip trace blob");
      continue;
    }
    try { event.payload.resize(size_t(header.bytes)); }
    catch (const std::bad_alloc&) { return self.Fail(error, "could not allocate trace event"); }
    if (!ReadExact(self.stream, event.payload) || Hash(event.payload) != header.hash) {
      event.payload.clear();
      return self.Fail(error, "trace event changed after validation");
    }
    event.kind = EventKind(header.subtype);
    event.sequence = header.sequence;
    return true;
  }
  return self.Fail(error, "trace footer is missing after validation");
}
bool Reader::ReadBlob(uint64_t id, std::vector<uint8_t>& bytes, std::string& error) {
  error.clear();
  bytes.clear();
  auto& self = *impl_;
  if (!self.opened || self.failed) return self.Fail(error, "trace reader is not open");
  // A caller can validate an optional reference without poisoning subsequent reads.
  if (!id || id > self.blobs.size()) { error = "unknown trace blob ID"; return false; }
  const auto& blob = self.blobs[size_t(id - 1)];
  try { bytes.resize(size_t(blob.size)); }
  catch (const std::bad_alloc&) { return self.Fail(error, "could not allocate trace blob"); }
  if (!Seek(self.blob_stream, blob.offset) || !ReadExact(self.blob_stream, bytes) || Hash(bytes) != blob.hash) {
    bytes.clear();
    return self.Fail(error, "trace blob changed after validation");
  }
  return true;
}
bool Reader::Finished() const { return impl_->opened && impl_->finished && !impl_->failed; }
Statistics Reader::GetStatistics() const { return impl_->stats; }

}  // namespace xn::gfx::trace
