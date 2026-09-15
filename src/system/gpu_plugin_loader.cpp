/**
 * @file        system/gpu_plugin_loader.cpp
 * @brief       Host-side loader for GPU emulation plugin DLLs
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/gpu_plugin.h>

#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/dynlib.h>

namespace rex::system {

#if defined(REXGLUE_GPU_BUILTIN)
// Defined by the GPU backend's plugin_main.cpp, linked into this binary.
extern "C" uint32_t rex_gpu_abi_version(void);
extern "C" IGraphicsSystem* rex_gpu_create(uint32_t abi_version, const GpuCreateInfo* info);
#endif

namespace {

// Plugin binaries follow the SDK's per-config postfix convention; this TU is
// part of rexruntime, so REXGLUE_BUILD_CONFIG matches the plugin's config.
std::string PluginFileName(std::string_view name) {
  constexpr std::string_view kConfig = REXGLUE_BUILD_CONFIG;
  std::string_view postfix = "";
  if (kConfig == "Debug") {
    postfix = "d";
  } else if (kConfig == "RelWithDebInfo") {
    postfix = "rd";
  }
#if REX_PLATFORM_WIN32
  return fmt::format("rexgpu-{}{}.dll", name, postfix);
#elif REX_PLATFORM_MAC
  return fmt::format("librexgpu-{}{}.dylib", name, postfix);
#else
  return fmt::format("librexgpu-{}{}.so", name, postfix);
#endif
}

// Plugins stay loaded for process lifetime: guest threads may still be in
// plugin code pages at shutdown.
std::vector<platform::DynamicLibrary>& LoadedPlugins() {
  static std::vector<platform::DynamicLibrary> plugins;
  return plugins;
}

}  // namespace

std::unique_ptr<IGraphicsSystem> LoadGpuPlugin(std::string_view name, std::string_view backend) {
#if defined(REXGLUE_GPU_BUILTIN)
  // The GPU backend is linked into this binary rather than dlopened. A plugin
  // resolves the runtime's symbols from librexruntime.so, so with a static
  // runtime there is no library for it to bind to - building it in is what
  // makes a single self-contained executable possible at all.
  (void)name;
  if (rex_gpu_abi_version() != kGpuPluginAbiVersion) {
    REXSYS_ERROR("built-in GPU backend is ABI {}, host is ABI {}", rex_gpu_abi_version(),
                 kGpuPluginAbiVersion);
    return nullptr;
  }
  std::string backend_str(backend);
  GpuCreateInfo info{};
  info.struct_size = sizeof(GpuCreateInfo);
  info.backend = backend_str.c_str();
  IGraphicsSystem* system = rex_gpu_create(kGpuPluginAbiVersion, &info);
  if (!system) {
    REXSYS_ERROR("built-in GPU backend failed to create a graphics system");
    return nullptr;
  }
  return std::unique_ptr<IGraphicsSystem>(system);
#else
#if REX_PLATFORM_ANDROID
  // There is no folder to sit next to on Android: the executable is
  // /system/bin/app_process and the plugin is in the APK's own native library
  // directory, whose name contains an install hash. The dynamic linker already
  // searches that directory, so ask for the library by name and let it resolve.
  std::filesystem::path path = PluginFileName(name);
#else
  auto path = rex::filesystem::GetExecutableFolder() / PluginFileName(name);
  if (!std::filesystem::exists(path)) {
    REXSYS_ERROR(
        "GPU plugin '{}' not found at {}. Stage it next to the executable "
        "(GPU_PLUGINS {} in rexglue_configure_target).",
        name, path.string(), name);
    return nullptr;
  }
#endif

  platform::DynamicLibrary library;
  if (!library.Load(path, platform::SymbolResolution::kImmediate)) {
    REXSYS_ERROR("GPU plugin '{}' failed to load: {}", name, path.string());
    return nullptr;
  }

  auto abi_version_fn = library.GetSymbol<GpuAbiVersionFn>(kGpuAbiVersionSymbol);
  auto create_fn = library.GetSymbol<GpuCreateFn>(kGpuCreateSymbol);
  if (!abi_version_fn || !create_fn) {
    REXSYS_ERROR("GPU plugin '{}' is not a rexglue GPU plugin (missing {} / {} exports): {}", name,
                 kGpuAbiVersionSymbol, kGpuCreateSymbol, path.string());
    return nullptr;
  }

  uint32_t plugin_abi = abi_version_fn();
  if (plugin_abi != kGpuPluginAbiVersion) {
    REXSYS_ERROR("GPU plugin '{}' has ABI version {}, host expects {}: {}", name, plugin_abi,
                 kGpuPluginAbiVersion, path.string());
    return nullptr;
  }

  std::string backend_str(backend);
  GpuCreateInfo info{};
  info.struct_size = sizeof(GpuCreateInfo);
  info.backend = backend_str.c_str();

  IGraphicsSystem* graphics_system = create_fn(kGpuPluginAbiVersion, &info);
  if (!graphics_system) {
    REXSYS_ERROR("GPU plugin '{}' factory returned no graphics system (backend '{}')", name,
                 backend_str);
    return nullptr;
  }

  LoadedPlugins().push_back(std::move(library));
  REXSYS_DEBUG("GPU plugin '{}' loaded ({})", name, path.filename().string());
  return std::unique_ptr<IGraphicsSystem>(graphics_system);
#endif  // REXGLUE_GPU_BUILTIN
}

}  // namespace rex::system
