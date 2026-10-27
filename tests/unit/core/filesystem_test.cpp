/**
 * Unit tests for host filesystem handles and directory enumeration.
 *
 * Covers the host-side handle only; guest path translation is out of scope.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem.h>

using rex::filesystem::FileAccess;
using rex::filesystem::FileHandle;

namespace {

constexpr std::string_view kPayload = "rexglue";

class ScopedTempFile {
 public:
  ScopedTempFile() {
    path_ = std::filesystem::temp_directory_path() /
            ("rexglue_filesystem_test_" + std::to_string(counter_++) + ".bin");
    FILE* file = rex::filesystem::OpenFile(path_, "wb");
    REQUIRE(file != nullptr);
    fwrite(kPayload.data(), 1, kPayload.size(), file);
    fclose(file);
  }
  ~ScopedTempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  static inline int counter_ = 0;
};

}  // namespace

TEST_CASE("OpenExisting read-only handle reads", "[filesystem]") {
  ScopedTempFile temp;
  auto handle = FileHandle::OpenExisting(temp.path(), FileAccess::kGenericRead);
  REQUIRE(handle != nullptr);

  std::array<uint8_t, 16> buffer{};
  size_t bytes_read = 0;
  REQUIRE(handle->Read(0, buffer.data(), kPayload.size(), &bytes_read));
  CHECK(bytes_read == kPayload.size());
  CHECK(std::string_view(reinterpret_cast<char*>(buffer.data()), bytes_read) == kPayload);
}

TEST_CASE("OpenExisting read+write handle retains read access", "[filesystem]") {
  ScopedTempFile temp;
  auto handle = FileHandle::OpenExisting(
      temp.path(), FileAccess::kGenericRead | FileAccess::kGenericWrite |
                       FileAccess::kFileReadData | FileAccess::kFileWriteData);
  REQUIRE(handle != nullptr);

  std::array<uint8_t, 16> buffer{};
  size_t bytes_read = 0;
  REQUIRE(handle->Read(0, buffer.data(), kPayload.size(), &bytes_read));
  CHECK(bytes_read == kPayload.size());

  const uint8_t patch = 'X';
  size_t bytes_written = 0;
  REQUIRE(handle->Write(0, &patch, 1, &bytes_written));
  CHECK(bytes_written == 1);

  REQUIRE(handle->Read(0, buffer.data(), 1, &bytes_read));
  CHECK(buffer[0] == patch);
}

TEST_CASE("OpenExisting kGenericAll grants both directions", "[filesystem]") {
  ScopedTempFile temp;
  auto handle = FileHandle::OpenExisting(temp.path(), FileAccess::kGenericAll);
  REQUIRE(handle != nullptr);

  const uint8_t patch = 'Z';
  size_t bytes_written = 0;
  REQUIRE(handle->Write(0, &patch, 1, &bytes_written));
  CHECK(bytes_written == 1);

  std::array<uint8_t, 4> buffer{};
  size_t bytes_read = 0;
  REQUIRE(handle->Read(0, buffer.data(), 1, &bytes_read));
  CHECK(buffer[0] == patch);
}

TEST_CASE("OpenExisting write-only handle writes at the requested offset", "[filesystem]") {
  ScopedTempFile temp;
  auto handle = FileHandle::OpenExisting(temp.path(),
                                         FileAccess::kGenericWrite | FileAccess::kFileAppendData);
  REQUIRE(handle != nullptr);

  const uint8_t patch = 'Q';
  size_t bytes_written = 0;
  REQUIRE(handle->Write(1, &patch, 1, &bytes_written));
  CHECK(bytes_written == 1);
  handle->Flush();
  handle.reset();

  auto reader = FileHandle::OpenExisting(temp.path(), FileAccess::kGenericRead);
  REQUIRE(reader != nullptr);
  std::array<uint8_t, 16> buffer{};
  size_t bytes_read = 0;
  REQUIRE(reader->Read(0, buffer.data(), kPayload.size(), &bytes_read));
  CHECK(bytes_read == kPayload.size());
  CHECK(buffer[1] == patch);
}

TEST_CASE("OpenExisting failed read reports zero bytes", "[filesystem]") {
  ScopedTempFile temp;
  auto handle = FileHandle::OpenExisting(temp.path(), FileAccess::kGenericWrite);
  REQUIRE(handle != nullptr);

  std::array<uint8_t, 16> buffer{};
  size_t bytes_read = 0xDEAD;
  CHECK_FALSE(handle->Read(0, buffer.data(), buffer.size(), &bytes_read));
  CHECK(bytes_read == 0);
}

#ifndef _WIN32
TEST_CASE("ListFiles follows shared content links and skips dangling links", "[filesystem]") {
  struct TempDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("rexglue_directory_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { REQUIRE(std::filesystem::create_directory(path)); }
    ~TempDirectory() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } temp;

  std::filesystem::create_directory(temp.path / "content");
  {
    std::ofstream payload(temp.path / "payload.bin", std::ios::binary);
    payload << kPayload;
    REQUIRE(payload.good());
  }
  std::filesystem::create_directory_symlink("content", temp.path / "shared-dlc");
  std::filesystem::create_symlink("payload.bin", temp.path / "shared-file");
  std::filesystem::create_symlink("missing", temp.path / "dangling");

  const auto entries = rex::filesystem::ListFiles(temp.path);
  REQUIRE(entries.size() == 4);
  using Type = rex::filesystem::FileInfo::Type;
  for (const auto& name : {"content", "shared-dlc", "payload.bin", "shared-file"}) {
    CAPTURE(name);
    const auto entry = std::find_if(entries.begin(), entries.end(),
                                    [&](const auto& info) { return info.name == name; });
    REQUIRE(entry != entries.end());
    const bool directory = std::string_view(name) == "content" ||
                           std::string_view(name) == "shared-dlc";
    CHECK(entry->type == (directory ? Type::kDirectory : Type::kFile));
    CHECK(entry->total_size == (directory ? 0 : kPayload.size()));
    CHECK(entry->path == temp.path);
  }
}
#endif
