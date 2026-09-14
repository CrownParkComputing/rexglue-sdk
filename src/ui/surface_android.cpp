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

#include <SDL3/SDL.h>

#include <rex/ui/surface_android.h>

namespace rex {
namespace ui {

// Asked of SDL rather than of ANativeWindow_getWidth/Height, for the same
// reason the Wayland surface does: SDL reports the size in PIXELS, and on a
// device with a display scale the native window's own numbers are not the ones
// the swapchain must match.
bool AndroidNativeWindowSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  int w = 0, h = 0;
  if (!SDL_GetWindowSizeInPixels(sdl_window_, &w, &h) || w <= 0 || h <= 0) {
    return false;
  }
  width_out = uint32_t(w);
  height_out = uint32_t(h);
  return true;
}

}  // namespace ui
}  // namespace rex
