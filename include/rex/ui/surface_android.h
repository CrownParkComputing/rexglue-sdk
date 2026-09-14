#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/ui/surface.h>

struct ANativeWindow;
struct SDL_Window;

namespace rex {
namespace ui {

// The presenter already knows how to make a VkSurfaceKHR from one of these -
// vkCreateAndroidSurfaceKHR and kTypeIndex_AndroidNativeWindow were both
// carried over from Xenia. Only the surface object itself was missing.
class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window, SDL_Window* sdl_window)
      : window_(window), sdl_window_(sdl_window) {}
  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  ANativeWindow* window_;
  SDL_Window* sdl_window_;
};

}  // namespace ui
}  // namespace rex
