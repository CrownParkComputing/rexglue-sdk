/**
 * @file        codegen/phase_gapfill.cpp
 * @brief       GapFill phase: find uncovered code regions and register them as functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/instruction.h"

#include <algorithm>

#include <rex/codegen/function_scanner.h>
#include <rex/codegen/phases.h>
#include "phase_helpers.h"

#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>

#include <ppc.h>

using rex::codegen::ppc::decode_instruction;
using rex::codegen::ppc::Opcode;
using rex::memory::load_and_swap;

namespace rex::codegen {

namespace {

//=============================================================================
// GapFill to register uncovered code regions
//=============================================================================

// Check if address looks like exception handler data (handler ptr + rdata ptr)
bool looksLikeExceptionData(const BinaryView& binary, const FunctionGraph& graph, uint32_t addr) {
  const uint8_t* data = binary.translate(addr);
  if (!data)
    return false;

  // Exception handler data pattern:
  // [addr+0]: pointer to __C_specific_handler (entry point)
  // [addr+4]: pointer to scope table in .rdata
  uint32_t firstDword = load_and_swap<uint32_t>(data);
  uint32_t secondDword = load_and_swap<uint32_t>(data + 4);

  // Check if first dword is a known entry point (like __C_specific_handler)
  if (!graph.isEntryPoint(firstDword)) {
    return false;
  }

  // Check if second dword points to .rdata section
  auto* rdataSection = binary.findSectionByName(".rdata");
  if (!rdataSection)
    return false;

  uint32_t rdataStart = rdataSection->baseAddress;
  uint32_t rdataEnd = rdataStart + rdataSection->size;

  if (secondDword >= rdataStart && secondDword < rdataEnd) {
    REXCODEGEN_TRACE(
        "GapFill: 0x{:08X} looks like exception data (handler=0x{:08X}, scope=0x{:08X}), skipping",
        addr, firstDword, secondDword);
    return true;
  }

  return false;
}

void gapFillCodeRegions(CodegenContext& ctx) {
  REXCODEGEN_TRACE("Analyze: checking for uncovered code regions...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& scan = ctx.scan;

  size_t gapsFound = 0;
  size_t segmentsCreated = 0;

  // Sweep each region linearly, letting block discovery decide where each
  // uncovered function actually ends.
  //
  // Splitting a region on terminators cannot do this correctly: a vtable
  // dispatch thunk ends in bctr, a shared epilogue is entered by an
  // unconditional branch, and a tail call only terminates when its target is
  // already known - so the result depends on the order regions are visited and
  // routinely swallows the function that follows. Anything swallowed that way
  // is unreachable at runtime, surfacing as "call to invalid or unregistered
  // function" rather than as an analysis error. Walking uncovered code and
  // taking the extent of the discovered blocks removes the guesswork.
  FunctionScanner scanner(binary);

  for (const auto& region : scan.codeRegions) {
    uint32_t addr = region.start;
    gapsFound++;

    while (addr < region.end) {
      if (auto* containingFunc = graph.getFunctionContaining(addr)) {
        // Already covered - resume after it. end() can sit behind addr for a
        // zero-sized node, so never move backwards.
        addr = std::max(containingFunc->end(), addr + 4);
        continue;
      }

      const uint8_t* data = binary.translate(addr);
      if (!data)
        break;

      // Alignment padding between functions is not code.
      if (load_and_swap<uint32_t>(data) == 0) {
        addr += 4;
        continue;
      }

      if (looksLikeExceptionData(binary, graph, addr)) {
        addr += 8;
        continue;
      }

      auto discovered = scanner.discover_blocks(addr);

      // Take the extent of the contiguous run of blocks starting at the entry,
      // not the furthest block. Block discovery follows an unconditional
      // branch as if it were internal flow, so a two-instruction thunk that
      // tail calls far away would otherwise claim everything in between -
      // swallowing the thunks that follow it.
      auto& blocks = discovered.blocks;
      std::sort(blocks.begin(), blocks.end(),
                [](const DiscoveredBlock& a, const DiscoveredBlock& b) { return a.base < b.base; });

      uint32_t end = addr;
      for (const auto& block : blocks) {
        if (block.base > end)
          break;  // discontinuity: the rest belongs to another function
        end = std::max<uint32_t>(end, block.end);
      }

      if (end <= addr) {
        addr += 4;
        continue;
      }

      graph.addFunction(addr, end - addr, FunctionAuthority::GAP_FILL, false);
      REXCODEGEN_TRACE("GapFill: registered sub_{:08X} (0x{:08X}-0x{:08X}, {} bytes)", addr, addr,
                       end, end - addr);
      segmentsCreated++;
      addr = end;
    }
  }

  if (segmentsCreated > 0) {
    REXCODEGEN_TRACE("Analyze: registered {} gap functions from {} regions", segmentsCreated,
                     gapsFound);
  } else {
    REXCODEGEN_TRACE("Analyze: no uncovered regions found");
  }
}

//=============================================================================
// Cleanup absorbed GAP_FILL functions
//=============================================================================

void cleanupAbsorbedGapFills(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  std::vector<uint32_t> toRemove;

  for (const auto& [addr, node] : graph.functions()) {
    if (node->authority() != FunctionAuthority::GAP_FILL)
      continue;

    for (const auto& [otherAddr, otherNode] : graph.functions()) {
      if (otherAddr == addr)
        continue;
      if (!otherNode->containsAddress(addr))
        continue;

      // This GAP_FILL is inside another function's blocks
      if (otherNode->authority() != FunctionAuthority::GAP_FILL) {
        // Absorbed by higher authority - remove
        toRemove.push_back(addr);
        break;
      } else if (otherAddr < addr) {
        // Both GAP_FILL, other has lower address - it survives
        toRemove.push_back(addr);
        break;
      }
    }
  }

  for (uint32_t addr : toRemove) {
    graph.removeFunction(addr);
  }

  if (!toRemove.empty()) {
    REXCODEGEN_TRACE("Analyze: removed {} absorbed GAP_FILL functions", toRemove.size());
  }
}

}  // anonymous namespace

namespace phases {

VoidResult GapFill(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  gapFillCodeRegions(ctx);

  // Discover blocks for gap-filled functions
  auto known = buildKnownFunctions(ctx.graph, /*excludeGapFill=*/true);
  size_t discovered = discoverPendingFunctions(ctx, known);
  REXCODEGEN_TRACE("Analyze: discovered blocks for {} gap-filled functions", discovered);

  cleanupAbsorbedGapFills(ctx);

  return Ok();
}

}  // namespace phases

}  // namespace rex::codegen
