// xenon-native shim for <rex/filesystem.h>: only OpenFile(), used by
// Shader::DumpUcode.
#pragma once
#include <cstdio>
#include <filesystem>
namespace rex::filesystem {
inline FILE* OpenFile(const std::filesystem::path& path, const char* mode) {
  return std::fopen(path.string().c_str(), mode);
}
}  // namespace rex::filesystem
