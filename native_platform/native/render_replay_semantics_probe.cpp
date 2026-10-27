// CPU-only schema regressions. Generates synthetic data and invokes the real
// replay executable with --validate-only; no game payload or GPU is required.
#include "gfx/render_trace.h"

#include <xxhash.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace trace = xn::gfx::trace;
using K = trace::EventKind;
constexpr uint32_t kPageBytes = 4096, kRegisters = 0x5003;
void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
struct Scratch {
  std::filesystem::path path;
  Scratch() {
    std::string pattern = (std::filesystem::temp_directory_path() / "xn-replay-semantics-XXXXXX").string();
    const char* directory = mkdtemp(pattern.data());
    Check(directory != nullptr, "could not create fixture directory");
    path = directory;
  }
  ~Scratch() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
struct Fixture {
  trace::Writer writer;
  std::string error;
  explicit Fixture(const std::filesystem::path& path) : writer(path) {
    Check(writer.Open(error), error);
  }
  uint64_t Blob(std::span<const uint8_t> bytes) {
    const uint64_t id = writer.PutBlob(bytes, error);
    Check(id != 0, error);
    return id;
  }
  void Event(K kind, const trace::ByteWriter& payload) {
    Check(writer.WriteEvent(kind, payload.Data(), error), error);
  }
  void Finish() { Check(writer.Finish(error), error); }
};

std::vector<uint8_t> Page(uint32_t version, const std::string& fault, bool literal, uint8_t value) {
  std::vector<uint8_t> raw(kPageBytes);
  raw[0] = value; raw.back() = 9;
  if (version == 1) return raw;
  trace::ByteWriter patch;
  if (fault == "patch_unknown") { patch.U32(2); return patch.Take(); }
  if (fault == "patch_literal_short") {
    patch.U32(0); patch.Bytes(std::span(raw).first(raw.size() - 1)); return patch.Take();
  }
  if (fault == "patch_count_overflow") { patch.U32(1); patch.U32(UINT32_MAX); return patch.Take(); }
  if (fault == "patch_overlap") {
    patch.U32(1); patch.U32(2);
    patch.U32(4); patch.U32(2); patch.Bytes(std::array<uint8_t, 2>{1, 2});
    patch.U32(5); patch.U32(1); patch.Bytes(std::array<uint8_t, 1>{3});
    return patch.Take();
  }
  if (fault == "patch_bounds" || fault == "patch_offset_overflow" || fault == "patch_zero_length" ||
      fault == "patch_truncated_run") {
    patch.U32(1); patch.U32(1);
    patch.U32(fault == "patch_offset_overflow" ? UINT32_MAX : 4095);
    patch.U32(fault == "patch_zero_length" ? 0 : 2);
    patch.Bytes(fault == "patch_truncated_run" ? std::span(raw).first(1) : std::span(raw).first(2));
    return patch.Take();
  }
  if (fault == "patch_empty") { patch.U32(1); patch.U32(0); return patch.Take(); }
  if (literal) { patch.U32(0); patch.Bytes(raw); }
  else {
    patch.U32(1); patch.U32(2);
    patch.U32(0); patch.U32(1); patch.Bytes(std::span(raw).first(1));
    patch.U32(4095); patch.U32(1); patch.Bytes(std::span(raw).last(1));
  }
  if (fault == "patch_trailing") patch.U32(123);
  return patch.Take();
}

void WriteFixture(const std::filesystem::path& path, uint32_t version, bool literal,
                  const std::string& fault) {
  Fixture fixture(path);
  const bool host = fault.starts_with("host_");
  trace::ByteWriter info;
  info.U32(fault == "unsupported_version" ? 99 : version);
  info.U64(UINT64_C(512) << 20); info.U32(fault == "swap_count" ? 3 : 2);
  if (host) info.U32(fault == "host_unknown_flag" ? 8 : fault == "host_no_flag" ? 0 : 1);
  fixture.Event(K::kCaptureInfo, info);
  auto memory = [&](const std::string& patch_fault, uint8_t value) {
    const auto page = Page(version, patch_fault, literal, value);
    trace::ByteWriter payload;
    payload.U32(patch_fault == "page_bounds" ? 131072 : 0);
    payload.U64(patch_fault == "dangling_blob" ? 9999 : fixture.Blob(page));
    fixture.Event(K::kMemoryPage, payload);
  };
  memory(fault, 17);
  std::vector<uint8_t> registers(size_t(kRegisters) * 4);
  trace::ByteWriter register_file;
  register_file.U64(fixture.Blob(fault == "register_blob_size" ? std::span(registers).first(4) : std::span(registers)));
  if (fault != "missing_registers") fixture.Event(K::kRegisterFile, register_file);
  if (fault == "snapshot") fixture.Event(K::kRegisterFile, register_file);
  if (fault == "register_bounds") {
    trace::ByteWriter write; write.U32(kRegisters); write.U32(1); fixture.Event(K::kRegisterWrite, write);
  }
  if (fault == "batch_overflow" || fault == "batch_side_effect") {
    trace::ByteWriter batch;
    batch.U32(fault == "batch_overflow" ? UINT32_MAX : 1);
    if (fault == "batch_side_effect") { batch.U32(0x1921); batch.U32(0); }
    fixture.Event(K::kRegisterBatch, batch);
  }
  auto host_inputs = [&] {
    if (!host) return;
    auto key = [](trace::ByteWriter& bytes) {
      bytes.U32(0x12000); bytes.U32(1); bytes.U32(1); bytes.U32(6);
    };
    trace::ByteWriter revision; key(revision);
    revision.U64(fault == "host_zero" ? 0 : 7);
    if (fault != "host_read_without_revision") fixture.Event(K::kTextureRevision, revision);
    if (fault == "host_duplicate_key") fixture.Event(K::kTextureRevision, revision);
    if (fault == "host_missing_read" || fault == "host_zero") return;
    trace::ByteWriter read; key(read);
    read.U64(fault == "host_revision_mismatch" ? 8 : 7);
    const bool success = fault != "host_read_false" && fault != "host_failed_read_blob";
    read.U32(success ? 1 : 0);
    // These are opaque native-format bytes; they intentionally are not RGBA.
    const auto id = fixture.Blob(std::array<uint8_t, 3>{7, 8, 9});
    read.U64(success || fault == "host_failed_read_blob" ? id : 0);
    fixture.Event(K::kTextureRead, read);
  };
  auto draw = [&] {
    trace::ByteWriter packet;
    packet.U32(fault == "draw_opcode" ? 0 : 0x22); packet.U32(4); packet.U32(3);
    packet.U32(fault == "draw_immediate_short" ? 1 : fault == "draw_dma_bounds" ? 0 : 2);
    packet.U32(fault == "draw_dma_bounds" ? (512u << 20) - 2 : 0);
    packet.U32(0); packet.U32(fault == "draw_boolean" ? 2 : 0);
    packet.U32(0); packet.U32(0); packet.U32(1); packet.U64(0);
    fixture.Event(K::kDraw, packet);
  };
  auto swap = [&] {
    trace::ByteWriter payload;
    payload.U32(0); payload.U32(fault == "swap_dimensions" ? 0 : 1); payload.U32(1);
    fixture.Event(K::kSwap, payload);
  };
  auto reference = [&] {
    const std::array<uint8_t, 4> pixels{42, 23, 0, 255};
    trace::ByteWriter payload;
    payload.U32(1); payload.U32(1);
    payload.U64(XXH3_64bits(pixels.data(), pixels.size()) + (fault == "reference_hash" ? 1 : 0));
    payload.U64(fixture.Blob(fault == "reference_size" ? std::span(pixels).first(3) : std::span(pixels)));
    fixture.Event(K::kFrameReference, payload);
  };
  host_inputs(); draw();
  if (fault == "early_reference") reference();
  swap(); memory({}, 42); draw(); swap();
  if (fault == "late_memory") memory({}, 43);
  reference();
  if (fault != "missing_end") {
    trace::ByteWriter end;
    if (fault == "end_payload") end.U32(1);
    fixture.Event(K::kEndCapture, end);
  }
  if (fault == "after_end") draw();
  fixture.Finish();
}

int Run(const std::filesystem::path& runner, const std::filesystem::path& trace,
        const std::filesystem::path& log) {
  const pid_t pid = fork();
  Check(pid >= 0, "could not fork replay validation");
  if (!pid) {
    const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(126);
    close(fd);
    execl(runner.c_str(), runner.c_str(), trace.c_str(), "--validate-only", static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) Check(errno == EINTR, "could not wait for replay validation");
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
std::string ReadLog(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string result{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (result.size() > 2048) result.resize(2048);
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Check(argc == 2, "usage: render_replay_semantics_probe REPLAY_EXECUTABLE");
    const auto runner = std::filesystem::absolute(argv[1]);
    Check(std::filesystem::exists(runner), "replay executable does not exist");
    Scratch scratch;
    size_t passed = 0;
    auto test = [&](const std::string& name, uint32_t version, bool literal,
                    const std::string& fault, bool valid) {
      const auto path = scratch.path / (name + ".trace");
      const auto log = scratch.path / (name + ".log");
      WriteFixture(path, version, literal, fault);
      const int result = Run(runner, path, log);
      Check(result == (valid ? 0 : 1), name + " returned " + std::to_string(result) + ": " + ReadLog(log));
      const auto output = ReadLog(log);
      Check(output.find(valid ? "\"semantics_valid\":true" : "\"verified\":false") != std::string::npos,
            name + " did not report its validation result: " + output);
      ++passed;
    };
    test("valid-v1", 1, false, {}, true);
    test("valid-v2-sparse", 2, false, {}, true);
    test("valid-v2-literal", 2, true, {}, true);
    test("valid-v2-empty-patch", 2, false, "patch_empty", true);
    test("valid-repeat-snapshot", 2, false, "snapshot", true);
    for (const std::string mode : {"host_valid", "host_zero", "host_read_false"})
      test(mode, 2, false, mode, true);
    for (const std::string fault : {
        "unsupported_version", "patch_unknown", "patch_literal_short", "patch_count_overflow",
        "patch_overlap", "patch_bounds", "patch_offset_overflow", "patch_zero_length",
        "patch_truncated_run", "patch_trailing", "page_bounds", "dangling_blob", "register_blob_size",
        "missing_registers", "register_bounds", "batch_overflow", "batch_side_effect", "draw_opcode",
        "draw_immediate_short", "draw_dma_bounds", "draw_boolean", "swap_dimensions", "swap_count",
        "early_reference", "reference_hash", "reference_size", "late_memory", "missing_end", "end_payload",
        "after_end", "host_unknown_flag", "host_no_flag", "host_missing_read", "host_read_without_revision",
        "host_revision_mismatch", "host_duplicate_key", "host_failed_read_blob"})
      test(fault, 2, false, fault, false);
    std::cout << "render replay semantics probe: " << passed << " CPU checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render replay semantics probe: " << error.what() << '\n';
    return 1;
  }
}
