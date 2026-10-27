// Regression for guest asynchronous writes that used to return PENDING forever.
// Separate process: Runtime owns its guest arena, unlike the unit_tests shared arena.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <rex/runtime.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <rex/filesystem/vfs.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xmemory.h>

namespace rex::kernel::xboxkrnl {
u32 NtReadFile_entry(u32, u32, mapped_void, mapped_void, ppc_ptr_t<system::X_IO_STATUS_BLOCK>,
                     mapped_void, u32, mapped_u64);
u32 NtWriteFile_entry(u32, u32, u32, mapped_void, ppc_ptr_t<system::X_IO_STATUS_BLOCK>, mapped_void,
                      u32, mapped_u64);
}

using rex::X_STATUS;

static void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("rex-write-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  int result = 0;
  try {
    rex::InitLogging();
    rex::Runtime runtime(root, root / "user");
    rex::RuntimeConfig config;
    config.tool_mode = true;
    Check(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS, "runtime setup failed");
    auto* memory = runtime.memory();
    const uint32_t buffer = memory->SystemHeapAlloc(4096);
    Check(buffer != 0, "guest allocation failed");
    constexpr char payload[] = "save-completion-regression";
    std::memcpy(memory->TranslateVirtual(buffer), payload, sizeof(payload));
    auto ev = rex::system::object_ref<rex::system::XEvent>(
        new rex::system::XEvent(runtime.kernel_state()));
    ev->Initialize(true, false);

    for (bool synchronous : {true, false}) {
      const std::string name = synchronous ? "sync.sav" : "async.sav";
      rex::filesystem::File* host_file = nullptr;
      rex::filesystem::FileAction action;
      Check(runtime.file_system()->OpenFile(nullptr, "cache:\\" + name,
                                            rex::filesystem::FileDisposition::kOverwriteIf,
                                            rex::filesystem::FileAccess::kGenericWrite, false, true,
                                            &host_file, &action) == X_STATUS_SUCCESS,
            "open failed");
      auto file = rex::system::object_ref<rex::system::XFile>(
          new rex::system::XFile(runtime.kernel_state(), host_file, synchronous));
      rex::system::X_IO_STATUS_BLOCK iosb{};
      iosb.status = X_STATUS_PENDING;
      ev->Reset();
      const auto status = rex::kernel::xboxkrnl::NtWriteFile_entry(
          file->handle(), ev->handle(), 0, nullptr,
          rex::MappedPtr<rex::system::X_IO_STATUS_BLOCK>::from_host(&iosb),
          rex::MappedPtr<void>(memory->TranslateVirtual(buffer), buffer), sizeof(payload), nullptr);
      Check(status == X_STATUS_SUCCESS, "write did not complete immediately");
      Check(iosb.status == X_STATUS_SUCCESS && iosb.information == sizeof(payload),
            "completion status/byte count incorrect");
      uint32_t type = 0, state = 0;
      ev->Query(&type, &state);
      Check(state != 0, "write event was not signalled");
      std::ifstream saved(root / "user" / "cache_partition" / name, std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(saved)), {});
      Check(bytes == std::string(payload, sizeof(payload)), "saved bytes differ");
      std::cout << name << ": bytes, status and completion event PASS\n";

      rex::filesystem::File* reader = nullptr;
      Check(runtime.file_system()->OpenFile(nullptr, "cache:\\" + name,
                                            rex::filesystem::FileDisposition::kOpen,
                                            rex::filesystem::FileAccess::kGenericRead, false, true,
                                            &reader, &action) == X_STATUS_SUCCESS,
            "read open failed");
      auto read_file = rex::system::object_ref<rex::system::XFile>(
          new rex::system::XFile(runtime.kernel_state(), reader, synchronous));
      std::memset(memory->TranslateVirtual(buffer), 0, sizeof(payload));
      iosb.status = X_STATUS_UNSUCCESSFUL;
      iosb.information = 0;
      ev->Reset();
      const auto read_status = rex::kernel::xboxkrnl::NtReadFile_entry(
          read_file->handle(), ev->handle(), nullptr, nullptr,
          rex::MappedPtr<rex::system::X_IO_STATUS_BLOCK>::from_host(&iosb),
          rex::MappedPtr<void>(memory->TranslateVirtual(buffer), buffer), sizeof(payload), nullptr);
      Check(read_status == (synchronous ? X_STATUS_SUCCESS : X_STATUS_PENDING),
            "read returned wrong synchronous/asynchronous status");
      Check(iosb.status == X_STATUS_SUCCESS && iosb.information == sizeof(payload),
            "read completion status/byte count incorrect");
      Check(std::memcmp(memory->TranslateVirtual(buffer), payload, sizeof(payload)) == 0,
            "read bytes differ");
      ev->Query(&type, &state);
      Check(state != 0, "read completion event was not signalled");
      std::cout << name << ": read bytes, IOSB, return status and event PASS\n";
    }
    rex::system::X_IO_STATUS_BLOCK iosb{};
    iosb.information = 999;
    const auto status = rex::kernel::xboxkrnl::NtWriteFile_entry(
        0, 0, 0, nullptr, rex::MappedPtr<rex::system::X_IO_STATUS_BLOCK>::from_host(&iosb),
        rex::MappedPtr<void>(memory->TranslateVirtual(buffer), buffer), sizeof(payload), nullptr);
    Check(status == X_STATUS_INVALID_HANDLE && iosb.status == X_STATUS_INVALID_HANDLE &&
              iosb.information == 0,
          "invalid handle completion incorrect");
    memory->SystemHeapFree(buffer);
    std::cout << "invalid handle: PASS\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    result = 1;
  }
  std::filesystem::remove_all(root);
  return result;
}
