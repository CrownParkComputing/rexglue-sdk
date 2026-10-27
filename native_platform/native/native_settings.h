#pragma once
#include <filesystem>
#include <string>
#include "gfx/host_overlay.h"

namespace forza4 {
struct NativeSettings {
  int width = 1280, height = 720;
  int window_mode = 1;  // windowed, desktop fullscreen, exclusive fullscreen
  int fps = 60;
  bool show_fps = true, vsync = true;
  xn::gfx::OutputFilter filter = xn::gfx::OutputFilter::Linear;
  float sharpness = .25f, ui_scale = 1.f, volume = 1.f;
  std::string disc2_root;
  bool auto_install_disc2 = true;
  bool Load(const std::filesystem::path& path, std::string& error);
  std::string ImportLegacy(const std::filesystem::path& path);
  bool Save(const std::filesystem::path& path, std::string& error) const;
  bool Validate(std::string& error) const;
};
}  // namespace forza4
