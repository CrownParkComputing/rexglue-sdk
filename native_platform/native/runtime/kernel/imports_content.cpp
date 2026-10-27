// One local storage device. Each content package gets an isolated directory
// beneath the current profile's title save directory, plus its XCONTENT_DATA.
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <rex/hook.h>
#include "kernel.h"
#include "local_profile.h"

using namespace xn;
namespace fs = std::filesystem;
namespace {
constexpr uint32_t kContentSize = 308;
constexpr uint32_t kDevice = 1;

uint32_t Complete(uint32_t result, uint32_t overlapped, uint32_t length = 0) {
  if (!overlapped) return result;
  CompleteOverlapped(overlapped, result, length);
  return X_ERROR_IO_PENDING;
}

bool Component(const std::string& name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find_first_of("/\\:") == std::string::npos;
}
std::string RootName(const char* str) {
  std::string name = str;
  if (!name.empty() && name.back() == ':') name.pop_back();
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
  return name;
}
fs::path TypeDir(uint32_t type) { return K().save_dir / "content" / std::format("{:08X}", type); }

class ContentEnumerator final : public Enumerator {
 public:
  std::vector<std::array<uint8_t, kContentSize>> records;
  uint32_t batch = 1;
  size_t cursor = 0;
  std::mutex mutex;
  uint32_t Read(uint32_t buffer, uint32_t capacity, uint32_t& count) override {
    std::lock_guard lock(mutex);
    count = 0;
    if (cursor == records.size()) return 18;
    if (!buffer || capacity < kContentSize) return X_ERROR_INSUFFICIENT_BUFFER;
    count = std::min({size_t(batch), records.size() - cursor, size_t(capacity / kContentSize)});
    for (uint32_t i = 0; i < count; ++i) std::memcpy(G(buffer + i * kContentSize), records[cursor++].data(), kContentSize);
    return X_ERROR_SUCCESS;
  }
};
}

u32 XamShowDeviceSelectorUI_entry(u32 user, u32 type, u32 flags, u64 requested,
                                  mapped_u32 device, u32 overlapped) {
  if (device) *device = 0;
  if (!device) return Complete(X_ERROR_INVALID_PARAMETER, overlapped);
  if (user != 0 && user != 0xFF) return Complete(X_ERROR_NO_SUCH_USER, overlapped);
  *device = kDevice;
  return Complete(X_ERROR_SUCCESS, overlapped);
}
XN_IMPORT(__imp__XamShowDeviceSelectorUI, XamShowDeviceSelectorUI_entry)

u32 XamContentGetDeviceName_entry(u32 device, u32 buffer, u32 capacity) {
  if (buffer && capacity) Store16(buffer, 0);
  if (device != kDevice) return 1167; // DEVICE_NOT_CONNECTED
  constexpr char16_t name[] = u"Local storage";
  if (!buffer || capacity < std::size(name)) return X_ERROR_INSUFFICIENT_BUFFER;
  for (size_t i = 0; i < std::size(name); ++i) Store16(buffer + uint32_t(i * 2), name[i]);
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamContentGetDeviceName, XamContentGetDeviceName_entry)

u32 XamContentGetDeviceData_entry(u32 device, u32 data) {
  if (!data) return X_ERROR_INVALID_PARAMETER;
  std::memset(G(data), 0, 0x50);
  if (device != kDevice) return 1167; // DEVICE_NOT_CONNECTED
  // The title directory may not exist until its first save. Query the volume
  // containing its nearest existing parent, without inventing free capacity.
  std::error_code ec;
  fs::path path = K().save_dir;
  while (!path.empty() && !fs::exists(path, ec)) {
    const auto parent = path.parent_path();
    if (parent == path) break;
    path = parent;
  }
  const auto space = fs::space(path, ec);
  if (ec) return 1167;
  Store32(data, kDevice);
  Store32(data + 4, 1); // HDD
  Store64(data + 8, space.capacity);
  Store64(data + 16, space.available);
  constexpr char16_t name[] = u"Local storage";
  for (uint32_t i = 0; i < std::size(name); ++i) Store16(data + 24 + i * 2, name[i]);
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamContentGetDeviceData, XamContentGetDeviceData_entry)

u32 XamContentGetDeviceState_entry(u32 device, u32 overlapped) {
  return Complete(device == kDevice ? X_ERROR_SUCCESS : 1167, overlapped);
}
XN_IMPORT(__imp__XamContentGetDeviceState, XamContentGetDeviceState_entry)

u32 XamContentCreateEnumerator_entry(u32 user, u32 device, u32 type, u32 flags,
                                      u32 count, mapped_u32 buffer_size, mapped_u32 handle) {
  if (buffer_size) *buffer_size = 0;
  if (handle) *handle = 0;
  if (!handle || !count || count > UINT32_MAX / kContentSize || device > kDevice) return X_ERROR_INVALID_PARAMETER;
  auto e = std::make_shared<ContentEnumerator>();
  e->batch = count;
  std::error_code ec;
  std::vector<fs::path> paths;
  fs::directory_iterator it(TypeDir(type), ec), end;
  while (!ec && it != end) {
    paths.push_back(it->path());
    it.increment(ec);
  }
  std::sort(paths.begin(), paths.end());
  for (const auto& path : paths) {
    if(path.filename().string().starts_with('.')) continue;
    std::array<uint8_t, kContentSize> data{};
    std::ifstream f(path / "metadata.bin", std::ios::binary);
    if (f.read(reinterpret_cast<char*>(data.data()), data.size())) e->records.push_back(data);
  }
  if (buffer_size) *buffer_size = count * kContentSize;
  *handle = K().objects.Add(e);
  REXKRNL_INFO("Content enumerate type={} packages={}", type, e->records.size());
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamContentCreateEnumerator, XamContentCreateEnumerator_entry)

namespace {
u32 CreateContentEntry(u32 user, mapped_string root, u32 data, u32 flags,
                       mapped_u32 disposition, mapped_u32 license, u32 overlapped) {
  if (disposition) *disposition = 0;
  if (license) *license = 0;
  if (!root || !data) return Complete(X_ERROR_INVALID_PARAMETER, overlapped);
  std::string mount = RootName(root);
  const char* file = reinterpret_cast<const char*>(G(data + 264));
  std::string name(file, strnlen(file, 42));
  uint32_t device = Load32(data), type = Load32(data + 4), mode = flags & 15;
  if (!Component(mount) || !Component(name) || device > kDevice || mode < 1 || mode > 5)
    return Complete(X_ERROR_INVALID_PARAMETER, overlapped);
  if (user != 0 && user != 0xFE && user != 0xFF) return Complete(X_ERROR_NO_SUCH_USER, overlapped);
  fs::path package = TypeDir(type) / name;
  std::lock_guard lock(K().content_mutex);
  std::error_code ec;
  bool exists = fs::is_directory(package / "files", ec);
  if (mode == 1 && exists) return Complete(183, overlapped); // ALREADY_EXISTS
  if ((mode == 3 || mode == 5) && !exists) return Complete(3, overlapped); // PATH_NOT_FOUND
  if (mode == 2 || mode == 5) {
    fs::remove_all(package / "files", ec);
    if (ec) return Complete(5, overlapped);
    exists = false;
  }
  if (!exists) {
    fs::create_directories(package / "files", ec);
    if (ec) return Complete(5, overlapped);
    std::array<uint8_t, kContentSize> metadata;
    std::memcpy(metadata.data(), G(data), kContentSize);
    metadata[0] = metadata[1] = metadata[2] = 0; metadata[3] = kDevice;
    std::ofstream f(package / "metadata.bin", std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(metadata.data()), metadata.size())) return Complete(5, overlapped);
  }
  K().content_mounts[mount] = package / "files";
  uint32_t action = exists ? 2 : 1;
  if (disposition) *disposition = action;
  if (license) *license = 1;
  REXKRNL_INFO("Content {} '{}' -> {}", exists ? "open" : "create", mount,
               package.string());
  return Complete(X_ERROR_SUCCESS, overlapped, action);
}
}  // namespace

u32 XamContentCreateEx_entry(u32 user, mapped_string root, u32 data, u32 flags,
                             mapped_u32 disposition, mapped_u32 license, u32 cache_size,
                             u64 content_size, u32 overlapped) {
  (void)cache_size;
  (void)content_size;
  return CreateContentEntry(user, root, data, flags, disposition, license, overlapped);
}
XN_IMPORT(__imp__XamContentCreateEx, XamContentCreateEx_entry)

// Legacy XamContentCreate carries the same XCONTENT_DATA and options as
// XamContentCreateEx, without cache/content-size arguments. Keep both imports
// on the same profile-backed package path so older titles persist saves under
// <profile-root>/<title-id>/content as the launcher expects.
u32 XamContentCreate_entry(u32 user, mapped_string root, u32 data, u32 flags,
                           mapped_u32 disposition, mapped_u32 license, u32 overlapped) {
  return CreateContentEntry(user, root, data, flags, disposition, license, overlapped);
}
XN_IMPORT(__imp__XamContentCreate, XamContentCreate_entry)

u32 XamContentOpenFile_entry(u32 user, mapped_string root, mapped_string path, u32 flags,
                              mapped_u32 disposition, mapped_u32 license, u32 overlapped) {
  if (disposition) *disposition = 0;
  if (license) *license = 0;
  // This API opens a packaged content file rather than an existing native
  // content directory. No such package has been installed by this runtime.
  REXKRNL_INFO("Content package file '{}' under '{}': not installed",
      path ? path.as<const char*>() : "", root ? root.as<const char*>() : "");
  return Complete(2 /* FILE_NOT_FOUND */, overlapped);
}
XN_IMPORT(__imp__XamContentOpenFile, XamContentOpenFile_entry)

// XamContentGetCreator(user, XCONTENT_DATA, is_creator*, creator_xuid*, overlapped).
// The original XAM content model only marks SAVEDGAME (1) as user-created;
// PROFILE (3) is a separate package type and must not be reported as a save owner.
u32 XamContentGetCreator_entry(u32 user, u32 data, mapped_u32 is_creator,
                               mapped_u64 creator_xuid, u32 overlapped) {
  if (is_creator) *is_creator = 0;
  if (creator_xuid) *creator_xuid = 0;
  if (!data) return Complete(X_ERROR_INVALID_PARAMETER, overlapped);
  if (user != 0 && user != 0xFE && user != 0xFF)
    return Complete(X_ERROR_NO_SUCH_USER, overlapped);
  const uint32_t type = Load32(data + 4);
  const char* file = reinterpret_cast<const char*>(G(data + 264));
  std::string name(file, strnlen(file, 42));
  if (!Component(name)) return Complete(X_ERROR_INVALID_PARAMETER, overlapped);
  const fs::path package = TypeDir(type) / name;
  std::error_code ec;
  if (!fs::is_directory(package / "files", ec))
    return Complete(3, overlapped);  // ERROR_PATH_NOT_FOUND
  // Only saved-game packages have an owner in XAM's content model.
  uint32_t creator = 0;
  uint64_t xuid = 0;
  if (type == 1) {
    creator = 1;
    xuid = LocalUser().xuid();
  }
  if (is_creator) *is_creator = creator;
  if (creator_xuid) *creator_xuid = xuid;
  REXKRNL_INFO("Content creator query type={} '{}' exists=1 creator={} xuid={:#x}", type, name,
               creator, xuid);
  return Complete(X_ERROR_SUCCESS, overlapped);
}
XN_IMPORT(__imp__XamContentGetCreator, XamContentGetCreator_entry)

u32 XamContentFlush_entry(mapped_string root, u32 overlapped) {
  if (!root) return Complete(X_ERROR_INVALID_PARAMETER, overlapped);
  const auto name = RootName(root);
  fs::path path;
  {
    std::lock_guard lock(K().content_mutex);
    auto it = K().content_mounts.find(name);
    if (it == K().content_mounts.end()) return Complete(X_ERROR_NOT_FOUND, overlapped);
    path = it->second;
  }
  const uint32_t result = FlushContentFiles(path);
  REXKRNL_INFO("Content flush '{}' overlapped={:#x} result={}", name, overlapped, result);
  return Complete(result, overlapped);
}
XN_IMPORT(__imp__XamContentFlush, XamContentFlush_entry)

u32 XamContentClose_entry(mapped_string root, u32 overlapped) {
  if (!root) return Complete(X_ERROR_INVALID_PARAMETER, overlapped);
  const auto name = RootName(root);
  fs::path path;
  {
    std::lock_guard lock(K().content_mutex);
    auto it = K().content_mounts.find(name);
    if (it == K().content_mounts.end()) return Complete(X_ERROR_NOT_FOUND, overlapped);
    path = it->second;
  }
  const uint32_t result = FlushContentFiles(path);
  if (result == X_ERROR_SUCCESS) {
    std::lock_guard lock(K().content_mutex);
    K().content_mounts.erase(name);
  }
  return Complete(result, overlapped);
}
XN_IMPORT(__imp__XamContentClose, XamContentClose_entry)

// XamContentDelete(user, root, data, overlapped). The XCONTENT_DATA names the
// package exactly as XamContentCreateEx does (device id, type, display name,
// file name at +264), so the deletion removes the same directory tree that
// create wrote. Any mount under this root name goes first: a title that
// deletes mounted content must not keep writing into it. OutRun Online Arcade
// reaches this from the xapilib thunk at 0x8227DD20, which passes every
// argument through unchanged.
u32 XamContentDelete_entry(u32 user, mapped_string root, u32 data, u32 overlapped) {
  if (!root || !data) return Complete(X_ERROR_INVALID_PARAMETER, overlapped);
  std::string mount = RootName(root);
  const char* file = reinterpret_cast<const char*>(G(data + 264));
  std::string name(file, strnlen(file, 42));
  uint32_t type = Load32(data + 4);
  if (!Component(mount) || !Component(name)) return Complete(X_ERROR_INVALID_PARAMETER, overlapped);
  if (user != 0 && user != 0xFE && user != 0xFF) return Complete(X_ERROR_NO_SUCH_USER, overlapped);
  std::lock_guard lock(K().content_mutex);
  K().content_mounts.erase(mount);
  std::error_code ec;
  fs::remove_all(TypeDir(type) / name, ec);
  if (ec) return Complete(5, overlapped);  // X_ERROR_ACCESS_DENIED
  REXKRNL_INFO("Content delete '{}' (type {:08X})", name, type);
  return Complete(X_ERROR_SUCCESS, overlapped);
}
XN_IMPORT(__imp__XamContentDelete, XamContentDelete_entry)

u32 XamContentSetThumbnail_entry(u32 user, u32 data, u32 image, u32 image_size, u32 overlapped) {
  REXKRNL_WARN("XamContentSetThumbnail(user={}, data={:#010x}, image={:#010x}, size={}): thumbnail ignored",
               user, data, image, image_size);
  return Complete(X_ERROR_SUCCESS, overlapped);
}
XN_IMPORT(__imp__XamContentSetThumbnail, XamContentSetThumbnail_entry)
