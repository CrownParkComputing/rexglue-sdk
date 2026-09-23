#pragma once

#include <functional>

namespace rex {
namespace ui {

// In-process raylib input endpoint, active only when REX_RAYLIB_DISPLAY=1.
// Reads keyboard/real-pad input and feeds it back into the runtime's SDL
// input as a uinput virtual Xbox 360 pad. Display is deliberately NOT part of
// this: the game's own SDL3/Vulkan window is the only thing on screen -
// raylib just needs a hidden window to read input at all.
//
// RunOnMainThread MUST be called from the process's real first thread, and
// the platform's own SDL/Vulkan windowing loop moves to a background thread
// instead - see the comment in windowed_app_main_sdl.cpp. Creating a GLFW
// window off-thread while another toolkit already owns a window on the main
// thread deadlocks NVIDIA's proprietary driver; this was found live running
// Daytona (see HANDOVER.md in daytona-recomp) and applies even to a hidden
// window, since the driver hazard is about context creation, not visibility.
namespace raylib_display {

// Whether REX_RAYLIB_DISPLAY=1 was set. Check this before paying for the
// thread split in windowed_app_main_sdl.cpp.
bool Enabled();

// Runs raylib's hidden window + input loop on the calling thread until the
// window is closed or Stop() is called from elsewhere. on_close is invoked
// once, from this same thread, right before returning - the caller uses it
// to tell the platform's own UI loop to quit too.
void RunOnMainThread(std::function<void()> on_close);

// Asks RunOnMainThread's loop to end. Idempotent, safe from any thread. Call
// this once the platform's own UI loop has quit on its own (ESC, a crash,
// the title exiting) so the raylib loop ends with it rather than outliving
// it and hanging shutdown.
void Stop();

}  // namespace raylib_display
}  // namespace ui
}  // namespace rex
