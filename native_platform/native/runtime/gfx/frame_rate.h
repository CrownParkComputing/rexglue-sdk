#pragma once
#include <cstdint>
#include <string_view>
struct SDL_Window;
namespace xn {
uint32_t FrameRate();
bool SetFrameRate(std::string_view value);
uint32_t CycleFrameRate(); // F6: 30 -> 60 -> 120 -> 30.
void UpdateFrameRateDisplay(SDL_Window* window, uint64_t swaps, uint32_t output_target = 0);
double MeasuredFrameRate();
bool FpsOverlayEnabled();
void ToggleFpsOverlay();
void SetFpsOverlayEnabled(bool enabled);
}
