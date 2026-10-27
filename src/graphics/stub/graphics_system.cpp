/**
 * @file        graphics/stub/graphics_system.cpp
 * @brief       rexgpu-stub: headless guest GPU
 *
 * @copyright   Copyright (c) 2026 ReXGlue contributors.
 * @license     BSD 3-Clause License.
 */

#include "stub/graphics_system.h"

#include "stub/command_processor.h"

namespace rex::graphics::stub {

std::unique_ptr<CommandProcessor> StubGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<StubCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::stub
