#include "frame_rate.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <SDL3/SDL.h>
#include <rex/logging.h>
namespace xn {
namespace {
// A boot-time experiment, separate from the presentation cap. Titles cache
// video-mode information during device creation, so changing only vblank at
// runtime would leave their scheduler with a different refresh rate.
std::atomic<uint32_t> rate{[] {
  const char* value = std::getenv("XN_GUEST_REFRESH_HZ");
  return value && std::strcmp(value, "120") == 0 ? 120u : 60u;
}()};
std::atomic<double> g_measured_fps{60.0};
std::atomic<bool> g_fps_overlay{true};
}
uint32_t FrameRate() { return rate.load(std::memory_order_relaxed); }
bool SetFrameRate(std::string_view value) {
  if (value == "30") rate = 30;
  else if (value == "60") rate = 60;
  else if (value == "120") rate = 120;
  else return false;
  return true;
}
uint32_t CycleFrameRate() { uint32_t next = FrameRate() == 30 ? 60 : FrameRate() == 60 ? 120 : 30; rate = next; return next; }
double MeasuredFrameRate() { return g_measured_fps.load(std::memory_order_relaxed); }
bool FpsOverlayEnabled() { return g_fps_overlay.load(std::memory_order_relaxed); }
void ToggleFpsOverlay() { g_fps_overlay.store(!g_fps_overlay.load(std::memory_order_relaxed), std::memory_order_relaxed); }
void SetFpsOverlayEnabled(bool enabled) { g_fps_overlay.store(enabled, std::memory_order_relaxed); }

void UpdateFrameRateDisplay(SDL_Window* window, uint64_t swaps, uint32_t output_target) {
  // Called only by the SDL owner thread; one title runs in each process.
  using clock = std::chrono::steady_clock;
  static auto since = clock::now();
  static uint64_t previous = swaps;
  static uint32_t displayed_target = 0;
  static double measured = 60.0;
  static const std::string caption = window ? SDL_GetWindowTitle(window) : "";
  auto now = clock::now();
  double seconds = std::chrono::duration<double>(now - since).count();
  const uint32_t target=output_target?output_target:FrameRate();
  bool refresh = target != displayed_target;
  if (seconds >= 1.0) {
    measured = double(swaps - previous) / seconds;
    g_measured_fps.store(measured, std::memory_order_relaxed);
    previous = swaps; since = now; refresh = true;
    REXLOG_INFO("FPS target={} measured={:.1f}", target, measured);
  }
  if (refresh) {
    displayed_target = target;
    if (window) SDL_SetWindowTitle(window, std::format("{} | {:.1f} FPS | target {}", caption, measured, displayed_target).c_str());
  }
}
}
