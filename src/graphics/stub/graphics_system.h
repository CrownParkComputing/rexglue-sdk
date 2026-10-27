/**
 * @file        graphics/stub/graphics_system.h
 * @brief       rexgpu-stub: headless guest GPU
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 *
 * @remarks     Guest-visible Xenos GPU state (register MMIO, ring buffer
 *              tracking, vblank source, interrupt dispatch, memory
 *              writebacks) with no host GPU behind it. The command
 *              processor consumes the PM4 stream so fence writebacks and
 *              read-pointer writeback still reach guest memory, but every
 *              draw/copy is counted and skipped and swaps go nowhere.
 *
 *              Exists for per-title bring-up and CPU-side benchmarking:
 *              isolates guest simulation + command-buffer generation cost
 *  its             from all GPU emulation cost. Selected with
 *              `gpu_plugin = "stub"`.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/graphics_system.h>
#include <rex/system/xtypes.h>

namespace rex::graphics::stub {

class StubGraphicsSystem final : public GraphicsSystem {
 public:
  StubGraphicsSystem() = default;
  ~StubGraphicsSystem() override = default;

  std::string name() const override { return "stub"; }

  // Headless: no provider, no presenter. The app still creates its window;
  // nothing is ever drawn into it. has_presentation() stays false, so the
  // app takes no immediate-drawer path and overlays are unavailable - this
  // is a benchmark/bring-up mode, use logs.
  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext* app_context) override {
    return X_STATUS_SUCCESS;
  }

 protected:
  void CreateProvider(bool with_presentation) override {}
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::stub
