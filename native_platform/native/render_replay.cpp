// Headless replay of the renderer seam. Inputs are renderer traces, never game files.
#include "gfx/render_trace.h"
#include "gfx/vulkan/vulkan_renderer.h"
#include "memory/physical_write_watch.h"

#include <xxhash.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" void xn_cpu_api_audit_checkpoint(const char*, uint64_t) __attribute__((weak));

namespace {
namespace trace = xn::gfx::trace;
constexpr uint64_t kPhysicalBytes = UINT64_C(512) << 20;
constexpr uint32_t kPageBytes = xn::PhysicalWriteWatch::kPageSize;
constexpr uint32_t kMaximumSwaps = 10000;
constexpr uint32_t kMaximumShaderWords = 1 << 20;
constexpr uint64_t kInputLimit = UINT64_C(256) << 20;

void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
void Checked(bool condition, const std::string& error, const char* operation) {
  Require(condition, std::string(operation) + ": " + error);
}
uint64_t Nanoseconds(clockid_t clock) {
  timespec time{};
  Require(clock_gettime(clock, &time) == 0, "clock_gettime failed");
  return uint64_t(time.tv_sec) * UINT64_C(1000000000) + uint64_t(time.tv_nsec);
}
std::string JsonString(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c < 0x20) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    else out << c;
  }
  out << '"';
  return out.str();
}
std::string Hex64(uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << value;
  return out.str();
}
void RendererLog(int level, const char* message) {
  if (level >= 3 || std::getenv("XN_REPLAY_VERBOSE")) std::fprintf(stderr, "%s\n", message);
}

struct Options {
  std::filesystem::path trace, cache, output;
  uint32_t repeat = 1;
  bool validate_only = false;
};
Options ParseOptions(int argc, char** argv) {
  Require(argc >= 2, "usage: render_replay TRACE [--repeat N] [--cache DIRECTORY] [--output DIRECTORY] [--validate-only]");
  Options options;
  options.trace = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--validate-only") { options.validate_only = true; continue; }
    Require(i + 1 < argc, "missing value for " + option);
    const std::string value = argv[++i];
    if (option == "--repeat") {
      const auto result = std::from_chars(value.data(), value.data() + value.size(), options.repeat);
      Require(result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
                  options.repeat > 0 && options.repeat <= kMaximumSwaps, "invalid repeat count");
    } else if (option == "--cache") options.cache = value;
    else if (option == "--output") options.output = value;
    else throw std::runtime_error("unknown option: " + option);
  }
  return options;
}

struct Decoded {
  struct Patch { uint32_t offset, size; size_t blob_offset; };
  trace::EventKind kind;
  std::array<uint32_t, 10> fields{};
  uint64_t blob_id = 0, physical_bytes = 0, expected_hash = 0;
  uint64_t revision = 0;
  std::vector<uint8_t> blob;
  std::vector<uint32_t> words;
  std::vector<xn::RegisterRange> ranges;
  std::vector<Patch> patches;
};
bool BatchRegister(uint32_t index) {
  return (index >= 0x2000 && index < 0x2800) || (index >= 0x4000 && index < 0x4928);
}
uint64_t ImageBytes(uint32_t width, uint32_t height) {
  Require(width && height && width <= 16384 && height <= 16384, "invalid image dimensions");
  return uint64_t(width) * height * 4;
}
Decoded Decode(trace::Reader& reader, const trace::Event& event, uint32_t version = 1) {
  Decoded result{};
  result.kind = event.kind;
  trace::ByteReader bytes(event.payload);
  auto u32 = [&](size_t index) { Require(bytes.U32(result.fields.at(index)), "truncated event scalar"); };
  auto blob = [&](uint64_t expected_bytes) {
    std::string error;
    Checked(reader.ReadBlob(result.blob_id, result.blob, error), error, "read referenced blob");
    Require(result.blob.size() == expected_bytes, "referenced blob size does not match event");
  };
  using K = trace::EventKind;
  switch (event.kind) {
    case K::kCaptureInfo:
      u32(0);
      Require(bytes.U64(result.physical_bytes), "truncated physical-memory size");
      u32(1);
      if (bytes.Remaining()) u32(2);
      Require((result.fields[0] == 1 || result.fields[0] == 2) && result.physical_bytes == kPhysicalBytes &&
                  result.fields[1] > 0 && result.fields[1] <= kMaximumSwaps &&
                  (result.fields[2] & ~1u) == 0,
              "unsupported capture version, physical-memory size, or swap count");
      break;
    case K::kMemoryPage:
      u32(0);
      Require(bytes.U64(result.blob_id), "truncated memory-page blob ID");
      Require(result.fields[0] < kPhysicalBytes / kPageBytes, "physical page is out of range");
      if (version == 1) {
        blob(kPageBytes);
        result.patches.push_back({0, kPageBytes, 0});
      } else {
        std::string error;
        Checked(reader.ReadBlob(result.blob_id, result.blob, error), error, "read compressed page patch");
        Require(result.blob.size() <= 8 + uint64_t(kPageBytes) * 9, "encoded physical page patch is oversized");
        trace::ByteReader patch(result.blob);
        uint32_t encoding = 0;
        Require(patch.U32(encoding), "truncated page patch encoding");
        if (encoding == 0) {
          std::span<const uint8_t> literal;
          Require(patch.Bytes(kPageBytes, literal), "truncated literal page patch");
          result.patches.push_back({0, kPageBytes, 4});
        } else if (encoding == 1) {
          uint32_t count = 0, end = 0;
          Require(patch.U32(count) && count <= kPageBytes, "invalid page patch run count");
          result.patches.reserve(count);
          for (uint32_t i = 0; i < count; ++i) {
            uint32_t offset = 0, size = 0;
            Require(patch.U32(offset) && patch.U32(size), "truncated page patch run");
            Require(offset >= end && offset < kPageBytes && size && size <= kPageBytes - offset,
                    "page patch runs overlap or exceed page bounds");
            const size_t blob_offset = result.blob.size() - patch.Remaining();
            std::span<const uint8_t> data;
            Require(patch.Bytes(size, data), "truncated page patch run bytes");
            result.patches.push_back({offset, size, blob_offset});
            end = offset + size;
          }
        } else throw std::runtime_error("unknown page patch encoding");
        Require(patch.Done(), "trailing bytes after page patch");
      }
      break;
    case K::kRegisterFile: {
      Require(bytes.U64(result.blob_id), "truncated register-file blob ID");
      blob(uint64_t(xn::GpuRegisterFile::kCount) * 4);
      result.words.resize(xn::GpuRegisterFile::kCount);
      trace::ByteReader registers(result.blob);
      for (auto& value : result.words) Require(registers.U32(value), "truncated register file");
      Require(registers.Done(), "trailing register-file bytes");
      std::vector<uint8_t>().swap(result.blob);
      break;
    }
    case K::kRegisterWrite:
      u32(0); u32(1);
      Require(result.fields[0] < xn::GpuRegisterFile::kCount, "register index is out of range");
      break;
    case K::kRegisterBatch:
      u32(0);
      Require(result.fields[0] <= xn::GpuRegisterFile::kCount, "register batch is oversized");
      result.words.resize(size_t(result.fields[0]) * 2);
      result.ranges.reserve(result.fields[0]);
      for (size_t i = 0; i < result.words.size(); i += 2) {
        Require(bytes.U32(result.words[i]) && bytes.U32(result.words[i + 1]), "truncated register batch");
        Require(BatchRegister(result.words[i]), "register batch includes a register with side effects");
        const uint32_t index = result.words[i];
        if (!result.ranges.empty() && result.ranges.back().index + result.ranges.back().count == index)
          ++result.ranges.back().count;
        else result.ranges.push_back({index, 1});
      }
      break;
    case K::kShaderLoad:
      u32(0); u32(1); u32(2);
      Require(bytes.U64(result.blob_id), "truncated shader blob ID");
      Require(result.fields[0] <= 1 && result.fields[2] <= kMaximumShaderWords &&
                  result.fields[1] <= kPhysicalBytes && uint64_t(result.fields[2]) * 4 <=
                      kPhysicalBytes - result.fields[1], "invalid shader type, address, or length");
      blob(uint64_t(result.fields[2]) * 4);
      result.words.resize(result.fields[2]);
      if (!result.blob.empty()) std::memcpy(result.words.data(), result.blob.data(), result.blob.size());
      std::vector<uint8_t>().swap(result.blob);
      break;
    case K::kDraw: {
      for (size_t i = 0; i < result.fields.size(); ++i) u32(i);
      Require(bytes.U64(result.blob_id), "truncated immediate-data blob ID");
      const auto& f = result.fields;
      Require((f[0] == 0x22 || f[0] == 0x36) && f[1] <= 0x3f && f[2] <= 0xffff &&
                  f[3] <= 2 && f[6] <= 1 && f[7] <= 3 && f[8] <= 16384 && f[9] <= 1,
              "invalid draw packet scalar");
      if (f[3] == 0) {
        Require(f[4] < kPhysicalBytes && uint64_t(f[2]) * (f[6] ? 4 : 2) <= kPhysicalBytes - f[4] &&
                    uint64_t(f[5]) <= kPhysicalBytes - f[4], "DMA index range is out of physical memory");
      }
      if (f[8]) {
        blob(uint64_t(f[8]) * 4);
        result.words.resize(f[8]);
        std::memcpy(result.words.data(), result.blob.data(), result.blob.size());
        std::vector<uint8_t>().swap(result.blob);
      } else Require(!result.blob_id, "empty immediate data has a nonzero blob ID");
      if (f[3] == 1)
        Require(uint64_t(f[2]) * (f[6] ? 4 : 2) <= uint64_t(f[8]) * 4,
                "immediate index count exceeds captured bytes");
      break;
    }
    case K::kGpuEvent:
      u32(0);
      Require(result.fields[0] <= 0x3f, "invalid GPU event initiator");
      break;
    case K::kSwap: {
      u32(0); u32(1); u32(2);
      const uint64_t size = ImageBytes(result.fields[1], result.fields[2]);
      Require(result.fields[0] < kPhysicalBytes && size <= kPhysicalBytes - result.fields[0],
              "swap front buffer is out of physical memory");
      break;
    }
    case K::kFrameReference:
      u32(0); u32(1);
      Require(bytes.U64(result.expected_hash) && bytes.U64(result.blob_id), "truncated frame reference");
      blob(ImageBytes(result.fields[0], result.fields[1]));
      Require(XXH3_64bits(result.blob.data(), result.blob.size()) == result.expected_hash,
              "reference pixel hash does not match reference image");
      break;
    case K::kTextureRevision:
    case K::kTextureRead:
      u32(0); u32(1); u32(2); u32(3);
      (void)ImageBytes(result.fields[1], result.fields[2]);
      Require(result.fields[3] <= 63, "host texture format is out of range");
      Require(bytes.U64(result.revision), "truncated host texture revision");
      if (event.kind == K::kTextureRead) {
        u32(4);
        Require(bytes.U64(result.blob_id), "truncated host texture blob ID");
        Require(result.revision && result.fields[4] <= 1, "invalid host texture read response");
        if (result.fields[4]) {
          std::string error;
          Checked(reader.ReadBlob(result.blob_id, result.blob, error), error, "read host texture response");
        } else Require(!result.blob_id, "failed host texture read has a blob ID");
      }
      break;
    case K::kEndCapture:
      break;
    default:
      throw std::runtime_error("texture-source traces are not supported by this physical-memory replay");
  }
  Require(bytes.Done(), "unexpected trailing bytes in event payload");
  return result;
}

struct Summary {
  uint32_t version = 1;
  uint32_t swaps = 0, width = 0, height = 0;
  uint64_t pixel_hash = 0;
  bool texture_source = false;
  std::vector<uint8_t> pixels;
};
Summary Preflight(trace::Reader& reader) {
  Summary summary;
  std::string error;
  bool info = false, registers = false, reference = false, end = false;
  using TextureKey = std::array<uint32_t, 4>;
  struct Response { uint64_t revision; bool read = false; };
  std::map<TextureKey, Response> responses;
  uint64_t response_bytes = 0;
  uint32_t swaps = 0;
  trace::Event event;
  while (reader.Next(event, error)) {
    Require(!end, "event appears after EndCapture");
    Decoded decoded = Decode(reader, event, summary.version);
    using K = trace::EventKind;
    if (!info) {
      Require(event.kind == K::kCaptureInfo, "first event must be CaptureInfo");
      summary.swaps = decoded.fields[1];
      summary.version = decoded.fields[0];
      summary.texture_source = decoded.fields[2] != 0;
      info = true;
      continue;
    }
    Require(event.kind != K::kCaptureInfo, "duplicate CaptureInfo");
    if (reference) Require(event.kind == K::kEndCapture, "event appears after final frame reference");
    if (swaps == summary.swaps)
      Require(event.kind == K::kFrameReference || event.kind == K::kEndCapture,
              "renderer input appears after final swap");
    const bool texture_event = event.kind == K::kTextureRevision || event.kind == K::kTextureRead;
    const bool callback = event.kind == K::kDraw || event.kind == K::kGpuEvent || event.kind == K::kSwap;
    if (texture_event) {
      Require(summary.texture_source && registers, "host texture response lacks source flag or register bootstrap");
      const uint64_t storage = decoded.blob.capacity() + 3 * sizeof(Decoded);
      Require(storage <= kInputLimit - response_bytes, "callback host inputs exceed 256 MiB limit");
      response_bytes += storage;
      const TextureKey key{decoded.fields[0], decoded.fields[1], decoded.fields[2], decoded.fields[3]};
      if (event.kind == K::kTextureRevision) {
        Require(responses.size() < 8192 && responses.emplace(key, Response{decoded.revision}).second,
                "duplicate or excessive host texture revision keys in callback");
      } else {
        const auto found = responses.find(key);
        Require(found != responses.end() && !found->second.read && found->second.revision == decoded.revision,
                "host texture read lacks its matching revision");
        found->second.read = true;
      }
      continue;
    }
    if (!responses.empty()) {
      Require(callback || event.kind == K::kMemoryPage, "host texture responses are detached from their callback");
      if (callback) {
        for (const auto& [key, response] : responses) {
          (void)key;
          Require(!response.revision || response.read, "nonzero host texture revision is missing its frozen read response");
        }
        responses.clear();
        response_bytes = 0;
      }
    }
    switch (event.kind) {
      case K::kMemoryPage: break;
      case K::kRegisterFile:
        // Standalone draws can provide a full snapshot again; the capture
        // emits that state before a draw whose registers_notified is false.
        registers = true;
        break;
      case K::kSwap:
        Require(registers && swaps < summary.swaps, "swap is missing bootstrap or exceeds capture count");
        ++swaps;
        break;
      case K::kFrameReference:
        Require(registers && swaps == summary.swaps && !reference, "frame reference is duplicated or early");
        summary.width = decoded.fields[0]; summary.height = decoded.fields[1];
        summary.pixel_hash = decoded.expected_hash;
        summary.pixels = std::move(decoded.blob);
        reference = true;
        break;
      case K::kEndCapture:
        Require(reference && swaps == summary.swaps, "EndCapture is missing final reference or swaps");
        end = true;
        break;
      default:
        Require(registers, "renderer input appears before register bootstrap");
        break;
    }
  }
  Checked(error.empty(), error, "preflight event stream");
  Require(reader.Finished() && info && registers && reference && end,
          "capture lacks a complete bootstrap, reference, end event, or footer");
  Checked(reader.Rewind(error), error, "rewind after preflight");
  return summary;
}

class ReplayTextureSource final : public xn::gfx::TextureUploadSource {
 public:
  void OnDraw(uint64_t, const xn::GpuRegisterFile&) override {}
  uint64_t Revision(uint32_t address, uint32_t width, uint32_t height, uint32_t format) const override {
    return Find({address, width, height, format}).revision;
  }
  bool Read(uint32_t address, uint32_t width, uint32_t height, uint32_t format,
            std::vector<uint8_t>& pixels) const override {
    const auto& response = Find({address, width, height, format});
    Require(response.read, "replay requested a missing frozen host texture read response");
    pixels.assign(response.pixels.begin(), response.pixels.end());
    return response.success;
  }
  void Hold(const Decoded& decoded) {
    const Key key{decoded.fields[0], decoded.fields[1], decoded.fields[2], decoded.fields[3]};
    if (decoded.kind == trace::EventKind::kTextureRevision) {
      Require(responses_.size() < 8192 && responses_.emplace(key, Response{decoded.revision}).second,
              "duplicate or excessive frozen host texture revision");
    } else {
      auto found = responses_.find(key);
      Require(found != responses_.end() && !found->second.read && found->second.revision == decoded.revision,
              "frozen host texture read has no matching revision");
      found->second.read = true;
      found->second.success = decoded.fields[4] != 0;
      found->second.pixels = decoded.blob;
    }
  }
  void CompleteCallback() { responses_.clear(); }
 private:
  using Key = std::array<uint32_t, 4>;
  struct Response {
    uint64_t revision;
    bool read = false, success = false;
    std::span<const uint8_t> pixels{};
  };
  const Response& Find(const Key& key) const {
    const auto found = responses_.find(key);
    Require(found != responses_.end(), "replay requested a missing frozen host texture revision key");
    return found->second;
  }
  std::map<Key, Response> responses_;
};

struct PreparedTarget {
  std::vector<Decoded> events;
  uint64_t bytes = 0;
};
PreparedTarget PrepareTarget(trace::Reader& reader, const Summary& reference) {
  // Keep the trace streaming, but prepare the one measured frame before any
  // Vulkan work. Disk latency, checksums and decoder allocations then cannot
  // masquerade as renderer CPU time or create a gap after the warmup swaps.
  Require(reference.swaps >= 2, "benchmark requires at least one warmup swap before the target frame");
  PreparedTarget target;
  uint32_t swaps = 0;
  trace::Event event;
  std::string error;
  while (reader.Next(event, error)) {
    if (swaps + 1 < reference.swaps) {
      if (event.kind == trace::EventKind::kSwap) ++swaps;
      continue;
    }
    Decoded decoded = Decode(reader, event, reference.version);
    if (event.kind == trace::EventKind::kFrameReference || event.kind == trace::EventKind::kEndCapture) continue;
    const uint64_t storage = decoded.blob.capacity() + uint64_t(decoded.words.capacity()) * 4 +
                             uint64_t(decoded.ranges.capacity()) * sizeof(xn::RegisterRange) +
                             uint64_t(decoded.patches.capacity()) * sizeof(Decoded::Patch) +
                             3 * sizeof(Decoded);  // includes vector-growth metadata overhead
    Require(storage <= kInputLimit - target.bytes, "target frame exceeds 256 MiB prepared-input limit");
    target.bytes += storage;
    if (event.kind == trace::EventKind::kSwap) ++swaps;
    target.events.push_back(std::move(decoded));
  }
  Checked(error.empty(), error, "prepare target event stream");
  Require(reader.Finished() && swaps == reference.swaps && !target.events.empty() &&
              target.events.back().kind == trace::EventKind::kSwap, "prepared target lacks its final swap");
  Checked(reader.Rewind(error), error, "rewind after target preparation");
  return target;
}

class PhysicalMemory {
 public:
  PhysicalMemory() {
    void* allocation = mmap(nullptr, kPhysicalBytes, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(allocation != MAP_FAILED, "could not mmap private 512 MiB replay physical memory");
    base_ = static_cast<uint8_t*>(allocation);
    try {
      const xn::PhysicalWriteWatch::Alias alias{base_, kPhysicalBytes, 0};
      watch_ = std::make_unique<xn::PhysicalWriteWatch>(base_, kPhysicalBytes, std::span(&alias, 1));
      Require(watch_->Enable(), "could not enable replay physical-memory write watch");
    } catch (...) { watch_.reset(); munmap(base_, kPhysicalBytes); throw; }
  }
  ~PhysicalMemory() { watch_.reset(); munmap(base_, kPhysicalBytes); }
  uint8_t* Data() const { return base_; }
  uint64_t Faults() const { return watch_->fault_count(); }
 private:
  uint8_t* base_ = nullptr;
  std::unique_ptr<xn::PhysicalWriteWatch> watch_;
};

struct Measurements {
  uint64_t wall_begin = 0, cpu_begin = 0, wall_ns = 0, cpu_ns = 0;
  uint64_t user_us = 0, system_us = 0;
  uint64_t faults_begin = 0, faults = 0;
  rusage usage_begin{};
  xn::gfx::RendererStats before{}, after{};
  bool active = false, complete = false;
  void Begin(xn::gfx::VulkanRenderer& renderer, const PhysicalMemory& memory) {
    Require(!active && !complete, "target frame timing was started twice");
    before = renderer.GetStats();
    faults_begin = memory.Faults();
    // Refresh thread CPU accounting before taking the rusage baseline; the
    // measured clock interval still begins after that snapshot.
    (void)Nanoseconds(CLOCK_THREAD_CPUTIME_ID);
    Require(getrusage(RUSAGE_THREAD, &usage_begin) == 0, "getrusage(RUSAGE_THREAD) start failed");
    wall_begin = Nanoseconds(CLOCK_MONOTONIC);
    cpu_begin = Nanoseconds(CLOCK_THREAD_CPUTIME_ID);
    active = true;
  }
  void Finish(xn::gfx::VulkanRenderer& renderer, const PhysicalMemory& memory) {
    Require(active, "target frame timing was not started");
    cpu_ns = Nanoseconds(CLOCK_THREAD_CPUTIME_ID) - cpu_begin;
    wall_ns = Nanoseconds(CLOCK_MONOTONIC) - wall_begin;
    rusage usage_end{};
    Require(getrusage(RUSAGE_THREAD, &usage_end) == 0, "getrusage(RUSAGE_THREAD) end failed");
    auto usage_micros = [](const timeval& value) {
      return uint64_t(value.tv_sec) * UINT64_C(1000000) + uint64_t(value.tv_usec);
    };
    const uint64_t user_begin = usage_micros(usage_begin.ru_utime);
    const uint64_t user_end = usage_micros(usage_end.ru_utime);
    const uint64_t system_begin = usage_micros(usage_begin.ru_stime);
    const uint64_t system_end = usage_micros(usage_end.ru_stime);
    user_us = user_end >= user_begin ? user_end - user_begin : 0;
    system_us = system_end >= system_begin ? system_end - system_begin : 0;
    after = renderer.GetStats();
    faults = memory.Faults() - faults_begin;
    active = false;
    complete = true;
  }
};

void WritePpm(const std::filesystem::path& path, std::span<const uint8_t> rgba,
              uint32_t width, uint32_t height) {
  Require(rgba.size() == ImageBytes(width, height), "cannot write malformed image");
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(bool(out), "could not create mismatch image");
  out << "P6\n" << width << ' ' << height << "\n255\n";
  std::vector<uint8_t> row(size_t(width) * 3);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x)
      std::memcpy(row.data() + size_t(x) * 3, rgba.data() + (size_t(y) * width + x) * 4, 3);
    out.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
  }
  Require(bool(out), "could not finish mismatch image");
}
void MismatchImages(const Options& options, uint32_t iteration, const Summary& reference,
                    std::span<const uint8_t> actual, uint32_t width, uint32_t height) {
  if (options.output.empty()) return;
  std::filesystem::create_directories(options.output);
  const std::string prefix = "iteration-" + std::to_string(iteration);
  WritePpm(options.output / (prefix + "-expected.ppm"), reference.pixels, reference.width, reference.height);
  if (width && height && uint64_t(width) * height * 4 == actual.size())
    WritePpm(options.output / (prefix + "-actual.ppm"), actual, width, height);
}

void PrintResult(const Options& options, const Summary& reference, uint32_t iteration,
                 const Measurements& timing, const xn::gfx::RendererStats& total,
                 const std::string& device, const PreparedTarget& target) {
  const auto& a = timing.after;
  const auto& b = timing.before;
  auto delta = [](uint64_t after, uint64_t before) { return after >= before ? after - before : 0; };
  std::cout << std::fixed << std::setprecision(6)
            << "{\"type\":\"renderer_replay\",\"iteration\":" << iteration
            << ",\"verified\":true,\"exact_pixels\":true,\"reference_xxh3\":" << JsonString(Hex64(reference.pixel_hash))
            << ",\"width\":" << reference.width << ",\"height\":" << reference.height
            << ",\"warmup_frames\":" << reference.swaps - 1 << ",\"target_frame\":" << reference.swaps
            << ",\"device\":" << JsonString(device) << ",\"pipeline_cache\":" << JsonString(options.cache.string())
            << ",\"replay_frame_wall_ms\":" << double(timing.wall_ns) / 1e6
            << ",\"replay_frame_cpu_ms\":" << double(timing.cpu_ns) / 1e6
            << ",\"replay_frame_user_ms\":" << double(timing.user_us) / 1000.0
            << ",\"replay_frame_system_ms\":" << double(timing.system_us) / 1000.0
            << ",\"replay_frame_rusage_minus_thread_cpu_ms\":"
            << (double(timing.user_us + timing.system_us) / 1000.0 - double(timing.cpu_ns) / 1e6)
            << ",\"rusage_clock_refresh\":true"
            << ",\"target_begin_monotonic_ns\":" << timing.wall_begin
            << ",\"target_end_monotonic_ns\":" << timing.wall_begin + timing.wall_ns
            << ",\"timing_scope\":\"prepared_target_events_including_memory_patches\""
            << ",\"prepared_input_bytes\":" << target.bytes
            << ",\"prepared_events\":" << target.events.size()
            << ",\"draws\":" << delta(a.draws, b.draws)
            << ",\"draws_skipped\":" << delta(a.draws_skipped, b.draws_skipped)
            << ",\"submissions\":" << delta(a.frames, b.frames)
            << ",\"pipeline_compiles\":" << delta(a.pipelines, b.pipelines)
            << ",\"shader_translations\":" << delta(a.shaders_translated, b.shaders_translated)
            << ",\"total_pipeline_compiles\":" << total.pipelines
            << ",\"total_shader_translations\":" << total.shaders_translated
            << ",\"pipelines_prewarmed\":" << total.pipelines_prewarmed
            << ",\"mirror_bytes_hashed\":" << delta(a.mirror_bytes_hashed, b.mirror_bytes_hashed)
            << ",\"constant_upload_bytes\":" << delta(a.constant_upload_bytes, b.constant_upload_bytes)
            << ",\"constant_uploads_reused\":" << delta(a.constant_uploads_reused, b.constant_uploads_reused)
            << ",\"float_constant_uploads_reused\":" << delta(a.float_constant_uploads_reused, b.float_constant_uploads_reused)
            << ",\"float_constant_bytes_avoided\":" << delta(a.float_constant_bytes_avoided, b.float_constant_bytes_avoided)
            << ",\"index_cache_hits\":" << delta(a.index_cache_hits, b.index_cache_hits)
            << ",\"texture_dirty_hits\":" << delta(a.texture_dirty_hits, b.texture_dirty_hits)
            << ",\"texture_dirty_misses\":" << delta(a.texture_dirty_misses, b.texture_dirty_misses)
            << ",\"gpu_constant_draws\":" << delta(a.gpu_constant_draws, b.gpu_constant_draws)
            << ",\"multi_draw_calls\":" << delta(a.multi_draw_calls, b.multi_draw_calls)
            << ",\"multi_draw_draws\":" << delta(a.multi_draw_draws, b.multi_draw_draws)
            << ",\"multi_draw_calls_saved\":" << delta(a.multi_draw_calls_saved, b.multi_draw_calls_saved)
            << ",\"constant_memory_flags\":" << a.constant_memory_flags
            << ",\"index_memory_flags\":" << a.index_memory_flags
            << ",\"draw_record_memory_flags\":" << a.draw_record_memory_flags
            << ",\"index_cache_misses\":" << delta(a.index_cache_misses, b.index_cache_misses)
            << ",\"converted_bytes_avoided\":" << delta(a.converted_bytes_avoided, b.converted_bytes_avoided)
            << ",\"index_ring_bytes_avoided\":" << delta(a.index_ring_bytes_avoided, b.index_ring_bytes_avoided)
            << ",\"textures_loaded\":" << delta(a.textures_loaded, b.textures_loaded)
            << ",\"textures_reuploaded\":" << delta(a.textures_reuploaded, b.textures_reuploaded)
            << ",\"gpu_completed_ms_delta\":" << double(delta(a.gpu_completed_ns, b.gpu_completed_ns)) / 1e6
            << ",\"gpu_completed_batches\":" << delta(a.gpu_completed_batches, b.gpu_completed_batches)
            << ",\"gpu_timestamp_valid_bits\":" << a.gpu_timestamp_valid_bits
            << ",\"gpu_timing_scope\":\"lagged_completed_batches\""
            << ",\"cpu_fence_wait_ms\":" << double(delta(a.cpu_fence_wait_ns, b.cpu_fence_wait_ns)) / 1e6
            << ",\"cpu_queue_submit_ms\":" << double(delta(a.cpu_queue_submit_ns, b.cpu_queue_submit_ns)) / 1e6
            << ",\"write_faults\":" << timing.faults
            << ",\"draw_state_commands_saved\":" << delta(a.draw_state_commands_saved, b.draw_state_commands_saved)
            << ",\"fused_draw_callbacks\":" << delta(a.fused_draw_callbacks, b.fused_draw_callbacks)
            << ",\"resident_index_hits\":" << delta(a.resident_index_hits, b.resident_index_hits)
            << ",\"resident_index_misses\":" << delta(a.resident_index_misses, b.resident_index_misses)
            << ",\"resident_index_upload_bytes\":" << delta(a.resident_index_upload_bytes, b.resident_index_upload_bytes)
            << ",\"resident_index_bytes_reused\":" << delta(a.resident_index_bytes_reused, b.resident_index_bytes_reused)
            << ",\"resident_index_fallbacks\":" << delta(a.resident_index_fallbacks, b.resident_index_fallbacks)
            << ",\"edram_tile_capacity\":" << a.edram_tile_capacity
            << ",\"resident_index_memory_flags\":" << a.resident_index_memory_flags
            << ",\"preparation_shader_hits\":" << delta(a.preparation_shader_hits, b.preparation_shader_hits)
            << ",\"preparation_viewport_hits\":" << delta(a.preparation_viewport_hits, b.preparation_viewport_hits)
            << ",\"preparation_system_hits\":" << delta(a.preparation_system_hits, b.preparation_system_hits)
            << ",\"preparation_pipeline_hits\":" << delta(a.preparation_pipeline_hits, b.preparation_pipeline_hits)
            << ",\"dirty_constant_uploads_reused\":" << delta(a.dirty_constant_uploads_reused, b.dirty_constant_uploads_reused)
            << ",\"float_constant_packs_avoided\":" << delta(a.float_constant_packs_avoided, b.float_constant_packs_avoided)
            << ",\"prepared_shader_lookups_avoided\":" << delta(a.prepared_shader_lookups_avoided, b.prepared_shader_lookups_avoided)
            << ",\"prepared_pipeline_lookups_avoided\":" << delta(a.prepared_pipeline_lookups_avoided, b.prepared_pipeline_lookups_avoided)
            << ",\"instance_eligible_draws\":" << delta(a.instance_profile.eligible_draws, b.instance_profile.eligible_draws)
            << ",\"instance_strict_runs\":" << delta(a.instance_profile.strict.runs, b.instance_profile.strict.runs)
            << ",\"instance_strict_calls_saved\":" << delta(a.instance_profile.strict.potential_draw_calls_saved, b.instance_profile.strict.potential_draw_calls_saved)
            << ",\"instance_max_run_snapshot\":" << a.instance_profile.strict.max_run
            << ",\"batch_profile_draws\":" << delta(a.draw_batch_profile.total_draws, b.draw_batch_profile.total_draws)
            << ",\"batch_weak_runs\":" << delta(a.draw_batch_profile.weak.runs, b.draw_batch_profile.weak.runs)
            << ",\"batch_strict_runs\":" << delta(a.draw_batch_profile.strict.runs, b.draw_batch_profile.strict.runs)
            << ",\"batch_weak_calls_saved\":" << delta(a.draw_batch_profile.weak.potential_draw_calls_saved, b.draw_batch_profile.weak.potential_draw_calls_saved)
            << ",\"batch_strict_calls_saved\":" << delta(a.draw_batch_profile.strict.potential_draw_calls_saved, b.draw_batch_profile.strict.potential_draw_calls_saved)
            << ",\"batch_pipeline_breaks\":" << delta(a.draw_batch_profile.weak.breaks.pipeline_layout, b.draw_batch_profile.weak.breaks.pipeline_layout)
            << ",\"batch_texture_breaks\":" << delta(a.draw_batch_profile.weak.breaks.textures, b.draw_batch_profile.weak.breaks.textures)
            << ",\"batch_dynamic_breaks\":" << delta(a.draw_batch_profile.weak.breaks.dynamic, b.draw_batch_profile.weak.breaks.dynamic)
            << ",\"batch_uniform_breaks\":" << delta(a.draw_batch_profile.strict.breaks.uniform_offsets, b.draw_batch_profile.strict.breaks.uniform_offsets)
            << "}\n";
}

bool Replay(trace::Reader& reader, const Summary& reference, const PreparedTarget& target,
            const Options& options, uint32_t iteration) {
  PhysicalMemory memory;
  xn::gfx::VulkanRendererConfig config;
  std::shared_ptr<ReplayTextureSource> texture_source;
  if (reference.texture_source) {
    texture_source = std::make_shared<ReplayTextureSource>();
    config.texture_source = texture_source;
  }
  config.vsync = false;
  const char* validation = std::getenv("XN_VK_VALIDATION");
  config.enable_validation = validation && validation[0] == '1';
  config.pipeline_cache_dir = options.cache;
  // Dumps/readbacks are deliberately excluded from the measured renderer callbacks.
  config.frame_dump_dir.clear();
  config.shader_dump_dir.clear();
  xn::gfx::VulkanRenderer renderer(config);
  Require(renderer.Initialize(nullptr, memory.Data(), memory.Data()), "headless Vulkan initialization failed");
  xn::GpuRegisterFile registers{};
  bool force_snapshot = true;
  uint32_t swaps = 0;
  Measurements timing;
  trace::Event event;
  std::string error;
  const char* fused_setting = std::getenv("XN_CP_DRAW_STATE_BATCH");
  const bool fuse_draw_state = fused_setting && std::strcmp(fused_setting, "1") == 0;
  std::vector<xn::RegisterRange> pending_registers;
  pending_registers.reserve(256);
  auto pending_range = [&](xn::RegisterRange range) {
    if (!pending_registers.empty()) {
      auto& previous = pending_registers.back();
      if (range.index >= previous.index && range.index <= previous.index + previous.count) {
        previous.count = std::max(previous.index + previous.count, range.index + range.count) - previous.index;
        return;
      }
    }
    pending_registers.push_back(range);
  };
  auto flush_registers = [&] {
    if (!pending_registers.empty()) {
      renderer.OnRegisterBatch(registers, pending_registers);
      pending_registers.clear();
    }
  };
  auto dispatch = [&](const Decoded& decoded) {
    const auto& f = decoded.fields;
    using K = trace::EventKind;
    switch (decoded.kind) {
      case K::kCaptureInfo: break;
      case K::kMemoryPage: {
        uint8_t* destination = memory.Data() + uint64_t(f[0]) * kPageBytes;
        xn::GuestWriteScope write(destination, kPageBytes);
        for (const auto& patch : decoded.patches)
          std::memcpy(destination + patch.offset, decoded.blob.data() + patch.blob_offset, patch.size);
        break;
      }
      case K::kRegisterFile:
        flush_registers();
        std::copy(decoded.words.begin(), decoded.words.end(), registers.values);
        force_snapshot = true;
        break;
      case K::kRegisterWrite:
        if (fuse_draw_state && BatchRegister(f[0])) {
          registers.values[f[0]] = f[1];
          pending_range({f[0], 1});
          break;
        }
        flush_registers();
        registers.values[f[0]] = f[1];
        renderer.OnRegisterWrite(f[0], f[1]);
        break;
      case K::kRegisterBatch: {
        for (size_t i = 0; i < decoded.words.size(); i += 2) {
          const uint32_t index = decoded.words[i];
          registers.values[index] = decoded.words[i + 1];
        }
        if (fuse_draw_state) {
          for (auto range : decoded.ranges) pending_range(range);
        } else renderer.OnRegisterBatch(registers, decoded.ranges);
        break;
      }
      case K::kShaderLoad:
        if (renderer.RequiresRegistersForShaderLoad()) flush_registers();
        renderer.OnShaderLoad(f[0], f[1], f[2], decoded.words.data());
        break;
      case K::kDraw: {
        xn::DrawPacket draw{};
        draw.opcode = f[0]; draw.prim_type = f[1]; draw.index_count = f[2]; draw.source = f[3];
        draw.index_base = f[4]; draw.index_size = f[5]; draw.index_32bit = f[6] != 0; draw.index_endian = f[7];
        draw.immediate = decoded.words.empty() ? nullptr : decoded.words.data(); draw.immediate_words = f[8];
        draw.registers_notified = f[9] != 0 && !force_snapshot;
        force_snapshot = false;
        if (!pending_registers.empty()) {
          renderer.OnDrawWithRegisterBatch(draw, registers, pending_registers);
          pending_registers.clear();
        } else renderer.OnDraw(draw, registers);
        if (texture_source) texture_source->CompleteCallback();
        break;
      }
      case K::kGpuEvent:
        flush_registers();
        renderer.OnEvent(f[0], registers);
        if (texture_source) texture_source->CompleteCallback();
        break;
      case K::kSwap:
        flush_registers();
        renderer.OnSwap(f[0], f[1], f[2]);
        if (texture_source) texture_source->CompleteCallback();
        ++swaps;
        break;
      case K::kFrameReference: case K::kEndCapture: break;
      case K::kTextureRevision: case K::kTextureRead:
        Require(bool(texture_source), "host texture input appears without a source proxy");
        texture_source->Hold(decoded);
        break;
      default: throw std::runtime_error("unsupported event reached replay after preflight");
    }
  };
  // Frozen host reads must outlive the separate response records up to their
  // renderer callback. Vector moves preserve the blob allocation addressed
  // by the proxy's spans. The target frame already owns these blobs.
  std::vector<Decoded> held_responses;
  uint64_t held_bytes = 0;
  while (swaps + 1 < reference.swaps && reader.Next(event, error)) {
    Decoded decoded = Decode(reader, event, reference.version);
    const bool response = decoded.kind == trace::EventKind::kTextureRevision ||
                          decoded.kind == trace::EventKind::kTextureRead;
    if (response) {
      const uint64_t storage = decoded.blob.capacity() + 3 * sizeof(Decoded);
      Require(storage <= kInputLimit - held_bytes, "callback host inputs exceed 256 MiB limit");
      held_bytes += storage;
      held_responses.push_back(std::move(decoded));
      dispatch(held_responses.back());
    } else {
      dispatch(decoded);
      if (decoded.kind == trace::EventKind::kDraw || decoded.kind == trace::EventKind::kGpuEvent ||
          decoded.kind == trace::EventKind::kSwap) {
        held_responses.clear();
        held_bytes = 0;
      }
    }
  }
  Checked(error.empty(), error, "replay event stream");
  Require(swaps + 1 == reference.swaps, "replay did not finish the warmup prefix");
  Require(pthread_setname_np(pthread_self(), "xn-replay-frame") == 0, "could not name target-frame thread");
  if (xn_cpu_api_audit_checkpoint) xn_cpu_api_audit_checkpoint("target_begin", reference.swaps-1);
  timing.Begin(renderer, memory);
  for (const Decoded& decoded : target.events) dispatch(decoded);
  timing.Finish(renderer, memory);
  if (xn_cpu_api_audit_checkpoint) xn_cpu_api_audit_checkpoint("target_end", reference.swaps-1);
  Require(pthread_setname_np(pthread_self(), "xn-replay") == 0, "could not restore replay thread name");
  Require(swaps == reference.swaps && timing.complete, "replay did not reach target frame");
  std::vector<uint8_t> actual;
  uint32_t width = 0, height = 0;
  Require(renderer.CaptureFrame(actual, width, height), "final renderer image readback failed");
  const uint64_t actual_hash = XXH3_64bits(actual.data(), actual.size());
  const bool match = width == reference.width && height == reference.height &&
                     actual_hash == reference.pixel_hash && actual == reference.pixels;
  if (!match) {
    size_t first_difference = 0;
    const size_t common = std::min(actual.size(), reference.pixels.size());
    while (first_difference < common && actual[first_difference] == reference.pixels[first_difference]) ++first_difference;
    std::cout << "{\"type\":\"renderer_replay\",\"iteration\":" << iteration
              << ",\"verified\":false,\"error\":\"pixel mismatch; benchmark rejected\",\"expected_xxh3\":"
              << JsonString(Hex64(reference.pixel_hash)) << ",\"actual_xxh3\":" << JsonString(Hex64(actual_hash))
              << ",\"actual_width\":" << width << ",\"actual_height\":" << height
              << ",\"first_differing_byte\":" << first_difference << "}\n";
    try { MismatchImages(options, iteration, reference, actual, width, height); }
    catch (const std::exception& failure) { std::cerr << "mismatch image output: " << failure.what() << '\n'; }
    return false;
  }
  PrintResult(options, reference, iteration, timing, renderer.GetStats(), renderer.DeviceName(), target);
  renderer.FlushCaches();
  renderer.Shutdown();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = ParseOptions(argc, argv);
    trace::Reader reader(options.trace);
    std::string error;
    Checked(reader.Open(error), error, "validate trace container");
    const Summary reference = Preflight(reader);
    if (options.validate_only) {
      std::cout << "{\"type\":\"trace_validation\",\"container_valid\":true,\"semantics_valid\":true,"
                << "\"render_verified\":false,\"swaps\":" << reference.swaps << "}\n";
      return 0;
    }
    const PreparedTarget target = PrepareTarget(reader, reference);
    if (!options.cache.empty()) std::filesystem::create_directories(options.cache);
    // Request the renderer's existing nonblocking timestamp counters. Their
    // completed-batch scope is reported explicitly in the JSON result.
    setenv("XN_GPU_TIMES", "1", 1);
    Require(pthread_setname_np(pthread_self(), "xn-replay") == 0, "could not name replay thread");
    xn::gfx::SetRendererLogSink(RendererLog);
    for (uint32_t iteration = 1; iteration <= options.repeat; ++iteration) {
      Checked(reader.Rewind(error), error, "rewind for replay iteration");
      if (!Replay(reader, reference, target, options, iteration)) return 2;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render replay: " << error.what() << '\n';
    std::cout << "{\"type\":\"error\",\"verified\":false,\"error\":" << JsonString(error.what()) << "}\n";
    return 1;
  }
}
