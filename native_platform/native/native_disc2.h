#pragma once
#include <filesystem>
#include <functional>
#include <string>
namespace forza4 {
struct Disc2Result { int installed=0, existing=0; std::string message; bool ok=false; };
Disc2Result InstallNativeDisc2(const std::filesystem::path& source,
    const std::filesystem::path& game_root, const std::filesystem::path& save_root,
    const std::function<void(const std::string&)>& progress = {});
}
