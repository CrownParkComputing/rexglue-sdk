#pragma once

#include "vertex_fetch_range.h"
#include "vertex_index_range.h"

namespace xn::gfx {

struct AffineVertexFetch {
  uint32_t slot = UINT32_MAX;
  uint32_t offset_constant = 0, scale_constant = 0;
};

// A compiler prefix used by expanded scenery: add a constant to the original
// vertex index, multiply by another constant, then perform one full fetch.
// Mini fetches may continue in following exec blocks. Prove the whole shader
// has no other full fetch and reuse the direct-fetch control-flow validation.
inline AffineVertexFetch AnalyzeAffineVertexFetch(std::span<const uint32_t> words) {
  using namespace rex::graphics;
  using namespace rex::graphics::ucode;
  if (words.size() < 12) return {};
  ControlFlowInstruction first_pair[2];
  UnpackControlFlowInstructions(words.data(), first_pair);
  const auto& first = first_pair[0];
  const uint32_t address = first.exec.address();
  if ((first.opcode() != ControlFlowOpcode::kExec && first.opcode() != ControlFlowOpcode::kExecEnd) ||
      !address || first.exec.count() < 3 || (first.exec.sequence() & 0x15) != 0x10 ||
      uint64_t(address + first.exec.count()) * 3 > words.size()) return {};
  ParsedAluInstruction add, mul;
  AluInstruction instruction;
  std::memcpy(&instruction, words.data() + address * 3, sizeof(instruction));
  ParseAluInstruction(instruction, xenos::ShaderType::kVertex, add);
  std::memcpy(&instruction, words.data() + (address + 1) * 3, sizeof(instruction));
  ParseAluInstruction(instruction, xenos::ShaderType::kVertex, mul);
  FetchInstruction fetch_instruction;
  std::memcpy(&fetch_instruction, words.data() + (address + 2) * 3, sizeof(fetch_instruction));
  if (fetch_instruction.opcode() != FetchOpcode::kVertexFetch) return {};
  const auto& fetch = fetch_instruction.vertex_fetch();
  if (fetch.is_mini_fetch() || fetch.is_predicated() || fetch.is_src_relative() ||
      fetch.is_dest_relative() || !fetch.stride()) return {};
  const auto& sum = add.vector_and_constant_result;
  if (add.is_predicated || add.vector_opcode != AluVectorOpcode::kAdd || add.vector_operand_count != 2 ||
      add.scalar_result.GetUsedWriteMask() || add.scalar_opcode != AluScalarOpcode::kRetainPrev ||
      sum.storage_target != InstructionStorageTarget::kRegister || !sum.storage_index || sum.is_clamped ||
      sum.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute || mul.is_predicated)
    return {};
  const InstructionResult* product;
  const InstructionOperand* operands;
  uint32_t component;
  if (mul.vector_opcode == AluVectorOpcode::kMul && mul.vector_operand_count == 2 &&
      !mul.scalar_result.GetUsedWriteMask() && mul.scalar_opcode == AluScalarOpcode::kRetainPrev) {
    product = &mul.vector_and_constant_result; operands = mul.vector_operands;
    component = fetch.src_swizzle();
  } else if ((mul.scalar_opcode == AluScalarOpcode::kMulsc0 || mul.scalar_opcode == AluScalarOpcode::kMulsc1) &&
             mul.scalar_operand_count == 2 && !mul.vector_and_constant_result.GetUsedWriteMask() &&
             (mul.vector_opcode == AluVectorOpcode::kMax || mul.vector_opcode == AluVectorOpcode::kAdd)) {
    product = &mul.scalar_result; operands = mul.scalar_operands; component = 0;
  } else return {};
  if (product->storage_target != InstructionStorageTarget::kRegister || product->is_clamped ||
      product->storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute ||
      product->storage_index != fetch.src() || !product->storage_index ||
      !(product->GetUsedWriteMask() & (1u << fetch.src_swizzle()))) return {};
  auto plain = [](const InstructionOperand& op) {
    return op.component_count && !op.is_negated && !op.is_absolute_value &&
           op.storage_addressing_mode == InstructionStorageAddressingMode::kAbsolute;
  };
  auto lane = [](const InstructionOperand& op, uint32_t component) -> uint32_t {
    const auto swizzle = op.GetComponent(component);
    return swizzle <= SwizzleSource::kW ? uint32_t(swizzle) : UINT32_MAX;
  };
  AffineVertexFetch out;
  out.offset_constant = out.scale_constant = UINT32_MAX;
  uint32_t sum_component = UINT32_MAX;
  for (unsigned i = 0; i < 2; ++i) {
    const auto& op = operands[i];
    if (!plain(op) || lane(op, component) > 3) return {};
    if (op.storage_source == InstructionStorageSource::kRegister && op.storage_index == sum.storage_index)
      sum_component = lane(op, component);
    else if (op.storage_source == InstructionStorageSource::kConstantFloat && op.storage_index < 256)
      out.scale_constant = op.storage_index * 4 + lane(op, component);
    else return {};
  }
  if (sum_component > 3 || !(sum.GetUsedWriteMask() & (1u << sum_component)) ||
      out.scale_constant == UINT32_MAX) return {};
  bool original_index = false;
  for (const auto& op : std::span(add.vector_operands, 2)) {
    if (!plain(op) || lane(op, sum_component) > 3) return {};
    if (op.storage_source == InstructionStorageSource::kRegister && op.storage_index == 0 &&
        lane(op, sum_component) == 0) original_index = true;
    else if (op.storage_source == InstructionStorageSource::kConstantFloat && op.storage_index < 256)
      out.offset_constant = op.storage_index * 4 + lane(op, sum_component);
    else return {};
  }
  if (!original_index || out.offset_constant == UINT32_MAX) return {};
  const uint32_t slot = fetch.fetch_constant_index();
  std::vector<uint32_t> direct(words.begin(), words.end());
  direct[(address + 2) * 3] &= ~((63u << 5) | (3u << 30));  // original r0.x
  if (slot >= 96 || !AnalyzeDirectVertexFetches(direct).test(slot)) return {};
  uint32_t pairs = uint32_t(words.size() / 3), full_fetches = 0;
  for (uint32_t i = 0; i < pairs; ++i) {
    ControlFlowInstruction pair[2];
    UnpackControlFlowInstructions(words.data() + i * 3, pair);
    for (const auto& cf : pair) {
      if (cf.opcode() != ControlFlowOpcode::kExec && cf.opcode() != ControlFlowOpcode::kExecEnd &&
          cf.opcode() != ControlFlowOpcode::kCondExec && cf.opcode() != ControlFlowOpcode::kCondExecPred &&
          cf.opcode() != ControlFlowOpcode::kCondExecPredClean) continue;
      pairs = std::min(pairs, cf.exec.address());
      for (uint32_t j = 0; j < cf.exec.count(); ++j) {
        if (!((cf.exec.sequence() >> (2 * j)) & 1)) continue;
        FetchInstruction next;
        std::memcpy(&next, words.data() + (cf.exec.address() + j) * 3, sizeof(next));
        if (!next.vertex_fetch().is_mini_fetch() && ++full_fetches != 1) return {};
      }
      if (cf.opcode() == ControlFlowOpcode::kExecEnd) {
        if (full_fetches == 1) { out.slot = slot; return out; }
        return {};
      }
    }
  }
  return {};
}

inline bool BoundAffineVertexFetch(VertexIndexRange& range, float offset, float scale) {
  if (range.first > range.last || range.last >= (1u << 23) || !std::isfinite(offset) ||
      !std::isfinite(scale) || scale < 0) return false;
  const double first = double(range.first) + offset, last = double(range.last) + offset;
  if (first < 0 || last >= (1u << 23)) return false;
  // Below 2^23, one unit encloses float addition error; after scaling, two
  // more enclose multiplication and either vertex-fetch rounding mode.
  const double lo = (first - 1) * scale - 2, hi = (last + 1) * scale + 2;
  if (!std::isfinite(hi) || hi >= (1u << 23)) return false;
  range = {uint32_t(std::max(0.0, std::floor(lo))), uint32_t(std::ceil(hi))};
  return true;
}

}  // namespace xn::gfx
