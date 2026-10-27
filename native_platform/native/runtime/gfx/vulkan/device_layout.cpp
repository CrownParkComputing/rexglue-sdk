/**
 ******************************************************************************
 * xenon-native - DeviceLayout / DeviceShadow
 ******************************************************************************
 */
#include <cstring>
#include <fstream>

#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

#include <rex/graphics/register_file.h>

#include "shape_a.h"

namespace xn::gfx {

using namespace rex::graphics;

DeviceLayout DeviceLayout::Default() {
  DeviceLayout l;
  l.register_blocks = {
      {0x2000, 10368, 16}, {0x2100, 10444, 21}, {0x2180, 10528, 5}, {0x2200, 10548, 12},
      {0x2280, 10596, 21}, {0x2300, 10680, 38}, {0x2380, 10832, 8},
  };
  return l;
}

bool DeviceLayout::LoadToml(const std::filesystem::path& path, DeviceLayout& out,
                            std::string* error) {
  toml::parse_result result = toml::parse_file(path.string());
  if (!result) {
    if (error) *error = std::string(result.error().description());
    return false;
  }
  const toml::table& root = result.table();
  const toml::table* device = root["device"].as_table();
  if (!device) {
    if (error) *error = "missing [device]";
    return false;
  }
  DeviceLayout l;
  l.register_blocks.clear();
  auto get_u32 = [&](const char* key, uint32_t& dst) {
    if (auto v = (*device)[key].value<int64_t>()) dst = uint32_t(*v);
  };
  get_u32("size", l.size);
  get_u32("vs_alu_constants", l.vs_alu_constants);
  get_u32("ps_alu_constants", l.ps_alu_constants);
  get_u32("fetch_constants", l.fetch_constants);
  get_u32("bool_loop_constants", l.bool_loop_constants);
  if (auto v = (*device)["big_endian"].value<bool>()) l.big_endian = *v;
  if (const toml::array* blocks = (*device)["register_block"].as_array()) {
    for (const toml::node& n : *blocks) {
      const toml::table* b = n.as_table();
      if (!b) continue;
      RegisterBlock rb;
      rb.register_base = uint32_t((*b)["register_base"].value_or<int64_t>(0));
      rb.shadow_offset = uint32_t((*b)["shadow_offset"].value_or<int64_t>(0));
      rb.count = uint32_t((*b)["count"].value_or<int64_t>(0));
      if (rb.count) l.register_blocks.push_back(rb);
    }
  }
  if (l.register_blocks.empty()) {
    if (error) *error = "no [[device.register_block]] entries";
    return false;
  }
  out = l;
  return true;
}

uint32_t DeviceShadow::Dword(uint32_t byte_offset) const {
  uint32_t v;
  std::memcpy(&v, base + byte_offset, 4);
  return layout->big_endian ? __builtin_bswap32(v) : v;
}

uint32_t DeviceShadow::Register(uint32_t register_index, bool* found) const {
  for (const DeviceLayout::RegisterBlock& b : layout->register_blocks) {
    if (register_index >= b.register_base && register_index < b.register_base + b.count) {
      if (found) *found = true;
      return Dword(b.shadow_offset + 4 * (register_index - b.register_base));
    }
  }
  if (found) *found = false;
  return 0;
}

void DeviceShadow::ExportRegisters(rex::graphics::RegisterFile& regs) const {
  auto copy = [&](uint32_t register_base, uint32_t shadow_offset, uint32_t count) {
    if (register_base + count > rex::graphics::RegisterFile::kRegisterCount) return;
    const uint8_t* src = base + shadow_offset;
    uint32_t* dst = &regs.values[register_base];
    if (layout->big_endian) {
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t v;
        std::memcpy(&v, src + 4 * i, 4);
        dst[i] = __builtin_bswap32(v);
      }
    } else {
      std::memcpy(dst, src, size_t(count) * 4);
    }
  };
  for (const DeviceLayout::RegisterBlock& b : layout->register_blocks) {
    copy(b.register_base, b.shadow_offset, b.count);
  }
  copy(XE_GPU_REG_SHADER_CONSTANT_000_X, layout->vs_alu_constants, 256 * 4);
  copy(XE_GPU_REG_SHADER_CONSTANT_256_X, layout->ps_alu_constants, 256 * 4);
  copy(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, layout->fetch_constants, 96 * 2);
  copy(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031, layout->bool_loop_constants, 8 + 32);
}

}  // namespace xn::gfx
