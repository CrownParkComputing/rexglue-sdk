// xenon-native: the frame recorder (oracle harness, W5). Wraps any IRenderer,
// forwards every call unchanged, and on the side keeps a per-frame draw
// digest and dumps presented frames, so a run can be diffed against the
// emulated port (tools/oracle/, docs/ORACLE.md).
//
// Files (prefix P = config.digest_prefix):
//   P_frames.csv   one row per swap: draw count, vertex sum, primitive
//                  histogram, shader hashes, register-file hashes
//   P_draws.csv    one row per draw: frame, index, opcode, prim, count,
//                  source, vs, ps, render-state hash, RB_MODECONTROL.edram_mode
//                  (6 = kCopy: the resolve helper draw the oracle never reports)
//   P_events.csv   script presses: name, seconds, frame index
// Pixel dumps (prefix D = config.dump_prefix): D_%04u.ppm from
// IRenderer::CaptureFrame, or D_%04u.draws.txt (the frame's draw log) when the
// renderer cannot read back (the stub).
//
// Frame index = number of swaps that preceded the frame's draws (0-based), the
// same convention as the oracle's [DRAWTRACE] swap= / REX_DUMP_FRAME index.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "renderer.h"

namespace xn {

struct FrameRecorderConfig {
  std::string digest_prefix;  // "" = no digest
  std::string dump_prefix;    // "" = no pixel dumps
  uint32_t dump_start = 0;
  uint32_t dump_every = 1;
  uint32_t dump_max = 0;      // 0 = unlimited
};

IRenderer* CreateFrameRecorder(IRenderer* inner, const FrameRecorderConfig& config);
// Records a scripted input press (no-op when no recorder exists).
void FrameRecorderNoteEvent(std::string_view name, double seconds, uint64_t frame);

// Shader hash as the oracle reports it: XXH3-64 over the raw (big-endian)
// microcode bytes as they sit in guest memory.
uint64_t ShaderUcodeHash(const uint32_t* dwords, uint32_t count);

}  // namespace xn
