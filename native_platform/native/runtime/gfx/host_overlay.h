#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace xn::gfx {
struct HostUiVertex { float position[2], uv[2]; uint8_t color[4]; };
struct HostUiCommand {
  float clip[4];
  uint32_t index_offset = 0, index_count = 0;
};
struct HostUiFont {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
};
struct HostUiFrame {
  std::shared_ptr<const HostUiFont> font;
  std::vector<HostUiVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<HostUiCommand> commands;
};
enum class OutputFilter : uint32_t { Linear, Nearest, Sharpened };
struct PresentationSettings {
  bool vsync = true;
  OutputFilter filter = OutputFilter::Linear;
  float sharpness = .25f;
};
}  // namespace xn::gfx
