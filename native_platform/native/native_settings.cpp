#include "native_settings.h"
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>
#include <cmath>
#include <fstream>

namespace forza4 {
std::string NativeSettings::ImportLegacy(const std::filesystem::path& path) {
  std::error_code ec;
  if(!std::filesystem::is_regular_file(path,ec)) return {};
  auto parsed=toml::parse_file(path.string());
  if(!parsed) return {};
  auto& t=parsed.table();
  disc2_root=t["forza4_disc2_content_root"].value_or(disc2_root);
  auto_install_disc2=t["forza4_auto_install_disc2_content"].value_or(auto_install_disc2);
  const auto name=t["keyboard_text"].value_or(std::string{});
  return name;
}
bool NativeSettings::Validate(std::string& error) const {
  if (width < 640 || width > 16384 || height < 360 || height > 8640 ||
      window_mode < 0 || window_mode > 2 || (fps != 30 && fps != 60 && fps != 120) ||
      uint32_t(filter) > uint32_t(xn::gfx::OutputFilter::Sharpened) ||
      !std::isfinite(sharpness) || sharpness < 0 || sharpness > 1 ||
      !std::isfinite(ui_scale) || ui_scale < .75f || ui_scale > 2.5f ||
      !std::isfinite(volume) || volume < 0 || volume > 1 || disc2_root.size() > 4095) {
    error = "Settings contain an invalid display, frame rate or slider value.";
    return false;
  }
  return true;
}
bool NativeSettings::Load(const std::filesystem::path& path, std::string& error) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return true;
  auto parsed = toml::parse_file(path.string());
  if (!parsed) { error = std::string(parsed.error().description()); return false; }
  NativeSettings value = *this;
  auto& t = parsed.table();
  value.width = t["display"]["width"].value_or(value.width);
  value.height = t["display"]["height"].value_or(value.height);
  value.window_mode = t["display"]["window_mode"].value_or(value.window_mode);
  value.fps = t["display"]["fps"].value_or(value.fps);
  value.show_fps = t["display"]["show_fps"].value_or(value.show_fps);
  value.vsync = t["display"]["vsync"].value_or(value.vsync);
  value.filter = xn::gfx::OutputFilter(t["display"]["filter"].value_or(int(value.filter)));
  value.sharpness = t["display"]["sharpness"].value_or(value.sharpness);
  value.ui_scale = t["display"]["ui_scale"].value_or(value.ui_scale);
  value.volume = t["audio"]["volume"].value_or(value.volume);
  value.disc2_root = t["content"]["disc2_root"].value_or(value.disc2_root);
  value.auto_install_disc2 = t["content"]["auto_install_disc2"].value_or(value.auto_install_disc2);
  if (!value.Validate(error)) return false;
  *this = std::move(value);
  return true;
}
bool NativeSettings::Save(const std::filesystem::path& path, std::string& error) const {
  if (!Validate(error)) return false;
  toml::table document{
    {"version", 1},
    {"display", toml::table{{"width",width},{"height",height},{"window_mode",window_mode},
      {"fps",fps},{"show_fps",show_fps},{"vsync",vsync},{"filter",int(filter)},
      {"sharpness",sharpness},{"ui_scale",ui_scale}}},
    {"audio", toml::table{{"volume",volume}}},
    {"content", toml::table{{"disc2_root",disc2_root},{"auto_install_disc2",auto_install_disc2}}}
  };
  std::error_code ec;
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) { error = ec.message(); return false; }
  auto temporary = path; temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::trunc);
    file << document << '\n'; file.flush();
    if (!file) { error = "Could not write settings."; return false; }
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec) { error = ec.message(); return false; }
  return true;
}
}  // namespace forza4
