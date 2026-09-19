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
#include <android/native_window.h>

#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/ui/surface_android.h>

namespace rex {
namespace ui {

AndroidNativeWindowSurface::AndroidNativeWindowSurface(ANativeWindow* window,
                                                       SDL_Window* sdl_window)
    : window_(window), sdl_window_(sdl_window) {
  // SDL keeps the Android surface at the physical display size. For handheld
  // performance profiles, ask SurfaceFlinger for smaller buffers so Vulkan
  // actually renders fewer pixels; the display still scales the buffer to the
  // panel. A zero cvar preserves the native display size.
  const int32_t width = REXCVAR_GET(window_width);
  const int32_t height = REXCVAR_GET(window_height);
  if (width > 0 && height > 0)
    ANativeWindow_setBuffersGeometry(window_, width, height, 0);
}

// Asked of SDL rather than of ANativeWindow_getWidth/Height, for the same
// reason the Wayland surface does: SDL reports the size in PIXELS, and on a
// device with a display scale the native window's own numbers are not the ones
// the swapchain must match.
bool AndroidNativeWindowSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  const int32_t configured_width = REXCVAR_GET(window_width);
  const int32_t configured_height = REXCVAR_GET(window_height);
  if (configured_width > 0 && configured_height > 0) {
    width_out = uint32_t(configured_width);
    height_out = uint32_t(configured_height);
    return true;
  }
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
