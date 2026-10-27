#pragma once

#include <algorithm>
#include <bitset>
#include <cstring>
#include <span>
#include <vector>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/shader.h>

namespace xn::gfx {

// Branches entirely after the vertex fetches cannot change their addresses.
// Require a tail containing only ALU/alloc/jumps, a terminating exec, and
// targets inside that tail. Calls, loops and further fetches remain unproven.
inline bool HasFetchFreeBranchTail(std::span<const uint32_t> words,
                                  uint32_t first_cf, uint32_t pairs) {
  using namespace rex::graphics::ucode;
  std::vector<uint32_t> targets;
  for (uint32_t at=first_cf; at<pairs*2; ++at) {
    ControlFlowInstruction pair[2];
    UnpackControlFlowInstructions(words.data() + (at/2)*3,pair);
    const auto& cf = pair[at&1];
    switch (cf.opcode()) {
      case ControlFlowOpcode::kNop: case ControlFlowOpcode::kAlloc:
      case ControlFlowOpcode::kMarkVsFetchDone: continue;
      case ControlFlowOpcode::kCondJmp:
        targets.push_back(cf.cond_jmp.address());
        continue;
      case ControlFlowOpcode::kExec: case ControlFlowOpcode::kExecEnd:
      case ControlFlowOpcode::kCondExec: case ControlFlowOpcode::kCondExecPred:
      case ControlFlowOpcode::kCondExecPredClean: break;
      default: return false;
    }
    const uint32_t first = cf.exec.address(), count = cf.exec.count();
    pairs = std::min(pairs,first);
    if (first <= at/2 || uint64_t(first+count)*3 > words.size()) return false;
    for (uint32_t j=0; j<count; ++j)
      if ((cf.exec.sequence() >> (j*2)) & 1) return false;
    if (cf.opcode() == ControlFlowOpcode::kExecEnd) {
      for (uint32_t target : targets)
        if (target < first_cf || target > at) return false;
      return true;
    }
  }
  return false;
}

// Recognize a deliberately narrow shader form: straight-line control flow,
// with vertex fetches using the original r0.x vertex index. Arithmetic may
// precede a fetch if it has not overwritten that component. Conditional ALU
// blocks are treated as possible writes. Texture samples and enclosed branch
// tails may follow the final vertex fetch; other control flow falls back.
// This is a proof from instructions, not a title/shader hash allowlist.
inline std::bitset<96> AnalyzeDirectVertexFetches(std::span<const uint32_t> words) {
  using namespace rex::graphics::ucode;
  using namespace rex::graphics;
  uint32_t pairs = uint32_t(words.size() / 3);
  bool have_full_fetch = false, full_fetch_direct = false;
  uint32_t full_fetch_slot = 0;
  std::bitset<96> direct_slots, unsafe_slots;
  bool index_overwritten = false, texture_tail = false;
  auto writes_index = [](const InstructionResult& result) {
    return result.storage_target == InstructionStorageTarget::kRegister &&
        (result.GetUsedWriteMask() & 1) &&
        (result.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute ||
         result.storage_index == 0);
  };
  for (uint32_t i = 0; i < pairs; ++i) {
    ControlFlowInstruction pair[2];
    UnpackControlFlowInstructions(words.data() + i * 3, pair);
    for (uint32_t k=0; k<2; ++k) {
      const auto& cf = pair[k];
      bool conditional = false;
      switch (cf.opcode()) {
        case ControlFlowOpcode::kNop:
        case ControlFlowOpcode::kAlloc:
        case ControlFlowOpcode::kMarkVsFetchDone:
          continue;
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd:
          break;
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredClean:
          conditional = true;
          break;
        case ControlFlowOpcode::kCondJmp:
          return HasFetchFreeBranchTail(words,i*2+k,pairs) ? direct_slots & ~unsafe_slots :
                                                           std::bitset<96>{};
        default:
          return {}; // branches, loops and conditional termination
      }
      const uint32_t first = cf.exec.address(), count = cf.exec.count();
      pairs = std::min(pairs, first);
      if (first <= i || uint64_t(first + count) * 3 > words.size()) return {};
      uint32_t sequence = cf.exec.sequence();
      for (uint32_t j = 0; j < count; ++j, sequence >>= 2) {
        if (!(sequence & 1)) {
          AluInstruction instruction;
          std::memcpy(&instruction, words.data() + (first + j) * 3, sizeof(instruction));
          ParsedAluInstruction parsed;
          ParseAluInstruction(instruction, xenos::ShaderType::kVertex, parsed);
          index_overwritten |= writes_index(parsed.vector_and_constant_result) ||
                               writes_index(parsed.scalar_result);
          continue;
        }
        if (conditional) return {}; // a skipped full fetch changes mini-fetch addressing
        FetchInstruction instruction;
        std::memcpy(&instruction, words.data() + (first + j) * 3, sizeof(instruction));
        // Texture sampling after all vertex fetches cannot enlarge their
        // address range. Reject any vertex fetch after this tail starts:
        // texture results may replace the index or other addressing state.
        if (instruction.opcode() == FetchOpcode::kTextureFetch) {
          texture_tail = true;
          continue;
        }
        if (instruction.opcode() != FetchOpcode::kVertexFetch || texture_tail) return {};
        const auto& fetch = instruction.vertex_fetch();
        if (fetch.is_predicated() || fetch.is_dest_relative()) return {};
        if (fetch.is_mini_fetch()) {
          if (!have_full_fetch) return {};
        } else {
          full_fetch_slot = fetch.fetch_constant_index();
          if (full_fetch_slot >= direct_slots.size()) return {};
          full_fetch_direct = !index_overwritten && fetch.stride() && !fetch.is_src_relative() &&
                              fetch.src() == 0 && fetch.src_swizzle() == 0;
          have_full_fetch = true;
        }
        if (full_fetch_direct && fetch.offset() >= 0) direct_slots.set(full_fetch_slot);
        else unsafe_slots.set(full_fetch_slot);
        // Mini fetches use the full fetch's latched address even if r0 has
        // since been overwritten. A subsequent full fetch must not use it.
        if (fetch.dest() == 0) index_overwritten = true;
      }
      if (cf.opcode() == ControlFlowOpcode::kExecEnd) return direct_slots & ~unsafe_slots;
    }
  }
  return {};
}

inline bool HasDirectVertexFetchPrefix(std::span<const uint32_t> words) {
  return AnalyzeDirectVertexFetches(words).any();
}

struct ScaledVertexFetch {
  uint32_t slot = UINT32_MAX;
  uint32_t constant_component = 0; // vertex float constant index * 4 + component
};

// A billboard compiler pattern: r0.z = r0.x * constant, immediately followed
// by one full fetch and its mini fetches. No other fetches are permitted, and
// the existing straight-line proof still validates the complete program.
// Keeping this deliberately narrow avoids assuming that arbitrary computed
// indices are bounded by the original draw indices.
inline ScaledVertexFetch AnalyzeScaledVertexFetch(std::span<const uint32_t> words) {
  using namespace rex::graphics;
  using namespace rex::graphics::ucode;
  if (words.size() < 9) return {};
  ControlFlowInstruction first_pair[2];
  UnpackControlFlowInstructions(words.data(), first_pair);
  const auto& first = first_pair[0];
  if ((first.opcode() != ControlFlowOpcode::kExec && first.opcode() != ControlFlowOpcode::kExecEnd) ||
      first.exec.count() < 2 || (first.exec.sequence() & 1) || !first.exec.address() ||
      uint64_t(first.exec.address() + first.exec.count()) * 3 > words.size()) return {};
  AluInstruction alu;
  std::memcpy(&alu, words.data() + first.exec.address() * 3, sizeof(alu));
  ParsedAluInstruction parsed;
  ParseAluInstruction(alu, xenos::ShaderType::kVertex, parsed);
  if (parsed.is_predicated) return {};
  const InstructionResult* result;
  const InstructionOperand* operands;
  uint32_t component;
  if (parsed.vector_opcode == AluVectorOpcode::kMul && parsed.vector_operand_count == 2 &&
      !parsed.scalar_result.GetUsedWriteMask() && parsed.scalar_opcode == AluScalarOpcode::kRetainPrev) {
    result = &parsed.vector_and_constant_result; operands = parsed.vector_operands; component = 2;
  } else if ((parsed.scalar_opcode == AluScalarOpcode::kMulsc0 ||
              parsed.scalar_opcode == AluScalarOpcode::kMulsc1) && parsed.scalar_operand_count == 2 &&
             !parsed.vector_and_constant_result.GetUsedWriteMask() &&
             (parsed.vector_opcode == AluVectorOpcode::kMax || parsed.vector_opcode == AluVectorOpcode::kAdd)) {
    result = &parsed.scalar_result; operands = parsed.scalar_operands; component = 0;
  } else return {};
  if (result->storage_target != InstructionStorageTarget::kRegister || result->storage_index != 0 ||
      result->storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute ||
      result->is_clamped || result->GetUsedWriteMask() != 4) return {};
  uint32_t scale = UINT32_MAX;
  bool index = false;
  for (uint32_t i = 0; i < 2; ++i) {
    const auto& op = operands[i];
    if (!op.component_count || op.is_negated || op.is_absolute_value ||
        op.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute) return {};
    const auto swizzle = op.GetComponent(component);
    if (op.storage_source == InstructionStorageSource::kRegister && op.storage_index == 0 &&
        swizzle == SwizzleSource::kX) index = true;
    else if (op.storage_source == InstructionStorageSource::kConstantFloat && op.storage_index < 256 &&
             swizzle >= SwizzleSource::kX && swizzle <= SwizzleSource::kW)
      scale = op.storage_index * 4 + uint32_t(swizzle) - uint32_t(SwizzleSource::kX);
    else return {};
  }
  if (!index || scale == UINT32_MAX) return {};
  std::vector<uint32_t> direct(words.begin(), words.end());
  uint32_t slot = UINT32_MAX;
  bool prefix_closed = false;
  for (uint32_t j = 1; j < first.exec.count(); ++j) {
    if (!((first.exec.sequence() >> (j * 2)) & 1)) { prefix_closed = true; continue; }
    if (prefix_closed) return {};
    FetchInstruction instruction;
    const uint32_t address = (first.exec.address() + j) * 3;
    std::memcpy(&instruction, words.data() + address, sizeof(instruction));
    if (instruction.opcode() != FetchOpcode::kVertexFetch) return {};
    const auto& fetch = instruction.vertex_fetch();
    if (j == 1) {
      if (fetch.is_mini_fetch() || fetch.src() != 0 || fetch.src_swizzle() != 2) return {};
      slot = fetch.fetch_constant_index();
      direct[address] &= ~(3u << 30); // prove the same fetch using the original r0.x
    } else if (!fetch.is_mini_fetch()) return {};
  }
  if (slot >= 96 || !AnalyzeDirectVertexFetches(direct).test(slot)) return {};
  // The proof above validates bounds and control flow; reject every fetch in
  // subsequent exec blocks, including re-execution of the prefix instructions.
  uint32_t pairs = uint32_t(words.size() / 3);
  for (uint32_t i = 0; i < pairs; ++i) {
    ControlFlowInstruction pair[2];
    UnpackControlFlowInstructions(words.data() + i * 3, pair);
    for (uint32_t k = 0; k < 2; ++k) {
      const auto& cf = pair[k];
      switch (cf.opcode()) {
        case ControlFlowOpcode::kExec: case ControlFlowOpcode::kExecEnd:
        case ControlFlowOpcode::kCondExec: case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredClean: break;
        default: continue;
      }
      pairs = std::min(pairs, cf.exec.address());
      if (i || k)
        for (uint32_t j = 0; j < cf.exec.count(); ++j)
          if ((cf.exec.sequence() >> (j * 2)) & 1) return {};
      if (cf.opcode() == ControlFlowOpcode::kExecEnd) return {slot, scale};
    }
  }
  return {};
}

} // namespace xn::gfx
