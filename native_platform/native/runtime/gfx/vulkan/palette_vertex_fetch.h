#pragma once

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <rex/graphics/pipeline/shader/shader.h>
#include "vertex_index_range.h"

namespace xn::gfx {

struct PaletteVertexFetch {
  uint32_t slot = UINT32_MAX;
  uint32_t divisor_component = 0, bias_component = 0, instance_stride_component = 0;
  uint32_t palette_constant = 0;
};

// A compiler pattern for instanced, single-bone skinning. The first stream
// supplies an unsigned byte bone index; the second reads bone + pose_base.
// Prove the complete prefix and reject any later fetch or non-linear control
// flow. This only reduces CPU mirroring; it does not change shader arithmetic.
inline PaletteVertexFetch AnalyzePaletteVertexFetch(std::span<const uint32_t> words) {
  using namespace rex::graphics;
  using namespace rex::graphics::ucode;
  struct Step { uint32_t address; bool fetch; };
  std::array<Step, 14> prefix{};
  uint32_t steps = 0, pairs = uint32_t(words.size() / 3);
  bool ended = false;
  for (uint32_t i = 0; i < pairs && !ended; ++i) {
    ControlFlowInstruction pair[2];
    UnpackControlFlowInstructions(words.data() + i * 3, pair);
    for (const auto& cf : pair) {
      switch (cf.opcode()) {
        case ControlFlowOpcode::kNop: case ControlFlowOpcode::kAlloc:
        case ControlFlowOpcode::kMarkVsFetchDone: continue;
        case ControlFlowOpcode::kExec: case ControlFlowOpcode::kExecEnd: break;
        default: return {};
      }
      const uint32_t first = cf.exec.address(), count = cf.exec.count();
      pairs = std::min(pairs, first);
      if (first <= i || uint64_t(first + count) * 3 > words.size()) return {};
      for (uint32_t j = 0; j < count; ++j) {
        const bool fetch = (cf.exec.sequence() >> (j * 2)) & 1;
        if (steps < prefix.size()) prefix[steps] = {first + j, fetch};
        else if (fetch) return {};
        ++steps;
      }
      if (cf.opcode() == ControlFlowOpcode::kExecEnd) { ended = true; break; }
    }
  }
  if (!ended || steps < prefix.size()) return {};
  constexpr uint32_t fetch_steps = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) |
                                   (1u << 11) | (1u << 12) | (1u << 13);
  std::array<ParsedAluInstruction, 14> a{};
  std::array<ParsedVertexFetchInstruction, 14> f{};
  VertexFetchInstruction previous_full{};
  for (uint32_t i = 0; i < prefix.size(); ++i) {
    if (prefix[i].fetch != bool(fetch_steps & (1u << i))) return {};
    const auto* data = words.data() + prefix[i].address * 3;
    if (prefix[i].fetch) {
      FetchInstruction raw;
      std::memcpy(&raw, data, sizeof(raw));
      if (raw.opcode() != FetchOpcode::kVertexFetch) return {};
      const auto& vf = raw.vertex_fetch();
      if (vf.is_predicated() || vf.is_dest_relative() || vf.is_src_relative() ||
          vf.offset() < 0 || vf.is_mini_fetch() != (i != 4 && i != 11)) return {};
      ParseVertexFetchInstruction(vf, previous_full, f[i]);
      if (!vf.is_mini_fetch()) previous_full = vf;
    } else {
      AluInstruction raw;
      std::memcpy(&raw, data, sizeof(raw));
      ParseAluInstruction(raw, xenos::ShaderType::kVertex, a[i]);
      if (a[i].is_predicated) return {};
    }
  }
  auto result = [](const InstructionResult& r, uint32_t mask, bool fetch_x = false) {
    if (!mask) return !r.GetUsedWriteMask();
    if (r.storage_target != InstructionStorageTarget::kRegister || r.storage_index != 0 ||
        r.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute ||
        r.is_clamped || r.GetUsedWriteMask() != mask) return false;
    for (uint32_t c = 0; c < 4; ++c)
      if ((mask & (1u << c)) && r.components[c] != SwizzleSource(fetch_x ? 0 : c)) return false;
    return true;
  };
  auto operand = [](const InstructionOperand& o, uint32_t lane, InstructionStorageSource source,
                    uint32_t index, uint32_t component, bool negate = false,
                    InstructionStorageAddressingMode addressing = InstructionStorageAddressingMode::kAbsolute) {
    return o.component_count && o.storage_source == source && o.storage_index == index &&
        o.storage_addressing_mode == addressing && o.is_negated == negate &&
        !o.is_absolute_value && o.GetComponent(lane) == SwizzleSource(component);
  };
  auto reg = [&](const InstructionOperand& o, uint32_t lane, uint32_t c, bool negate = false) {
    return operand(o, lane, InstructionStorageSource::kRegister, 0, c, negate);
  };
  auto constant = [&](const InstructionOperand& o, uint32_t lane) -> uint32_t {
    const uint32_t c = uint32_t(o.GetComponent(lane));
    if (o.storage_index >= 256 || c >= 4 ||
        !operand(o, lane, InstructionStorageSource::kConstantFloat, o.storage_index, c)) return UINT32_MAX;
    return o.storage_index * 4 + c;
  };
  auto vector = [&](uint32_t i, AluVectorOpcode opcode, uint32_t mask, uint32_t count) {
    return a[i].vector_opcode == opcode && a[i].vector_operand_count == count &&
        result(a[i].vector_and_constant_result, mask) && result(a[i].scalar_result, 0) &&
        a[i].scalar_opcode == AluScalarOpcode::kRetainPrev;
  };
  auto scalar = [&](uint32_t i, AluScalarOpcode opcode, uint32_t mask, uint32_t count) {
    return a[i].scalar_opcode == opcode && a[i].scalar_operand_count == count &&
        result(a[i].scalar_result, mask) && result(a[i].vector_and_constant_result, 0) &&
        a[i].vector_opcode == AluVectorOpcode::kMax;
  };
  PaletteVertexFetch out;
  out.bias_component = constant(a[0].vector_operands[1], 2);
  out.divisor_component = constant(a[0].scalar_operands[0], 0);
  out.instance_stride_component = constant(a[8].scalar_operands[0], 0);
  if (out.bias_component == UINT32_MAX || out.divisor_component == UINT32_MAX ||
      out.instance_stride_component == UINT32_MAX) return {};
  if (a[0].vector_opcode != AluVectorOpcode::kAdd || a[0].vector_operand_count != 2 ||
      a[0].scalar_opcode != AluScalarOpcode::kRcp || a[0].scalar_operand_count != 1 ||
      !result(a[0].vector_and_constant_result, 4) || !result(a[0].scalar_result, 2) ||
      !reg(a[0].vector_operands[0], 2, 0) ||
      !vector(1, AluVectorOpcode::kMul, 2, 2) || !reg(a[1].vector_operands[0], 1, 2) ||
      !reg(a[1].vector_operands[1], 1, 1) ||
      !scalar(2, AluScalarOpcode::kTruncs, 2, 1) || !reg(a[2].scalar_operands[0], 0, 1) ||
      !vector(3, AluVectorOpcode::kMad, 1, 3) || !reg(a[3].vector_operands[0], 0, 1, true) ||
      constant(a[3].vector_operands[1], 0) != out.divisor_component ||
      !reg(a[3].vector_operands[2], 0, 0)) return {};
  // Geometry fetches cannot overwrite the instance number in r0.y. The
  // fourth fetch replaces only r0.z with an integer in [0,255].
  for (uint32_t i = 4; i <= 6; ++i)
    if (f[i].result.storage_index == 0) return {};
  if (!reg(f[4].operands[0], 0, 0) || !f[4].attributes.stride ||
      !result(f[7].result, 4, true) ||
      f[7].attributes.data_format != xenos::VertexFormat::k_8_8_8_8 ||
      f[7].attributes.is_signed || !f[7].attributes.is_integer || f[7].attributes.exp_adjust ||
      !(scalar(8, AluScalarOpcode::kMulsc0, 1, 2) || scalar(8, AluScalarOpcode::kMulsc1, 1, 2)) ||
      !reg(a[8].scalar_operands[1], 0, 1) ||
      !scalar(9, AluScalarOpcode::kMaxAs, 0, 1) || !reg(a[9].scalar_operands[0], 0, 0) ||
      !reg(a[9].scalar_operands[0], 1, 0) ||
      !vector(10, AluVectorOpcode::kAdd, 4, 2) || !reg(a[10].vector_operands[0], 2, 2)) return {};
  const auto& pose = a[10].vector_operands[1];
  out.palette_constant = pose.storage_index;
  if (out.palette_constant >= 256 ||
      !operand(pose, 2, InstructionStorageSource::kConstantFloat, out.palette_constant, 0, false,
               InstructionStorageAddressingMode::kAddressRegisterRelative) ||
      !reg(f[11].operands[0], 0, 2) || !f[11].attributes.stride) return {};
  out.slot = f[11].operands[1].storage_index;
  if (out.slot >= 96 || out.slot == f[4].operands[1].storage_index) return {};
  return out;
}

template <typename ReadConstant>
bool BoundPaletteVertexFetch(VertexIndexRange& range, const PaletteVertexFetch& fetch,
                            ReadConstant read) {
  if (fetch.slot >= 96 || range.first > range.last || range.last > 65535) return false;
  const double divisor = read(fetch.divisor_component), bias = read(fetch.bias_component);
  const double stride = read(fetch.instance_stride_component);
  if (!std::isfinite(divisor) || divisor < 1 || divisor > 65536 || std::floor(divisor) != divisor ||
      !std::isfinite(bias) || bias < 0 || bias > .5 ||
      !std::isfinite(stride) || stride < 0 || stride > 255 || std::floor(stride) != stride) return false;
  // The add and multiply are correctly rounded; the normal reciprocal has
  // at most 2.5 ULP error. Their combined relative error is < 2^-20.
  // Add an absolute 2^-20 allowance for zero/subnormal input. Using the
  // actual quotient avoids including an unrelated pose merely because a
  // small quotient lies within the old global 1/16 bound of an integer.
  // https://docs.vulkan.org/spec/latest/appendices/spirvenv.html#spirvenv-precision-operation
  const double first_quotient = (range.first + bias) / divisor;
  const double last_quotient = (range.last + bias) / divisor;
  const double error = (last_quotient + 1.0) / 1048576.0;
  const uint32_t first = uint32_t(std::max(0.0, std::floor(first_quotient - error)));
  const uint32_t last = uint32_t(std::floor(last_quotient + error));
  if (last * stride > 255 || fetch.palette_constant + last * stride >= 256) return false;
  VertexIndexRange palette;
  for (uint32_t q = first; q <= last; ++q) {
    const double base = read((fetch.palette_constant + q * uint32_t(stride)) * 4);
    // These integers, their sum with an unsigned byte and fetch rounding are
    // exactly representable. Negative, fractional or wrapped cases fall back.
    if (!std::isfinite(base) || base < 0 || base > (1u << 20) || std::floor(base) != base) return false;
    palette.first = std::min(palette.first, uint32_t(base));
    palette.last = std::max(palette.last, uint32_t(base) + 255);
    if (!stride) break;
  }
  range = palette;
  return true;
}
} // namespace xn::gfx
