#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

#include <rex/filesystem/devices/host_path_device.h>

namespace {

class TemporaryPackage {
 public:
  TemporaryPackage() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("rex_content_visibility_" + std::to_string(stamp));
    std::filesystem::create_directories(path_ / "nested");
    std::ofstream(path_ / "__thumbnail.png") << "host metadata";
    std::ofstream(path_ / "rr6_save.bin") << "guest payload";
    std::ofstream(path_ / "nested" / "__thumbnail.png") << "guest-owned nested file";
  }

  ~TemporaryPackage() { std::filesystem::remove_all(path_); }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("content packages hide only root host thumbnail metadata", "[filesystem][content]") {
  TemporaryPackage package;
  rex::filesystem::HostPathDevice device("\\Device\\ContentTest", package.path(), false,
                                          /*allow_share_delete=*/true,
                                          /*hide_internal_content_metadata=*/true);

  REQUIRE(device.Initialize());
  CHECK(device.ResolvePath("rr6_save.bin") != nullptr);
  CHECK(device.ResolvePath("__thumbnail.png") == nullptr);
  CHECK(device.ResolvePath("__THUMBNAIL.PNG") == nullptr);
  CHECK(device.ResolvePath("nested\\__thumbnail.png") != nullptr);
}

TEST_CASE("ordinary host devices expose root thumbnail files", "[filesystem][content]") {
  TemporaryPackage package;
  rex::filesystem::HostPathDevice device("\\Device\\HostTest", package.path(), false);

  REQUIRE(device.Initialize());
  CHECK(device.ResolvePath("__thumbnail.png") != nullptr);
}
