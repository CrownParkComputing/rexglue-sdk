// Standalone synthetic trace-format regression probe. No game assets are used.
#include "gfx/render_trace.h"

#include <xxhash.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xn::gfx::trace;

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
void Ok(bool condition, const std::string& error, const std::string& operation) {
  Check(condition, operation + ": " + error);
}
struct Scratch {
  std::filesystem::path path;
  Scratch() {
    std::string pattern = (std::filesystem::temp_directory_path() /
                           "xn-render-trace-probe-XXXXXX").string();
    const char* directory = mkdtemp(pattern.data());
    Check(directory != nullptr, "could not create synthetic fixture directory");
    path = directory;
  }
  ~Scratch() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
uint64_t ConstantHash(std::span<const uint8_t>) { return 7; }
uint64_t Load64(const uint8_t* in) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= uint64_t(in[i]) << (i * 8);
  return value;
}
void Store32(uint8_t* out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) out[i] = uint8_t(value >> (i * 8));
}
void Store64(uint8_t* out, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) out[i] = uint8_t(value >> (i * 8));
}
std::vector<uint8_t> FileBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void Save(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  Check(bool(stream), "could not write synthetic fixture");
}
void FixRecordHeader(std::vector<uint8_t>& bytes, size_t offset) {
  Store64(bytes.data() + offset + 32, XXH3_64bits(bytes.data() + offset, 32));
}
void FixRecordPayload(std::vector<uint8_t>& bytes, size_t offset) {
  const size_t size = size_t(Load64(bytes.data() + offset + 16));
  Store64(bytes.data() + offset + 24, XXH3_64bits(bytes.data() + offset + 40, size));
  FixRecordHeader(bytes, offset);
}

void TestBytes() {
  ByteWriter writer;
  writer.U32(0x12345678);
  writer.U64(UINT64_C(0x123456789abcdef0));
  writer.String(std::string("a\0b", 3));
  const std::array<uint8_t, 19> expected{
      0x78, 0x56, 0x34, 0x12, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
      3, 0, 0, 0, 'a', 0, 'b'};
  Check(std::equal(expected.begin(), expected.end(), writer.Data().begin(), writer.Data().end()),
        "scalar encoding is not explicit little endian");
  ByteReader reader(writer.Data());
  uint32_t u32 = 0;
  uint64_t u64 = 0;
  std::string value;
  Check(reader.U32(u32) && u32 == 0x12345678 && reader.U64(u64) &&
            u64 == UINT64_C(0x123456789abcdef0) && reader.String(value) &&
            value == std::string("a\0b", 3) && reader.Done(), "scalar round trip");
  Check(!reader.U32(u32) && !reader.Valid() && !reader.Done(), "over-read must invalidate parser");
  std::span<const uint8_t> out;
  Check(!reader.Bytes(0, out), "invalid byte reader must remain invalid");
  ByteReader truncated(std::span(writer.Data()).first(3));
  Check(!truncated.U32(u32), "truncated integer was accepted");
  ByteReader bounded(std::span(writer.Data()).subspan(12));
  Check(!bounded.String(value, 2) && !bounded.Valid(), "oversized string was accepted");
}

void TestRoundTrip(const std::filesystem::path& path) {
  std::string error;
  std::vector<uint8_t> a(160 * 1024), b;
  for (size_t i = 0; i < a.size(); ++i) a[i] = uint8_t(i * 31 + i / 1024);
  b = a;
  b[100 * 1024] ^= 0x7f;  // Colliding candidate comparison must reach beyond its first chunk.
  std::vector<std::pair<EventKind, std::vector<uint8_t>>> expected;
  uint64_t a_id = 0, b_id = 0, empty_id = 0;
  Statistics written;
  {
    Writer writer(path, {}, ConstantHash);
    Ok(writer.Open(error), error, "open writer");
    empty_id = writer.PutBlob({}, error);
    Check(empty_id != 0, "empty blob needs a valid ID");
    a_id = writer.PutBlob(a, error);
    b_id = writer.PutBlob(b, error);
    Check(a_id && b_id && a_id != b_id, "forced hash collision aliased distinct data");
    Check(writer.PutBlob(a, error) == a_id && writer.PutBlob(b, error) == b_id &&
          writer.PutBlob({}, error) == empty_id, "identical blob did not deduplicate");
    auto event = [&](EventKind kind, ByteWriter& bytes) {
      expected.emplace_back(kind, bytes.Data());
      Ok(writer.WriteEvent(kind, bytes.Data(), error), error, "write event");
    };
    ByteWriter info;
    info.U32(1); info.U32(8); info.U32(4096); info.String("synthetic renderer");
    event(EventKind::kCaptureInfo, info);
    ByteWriter memory;
    memory.U32(0x2000); memory.U32(uint32_t(a.size())); memory.U64(a_id);
    event(EventKind::kMemoryPage, memory);
    ByteWriter registers;
    registers.U32(0); registers.U32(2); registers.U32(0x11223344); registers.U32(0x55667788);
    event(EventKind::kRegisterFile, registers);
    ByteWriter draw;
    draw.U32(3); draw.U32(6); draw.U32(0xdeadbeef); draw.U64(b_id);
    const std::array<uint8_t, 4> immediate{0x81, 0x02, 0xff, 0};
    draw.U32(uint32_t(immediate.size())); draw.Bytes(immediate);
    event(EventKind::kDraw, draw);
    for (uint32_t frame = 0; frame < 9; ++frame) {
      ByteWriter swap;
      swap.U32(frame); swap.U32(4); swap.U32(4);
      event(EventKind::kSwap, swap);
      ByteWriter reference;
      reference.U32(frame); reference.U64(UINT64_C(0x1122334400000000) + frame); reference.U64(empty_id);
      event(EventKind::kFrameReference, reference);
    }
    ByteWriter end;
    event(EventKind::kEndCapture, end);
    Ok(writer.Finish(error), error, "finish trace");
    written = writer.GetStatistics();
    Check(written.blobs == 3 && written.events == expected.size() &&
          written.records == written.blobs + written.events &&
          written.unique_blob_bytes == a.size() + b.size() &&
          written.deduplicated_blob_bytes == a.size() + b.size(), "writer statistics");
  }
  Reader reader(path);
  Ok(reader.Open(error), error, "validate trace");
  Check(!reader.Finished(), "reader starts finished");
  std::vector<uint8_t> bytes;
  Check(!reader.ReadBlob(0, bytes, error) && !error.empty() && bytes.empty(), "zero blob ID accepted");
  Check(!reader.ReadBlob(4, bytes, error) && !error.empty(), "unknown blob ID accepted");
  Ok(reader.ReadBlob(empty_id, bytes, error), error, "read empty blob");
  Check(bytes.empty(), "empty blob gained data");
  for (unsigned iteration = 0; iteration < 2; ++iteration) {
    Event event;
    uint64_t last_sequence = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
      Ok(reader.Next(event, error), error, "read event");
      Check(event.kind == expected[i].first && event.payload == expected[i].second &&
            (i == 0 || event.sequence > last_sequence), "event bytes/order changed");
      last_sequence = event.sequence;
      Ok(reader.ReadBlob(i % 2 ? a_id : b_id, bytes, error), error, "interleaved blob read");
      Check(bytes == (i % 2 ? a : b), "blob bytes changed");
    }
    Check(!reader.Next(event, error) && error.empty() && reader.Finished(), "verified end not reported");
    Check(!reader.Next(event, error) && error.empty(), "end iterator is not stable");
    if (!iteration) Ok(reader.Rewind(error), error, "rewind");
  }
  const auto read = reader.GetStatistics();
  Check(read.file_bytes == written.file_bytes && read.records == written.records &&
        read.events == written.events && read.blobs == written.blobs &&
        read.unique_blob_bytes == written.unique_blob_bytes, "reader statistics");
  // Hundreds of KiB of repeated content must occupy exactly two distinct raw blobs.
  Check(written.file_bytes < a.size() * 3, "deduplication did not reduce storage");
}

std::vector<uint8_t> Fixture(const std::filesystem::path& path) {
  std::string error;
  Writer writer(path);
  Ok(writer.Open(error), error, "fixture open");
  const std::array<uint8_t, 16> blob{1, 2, 3, 4, 5, 6, 7, 8};
  const uint64_t id = writer.PutBlob(blob, error);
  Check(id == 1, "fixture blob");
  ByteWriter info;
  info.U32(1); info.U32(8);
  Ok(writer.WriteEvent(EventKind::kCaptureInfo, info.Data(), error), error, "fixture info");
  ByteWriter memory;
  memory.U32(4096); memory.U32(16); memory.U64(id);
  Ok(writer.WriteEvent(EventKind::kMemoryPage, memory.Data(), error), error, "fixture memory");
  Ok(writer.Finish(error), error, "fixture finish");
  return FileBytes(path);
}

void TestMalformed(const std::filesystem::path& directory) {
  const auto base = Fixture(directory / "valid.trace");
  const auto path = directory / "bad.trace";
  auto reject = [&](std::span<const uint8_t> bytes, const std::string& reason) {
    Save(path, bytes);
    Reader reader(path);
    std::string error;
    Check(!reader.Open(error) && !error.empty() && !reader.Finished(), "accepted " + reason);
    Event event;
    Check(!reader.Next(event, error), "exposed events from " + reason);
  };
  // Test every truncation of the small synthetic fixture, including record boundaries.
  for (size_t n = 0; n < base.size(); ++n) reject(std::span(base).first(n), "truncated trace");
  for (size_t offset : {size_t(0), size_t(8), size_t(12), size_t(16), size_t(56),
                        size_t(64), size_t(64 + 24), size_t(64 + 32), size_t(64 + 40 + 24),
                        base.size() - 1}) {
    auto changed = base;
    changed[offset] ^= 0x80;
    reject(changed, "checksum corruption");
  }
  auto changed = base;
  changed.push_back(0);
  reject(changed, "trailing bytes");
  // Repair outer checksums to exercise semantic bounds independently of integrity checks.
  changed = base;
  Store32(changed.data() + 8, 99);
  Store64(changed.data() + 56, XXH3_64bits(changed.data(), 56));
  reject(changed, "unsupported version");
  changed = base;
  changed[16] = 1;
  Store64(changed.data() + 56, XXH3_64bits(changed.data(), 56));
  reject(changed, "unknown header flags");
  const size_t blob_offset = 64;
  const size_t event_offset = blob_offset + 40 + size_t(Load64(base.data() + blob_offset + 16));
  changed = base;
  Store32(changed.data() + event_offset + 4, 999);
  FixRecordHeader(changed, event_offset);
  reject(changed, "invalid event kind");
  changed = base;
  Store32(changed.data() + blob_offset, 999);
  FixRecordHeader(changed, blob_offset);
  reject(changed, "invalid record kind");
  changed = base;
  Store64(changed.data() + blob_offset + 40, 0);
  FixRecordPayload(changed, blob_offset);
  reject(changed, "zero blob ID");
  changed = base;
  Store64(changed.data() + blob_offset + 40, 2);
  FixRecordPayload(changed, blob_offset);
  reject(changed, "nonconsecutive blob ID");
  changed = base;
  Store64(changed.data() + blob_offset + 48, 15);
  FixRecordPayload(changed, blob_offset);
  reject(changed, "inconsistent blob length");
  changed = base;
  Store64(changed.data() + blob_offset + 56, UINT64_C(0x12345678));
  FixRecordPayload(changed, blob_offset);
  reject(changed, "invalid blob content hash");
  changed = base;
  Store64(changed.data() + event_offset + 8, 888);
  FixRecordHeader(changed, event_offset);
  reject(changed, "invalid sequence");
  changed = base;
  Store64(changed.data() + event_offset + 16, UINT64_MAX);
  FixRecordHeader(changed, event_offset);
  reject(changed, "overflowing event size");
  const size_t footer_offset = base.size() - 72;
  changed = base;
  Store64(changed.data() + footer_offset + 40, 888);
  FixRecordPayload(changed, footer_offset);
  reject(changed, "invalid footer counts");
  changed = base;
  Store64(changed.data() + footer_offset + 64, UINT64_C(0xbadf00d));
  FixRecordPayload(changed, footer_offset);
  reject(changed, "invalid chained record checksum");
}

void TestLimits(const std::filesystem::path& directory) {
  std::string error;
  const std::array<uint8_t, 5> bytes{1, 2, 3, 4, 5};
  auto writer_case = [&](Limits limits, bool blob) {
    const auto path = directory / "writer-limit.trace";
    {
      Writer writer(path, limits);
      Ok(writer.Open(error), error, "limit writer open");
      Check(blob ? writer.PutBlob(bytes, error) == 0 : !writer.WriteEvent(EventKind::kDraw, bytes, error),
            "writer exceeded configured limit");
      Check(!error.empty() && !writer.Finish(error), "failed writer was allowed to finish");
    }
    Reader reader(path);
    Check(!reader.Open(error), "failed writer produced a complete trace");
  };
  Limits limits;
  limits.max_blob_bytes = 4;
  writer_case(limits, true);
  limits = {}; limits.max_event_bytes = 4;
  writer_case(limits, false);
  limits = {}; limits.max_blobs = 0;
  writer_case(limits, true);
  limits = {}; limits.max_events = 0;
  writer_case(limits, false);
  limits = {}; limits.max_file_bytes = 136;
  writer_case(limits, false);
  {
    Writer writer(directory / "empty.trace", limits);
    Ok(writer.Open(error), error, "minimal trace open");
    Ok(writer.Finish(error), error, "minimal trace finish");
    Reader reader(directory / "empty.trace", limits);
    Ok(reader.Open(error), error, "minimal trace read");
    Event event;
    Check(!reader.Next(event, error) && error.empty() && reader.Finished(), "minimal trace iterator");
  }
  const auto valid = directory / "limits-valid.trace";
  const auto fixture = Fixture(valid);
  auto reader_case = [&](Limits bounded) {
    Reader reader(valid, bounded);
    Check(!reader.Open(error) && !error.empty(), "reader exceeded configured limit");
  };
  limits = {}; limits.max_file_bytes = fixture.size() - 1; reader_case(limits);
  limits = {}; limits.max_blob_bytes = 15; reader_case(limits);
  limits = {}; limits.max_event_bytes = 7; reader_case(limits);
  limits = {}; limits.max_blobs = 0; reader_case(limits);
  limits = {}; limits.max_events = 1; reader_case(limits);
  {
    const auto path = directory / "unfinished.trace";
    { Writer writer(path); Ok(writer.Open(error), error, "unfinished open");
      Check(writer.PutBlob(bytes, error) != 0, "unfinished blob"); }
    Reader reader(path);
    Check(!reader.Open(error), "unfinished writer was accepted");
  }
  {
    Writer writer(directory / "invalid-kind.trace");
    Ok(writer.Open(error), error, "kind writer open");
    Check(!writer.WriteEvent(EventKind(999), {}, error), "writer accepted unknown event kind");
  }
}

void TestChangedAfterOpen(const std::filesystem::path& directory) {
  const auto path = directory / "changed.trace";
  auto base = Fixture(path);
  std::string error;
  {
    Reader reader(path);
    Ok(reader.Open(error), error, "mutation reader open");
    base[64 + 40 + 24] ^= 1;
    Save(path, base);
    std::vector<uint8_t> bytes;
    Check(!reader.ReadBlob(1, bytes, error) && bytes.empty(), "changed blob was replayed");
  }
  base = Fixture(path);
  {
    Reader reader(path);
    Ok(reader.Open(error), error, "event mutation reader open");
    const size_t first_event = 64 + 40 + size_t(Load64(base.data() + 64 + 16));
    base[first_event + 40] ^= 1;
    Save(path, base);
    Event event;
    Check(!reader.Next(event, error) && event.payload.empty(), "changed event was replayed");
  }
}

}  // namespace

int main() {
  try {
    Scratch scratch;
    TestBytes();
    TestRoundTrip(scratch.path / "round-trip.trace");
    TestMalformed(scratch.path);
    TestLimits(scratch.path);
    TestChangedAfterOpen(scratch.path);
    std::cout << "render trace probe: all synthetic checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render trace probe: " << error.what() << '\n';
    return 1;
  }
}
