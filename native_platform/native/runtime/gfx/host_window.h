#pragma once

#include <SDL3/SDL.h>
#include <string_view>
#include <rex/logging.h>

namespace xn {
// Common desktop controls for every conversion. The game's rendering size is
// independent of the window's output size.
struct HostWindowOptions {
  bool fullscreen = false;
  bool Parse(std::string_view argument) {
    if (argument == "--fullscreen") { fullscreen = true; return true; }
    if (argument == "--windowed") { fullscreen = false; return true; }
    return false;
  }
  SDL_Window* Create(const char* title) const {
    SDL_Window* window = SDL_CreateWindow(title,1280,720,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window && fullscreen && !SDL_SetWindowFullscreen(window,true))
      REXLOG_WARN("cannot enter fullscreen: {}",SDL_GetError());
    return window;
  }
};

inline void HandleHostWindowEvent(SDL_Window* window, const SDL_Event& event) {
  if (!window) return;
  if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
      event.key.windowID == SDL_GetWindowID(window) &&
      (event.key.key == SDLK_F11 || (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))) {
    const bool enter = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
    if (!SDL_SetWindowFullscreen(window,enter))
      REXLOG_WARN("cannot change fullscreen mode: {}",SDL_GetError());
  }
  if ((event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) &&
      event.window.windowID == SDL_GetWindowID(window))
    REXLOG_INFO("Fullscreen mode: {}",event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ? "ON" : "OFF");
}
} // namespace xn
