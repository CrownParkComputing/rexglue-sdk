/**
 * @file        ui/windowed_app_main_sdl.cpp
 * @brief       Entry point for windowed applications (SDL3 windowing)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/platform.h>
#if REX_PLATFORM_ANDROID
// SDLActivity dlopens this library and looks for SDL_main, not main. Including
// this header renames main() to an exported SDL_main; the library is built with
// hidden visibility, so without the header's export decoration the symbol is
// present but not findable and the app exits the instant it starts.
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#if defined(REX_HAS_RAYLIB_DISPLAY) && REX_HAS_RAYLIB_DISPLAY
#include <atomic>
#include <thread>

#include "raylib_display.h"
#endif

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#endif

namespace {

int RunWindowedApp(int argc, char** argv) {
#if REX_PLATFORM_ANDROID
  // SDL's Java argument bridge is optional on vendor Android builds. Keep the
  // native entry point self-contained so a port still launches when that
  // bridge supplies an empty argv: SDL exposes the app-specific external files
  // directory, which is where the APK deploy script installs game and user
  // data. These are appended only as fallbacks, so explicit Java/device args
  // continue to win through the normal cvar precedence rules.
  std::vector<std::string> android_fallback_args;
  std::vector<std::string> remaining;
  const char* android_files = SDL_GetAndroidExternalStoragePath();
  bool has_game_root = false;
  bool has_user_root = false;
  if (android_files && *android_files) {
    for (int i = 1; i < argc; ++i) {
      if (!argv[i]) continue;
      has_game_root |= std::string_view(argv[i]).starts_with("--game_data_root");
      has_user_root |= std::string_view(argv[i]).starts_with("--user_data_root");
    }
    if (!has_game_root)
      android_fallback_args.emplace_back(std::string("--game_data_root=") + android_files + "/game");
    if (!has_user_root)
      android_fallback_args.emplace_back(std::string("--user_data_root=") + android_files + "/user");
    // 640x360 is the low Android profile. It cuts presentation bandwidth and
    // keeps the guest output readable when the panel is scaled up by Android.
    android_fallback_args.emplace_back("--window_width=640");
    android_fallback_args.emplace_back("--window_height=360");
    android_fallback_args.emplace_back("--resolution=640x360");
    std::vector<char*> fallback_argv;
    fallback_argv.reserve(static_cast<size_t>(argc) + android_fallback_args.size());
    for (int i = 0; i < argc; ++i) fallback_argv.push_back(argv[i]);
    for (auto& arg : android_fallback_args) fallback_argv.push_back(arg.data());
    fallback_argv.push_back(nullptr);
    argc = static_cast<int>(fallback_argv.size()) - 1;
    argv = fallback_argv.data();
    remaining = rex::cvar::Init(argc, argv);
    rex::cvar::ApplyEnvironment();
    rex::InitLoggingEarly();
  }
#else
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();
#endif

#if defined(REX_HAS_RAYLIB_DISPLAY) && REX_HAS_RAYLIB_DISPLAY
  // Reaches into the SDL app's context from raylib's on_close callback, which
  // runs on a different thread - see the comment below on why the two loops
  // are split across threads at all. Set once app_context exists, cleared
  // before it is destroyed, so on_close never touches a dangling context.
  std::atomic<rex::ui::SDLWindowedAppContext*> live_context{nullptr};
#endif

  // The SDL app's whole lifetime: construct its context, build the app,
  // initialize it, pump its loop, tear it down. Ordinarily this just runs on
  // the calling thread; with the raylib display on, it runs on a background
  // thread instead (below) because SDLWindowedAppContext fixes "the UI
  // thread" to whichever thread constructs it - IsInUIThread() and
  // HasQuitFromUIThread() assert on that identity, so the construction has to
  // happen on the same thread as the loop, not just the loop call.
  auto run_sdl_app = [&]() -> int {
    int result;
    rex::ui::SDLWindowedAppContext app_context;
    if (!app_context.Initialize()) {
      return EXIT_FAILURE;
    }
#if defined(REX_HAS_RAYLIB_DISPLAY) && REX_HAS_RAYLIB_DISPLAY
    live_context.store(&app_context, std::memory_order_release);
#endif

#if REX_PLATFORM_WIN32
    // Apartment-threaded COM for shell dialogs.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      return EXIT_FAILURE;
    }
#endif

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    // Match remaining positional args to the app's expected options.
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;

#if defined(REX_HAS_RAYLIB_DISPLAY) && REX_HAS_RAYLIB_DISPLAY
    // The SDL app quit on its own (ESC, a crash, the title exiting) - close
    // the raylib window with it rather than leaving it open, which would
    // hang the join below forever.
    live_context.store(nullptr, std::memory_order_release);
    rex::ui::raylib_display::Stop();
#endif

    app->InvokeOnDestroy();

#if REX_PLATFORM_WIN32
    CoUninitialize();
#endif
    return result;
  };

#if defined(REX_HAS_RAYLIB_DISPLAY) && REX_HAS_RAYLIB_DISPLAY
  if (rex::ui::raylib_display::Enabled()) {
    // raylib's GLFW window has to be created on the process's real first
    // thread: creating it from any other thread, in a process where
    // SDL3/Vulkan already owns a window on the main thread, deadlocks
    // NVIDIA's proprietary driver - found live running Daytona (see
    // HANDOVER.md in daytona-recomp), not a hang worth chasing with a mutex,
    // since the two windows simply cannot both be created off the main
    // thread in one process on this driver. So the SDL app moves to a
    // background thread instead, and this thread stays raylib's.
    int sdl_result = EXIT_SUCCESS;
    std::thread sdl_thread([&] { sdl_result = run_sdl_app(); });
    rex::ui::raylib_display::RunOnMainThread([&] {
      // The raylib window closed on its own (the user closed it) - tell the
      // SDL app to quit too. RequestDeferredQuit is documented safe from any
      // thread, which this is: raylib's main thread, not the SDL one.
      if (rex::ui::SDLWindowedAppContext* ctx =
              live_context.load(std::memory_order_acquire)) {
        ctx->RequestDeferredQuit();
      }
    });
    sdl_thread.join();
    return sdl_result;
  }
#endif

  return run_sdl_app();
}

#if REX_PLATFORM_WIN32
// Convert wide argv from CommandLineToArgvW to UTF-8 for cvar::Init.
std::vector<std::string> WideArgsToUtf8(int argc, wchar_t** wargv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    std::wstring wide(wargv[i]);
    if (wide.empty()) {
      args.emplace_back();
      continue;
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr,
                                   0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size,
                        nullptr, nullptr);
    args.push_back(std::move(utf8));
  }
  return args;
}
#endif

}  // namespace

#if REX_PLATFORM_WIN32

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hinstance_prev, LPWSTR command_line,
                    int show_cmd) {
  (void)hinstance;
  (void)hinstance_prev;
  (void)command_line;
  (void)show_cmd;

  int wargc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  auto utf8_args = WideArgsToUtf8(wargc, wargv);
  LocalFree(wargv);

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(utf8_args.size());
  for (auto& s : utf8_args) {
    argv_ptrs.push_back(s.data());
  }
  return RunWindowedApp(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
}

#else

int main(int argc, char* argv[]) {
  return RunWindowedApp(argc, argv);
}

#endif
