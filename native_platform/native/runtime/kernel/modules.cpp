// Native code for additional retail XEX images. No runtime-loaded host DLLs.
#include "modules.h"
#include "kernel.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <rex/hook.h>

namespace xn {
namespace {
struct Module {
  const rex::PPCImageInfo* info = nullptr;
  XexImage image;
  uint32_t handle = 0;
  uint32_t header = 0;
  uint32_t references = 0;
};
std::recursive_mutex module_mutex;
std::map<std::string, Module> modules;

std::string ModuleName(std::string name) {
  auto separator = name.find_last_of("\\/:");
  if (separator != std::string::npos) name.erase(0, separator + 1);
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
  return name;
}

uint32_t ExportAddress(const Module& module, uint32_t ordinal) {
  return FindXexExport(module.image, ordinal);
}
}  // namespace

void RegisterModuleImage(std::string name, const rex::PPCImageInfo& info) {
  std::lock_guard lock(module_mutex);
  auto& module = modules[ModuleName(std::move(name))];
  if (module.references) Fatal("Cannot replace a loaded native module");
  module.info = &info;
}

u32 NativeLoadImage(mapped_string path, u32 flags, u32 minimum_version, mapped_u32 result) {
  if (!result) return X_STATUS_INVALID_PARAMETER;
  *result = 0;
  if (!path) return X_STATUS_INVALID_PARAMETER;
  std::lock_guard lock(module_mutex);
  auto found = modules.find(ModuleName(path.as<const char*>()));
  if (found == modules.end()) return X_STATUS_OBJECT_NAME_NOT_FOUND;
  auto& module = found->second;
  if (module.references) {
    ++module.references;
    *result = module.handle;
    return X_STATUS_SUCCESS;
  }
  std::filesystem::path host;
  if (!ResolveHostPath(path.as<const char*>(), host)) return X_STATUS_OBJECT_NAME_NOT_FOUND;
  std::string error;
  if (!LoadXexFile(host.string(), module.image, error)) {
    REXKRNL_ERROR("native module {}: {}", host.string(), error);
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  if (module.image.base_address != module.info->image_base || module.image.image_size != module.info->image_size)
    return X_STATUS_INVALID_PARAMETER;
  auto& arena = K().arena;
  if (!arena.AllocVirtualFixed(module.image.base_address, module.image.image_size)) return X_STATUS_NO_MEMORY;
  std::memcpy(G(module.image.base_address), module.image.image.data(), module.image.image_size);
  RegisterImageFunctions(*module.info);
  module.header = arena.SystemAlloc(uint32_t(module.image.header.size()), 0x1000);
  module.handle = arena.SystemAlloc(0x80, 16);
  std::memcpy(G(module.header), module.image.header.data(), module.image.header.size());
  Store32(module.handle + 0x58, module.header);
  Store32(module.handle + 0x1C, module.image.base_address);
  Store32(module.handle + 0x20, module.image.image_size);
  module.references = 1;
  PatchImageVariableImports(module.image);
  if (module.image.entry_point && !CallGuest(module.image.entry_point, {module.handle, 1, 0})) {
    module.references = 0;
    UnregisterImageFunctions(*module.info);
    arena.FreeVirtual(module.image.base_address, nullptr);
    arena.SystemFree(module.header);
    arena.SystemFree(module.handle);
    module.handle = module.header = 0;
    return X_STATUS_UNSUCCESSFUL;
  }
  REXKRNL_INFO("native module loaded: {} at {:#010x}", found->first, module.image.base_address);
  *result = module.handle;
  return X_STATUS_SUCCESS;
}

u32 NativeUnloadImage(u32 handle) {
  std::lock_guard lock(module_mutex);
  for (auto& [name, module] : modules) {
    if (!module.references || module.handle != handle) continue;
    if (--module.references) return X_STATUS_SUCCESS;
    if (module.image.entry_point) CallGuest(module.image.entry_point, {module.handle, 0, 0});
    UnregisterImageFunctions(*module.info);
    K().arena.FreeVirtual(module.image.base_address, nullptr);
    K().arena.SystemFree(module.header);
    K().arena.SystemFree(module.handle);
    module.handle = module.header = 0;
    return X_STATUS_SUCCESS;
  }
  return X_STATUS_INVALID_HANDLE;
}

u32 NativeGetModuleHandle(mapped_string name, mapped_u32 result) {
  if (!result) return X_STATUS_INVALID_PARAMETER;
  *result = 0;
  if (!name) { *result = K().ldr_entry_guest; return X_STATUS_SUCCESS; }
  std::lock_guard lock(module_mutex);
  auto found = modules.find(ModuleName(name.as<const char*>()));
  if (found == modules.end() || !found->second.references) return X_STATUS_OBJECT_NAME_NOT_FOUND;
  *result = found->second.handle;
  return X_STATUS_SUCCESS;
}

u32 NativeGetProcedureAddress(u32 handle, u32 ordinal, mapped_u32 result) {
  if (!result) return X_STATUS_INVALID_PARAMETER;
  *result = 0;
  std::lock_guard lock(module_mutex);
  for (auto& [name, module] : modules) {
    if (!module.references || module.handle != handle) continue;
    auto address = ExportAddress(module, ordinal);
    if (!address) return X_STATUS_OBJECT_NAME_NOT_FOUND;
    *result = address;
    return X_STATUS_SUCCESS;
  }
  return X_STATUS_INVALID_HANDLE;
}
}  // namespace xn

XN_IMPORT(__imp__XexLoadImage, xn::NativeLoadImage)
XN_IMPORT(__imp__XexUnloadImage, xn::NativeUnloadImage)
XN_IMPORT(__imp__XexGetModuleHandle, xn::NativeGetModuleHandle)
XN_IMPORT(__imp__XexGetProcedureAddress, xn::NativeGetProcedureAddress)
