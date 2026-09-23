#include "raylib_display.h"

#include <raylib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/graphics/flags.h>
#include <rex/ui/flags.h>

namespace rex {
namespace ui {
namespace raylib_display {
namespace {

std::atomic<bool> stop_requested{false};

// evdev name of the uinput pad VirtPad creates. raylib's own gamepad
// enumeration can see it (GLFW hands out slots in /dev/input order, so with
// no other pad it lands on index 0) - and forwarding it 1:1 would mean
// reading back our own output, masking both the keyboard and any real pad.
constexpr const char* kVirtualPadName = "Microsoft X-Box 360 pad";

// First raylib gamepad that is not our own virtual pad, or -1.
int FindRealGamepad() {
  for (int i = 0; i < 4; ++i) {
    if (!IsGamepadAvailable(i)) {
      continue;
    }
    const char* name = GetGamepadName(i);
    if (!name || strcmp(name, kVirtualPadName) != 0) {
      return i;
    }
  }
  return -1;
}

// uinput virtual Xbox 360 pad. SDL in-process picks it up as a real
// controller regardless of window focus.
struct VirtPad {
  int fd = -1;

  bool Create() {
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
      REXLOG_WARN("raylib_display: cannot open /dev/uinput: {}", strerror(errno));
      return false;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    for (int b : {BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH, BTN_TL, BTN_TR,
                  BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR}) {
      ioctl(fd, UI_SET_KEYBIT, b);
    }
    for (int a : {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X,
                  ABS_HAT0Y}) {
      ioctl(fd, UI_SET_ABSBIT, a);
    }
    uinput_setup setup{};
    snprintf(setup.name, sizeof(setup.name), "%s", kVirtualPadName);
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x045e;
    setup.id.product = 0x028e;
    setup.id.version = 0x0114;
    ioctl(fd, UI_DEV_SETUP, &setup);
    auto set_abs = [&](int axis, int mn, int mx) {
      uinput_abs_setup abs{};
      abs.code = axis;
      abs.absinfo.minimum = mn;
      abs.absinfo.maximum = mx;
      ioctl(fd, UI_ABS_SETUP, &abs);
    };
    set_abs(ABS_X, -32768, 32767);
    set_abs(ABS_Y, -32768, 32767);
    set_abs(ABS_RX, -32768, 32767);
    set_abs(ABS_RY, -32768, 32767);
    set_abs(ABS_Z, 0, 255);
    set_abs(ABS_RZ, 0, 255);
    set_abs(ABS_HAT0X, -1, 1);
    set_abs(ABS_HAT0Y, -1, 1);
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
      REXLOG_WARN("raylib_display: UI_DEV_CREATE: {}", strerror(errno));
      close(fd);
      fd = -1;
      return false;
    }
    return true;
  }

  void Emit(int type, int code, int value) {
    input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) { /* device went away; keep going */ }
  }
  void Key(int code, bool down) { Emit(EV_KEY, code, down ? 1 : 0); }
  void Abs(int code, int value) { Emit(EV_ABS, code, value); }
  void Sync() { Emit(EV_SYN, SYN_REPORT, 0); }

  ~VirtPad() {
    if (fd >= 0) {
      ioctl(fd, UI_DEV_DESTROY);
      close(fd);
    }
  }
};

// Logical pad state gathered each frame from keyboard + real pad.
struct PadState {
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint8_t lt = 0, rt = 0;
  int8_t hatx = 0, haty = 0;
  bool a = false, b = false, x = false, y = false;
  bool lb = false, rb = false, back = false, start = false, l3 = false, r3 = false;
};

PadState GatherPadState(int real_pad) {
  PadState s;
  if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A)) s.lx = -32768;
  if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) s.lx = 32767;
  if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W)) s.ly = -32768;
  if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S)) s.ly = 32767;
  if (IsKeyDown(KEY_J)) s.rx = -32768;
  if (IsKeyDown(KEY_L)) s.rx = 32767;
  if (IsKeyDown(KEY_I)) s.ry = -32768;
  if (IsKeyDown(KEY_K)) s.ry = 32767;
  s.a = IsKeyDown(KEY_SPACE);
  s.b = IsKeyDown(KEY_LEFT_SHIFT);
  s.x = IsKeyDown(KEY_C);
  s.y = IsKeyDown(KEY_V);
  s.lb = IsKeyDown(KEY_F);
  s.rb = IsKeyDown(KEY_G);
  s.back = IsKeyDown(KEY_TAB);
  s.start = IsKeyDown(KEY_ENTER);
  s.lt = IsKeyDown(KEY_Q) ? 255 : 0;
  s.rt = IsKeyDown(KEY_R) ? 255 : 0;

  // A real pad forwarded 1:1 wins over the keyboard. Never our own virtual
  // pad (FindRealGamepad) - that would echo our own output back as input.
  if (real_pad >= 0) {
    auto axis16 = [](float v) -> int16_t {
      return static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767);
    };
    s.lx = axis16(GetGamepadAxisMovement(real_pad, GAMEPAD_AXIS_LEFT_X));
    s.ly = axis16(GetGamepadAxisMovement(real_pad, GAMEPAD_AXIS_LEFT_Y));
    s.rx = axis16(GetGamepadAxisMovement(real_pad, GAMEPAD_AXIS_RIGHT_X));
    s.ry = axis16(GetGamepadAxisMovement(real_pad, GAMEPAD_AXIS_RIGHT_Y));
    s.lt = (uint8_t)(std::clamp(GetGamepadAxisMovement(real_pad, GAMEPAD_AXIS_LEFT_TRIGGER), 0.0f,
                                1.0f) *
                     255);
    s.rt = (uint8_t)(std::clamp(GetGamepadAxisMovement(real_pad, GAMEPAD_AXIS_RIGHT_TRIGGER), 0.0f,
                                1.0f) *
                     255);
    s.a = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    s.b = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
    s.x = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
    s.y = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_RIGHT_FACE_UP);
    s.lb = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_LEFT_TRIGGER_1);
    s.rb = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1);
    s.back = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_MIDDLE_LEFT);
    s.start = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_MIDDLE_RIGHT);
    s.l3 = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_LEFT_THUMB);
    s.r3 = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_RIGHT_THUMB);
    s.hatx = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_LEFT_FACE_LEFT)   ? -1
             : IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) ? 1
                                                                             : 0;
    s.haty = IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_LEFT_FACE_UP)     ? -1
             : IsGamepadButtonDown(real_pad, GAMEPAD_BUTTON_LEFT_FACE_DOWN) ? 1
                                                                            : 0;
  }
  return s;
}

// The FPS cap steps this cycles through with left/right - matches rexmenu's
// desktop menu, so the two aren't teaching two different vocabularies.
constexpr int kFpsCapValues[] = {0, 30, 60, 120};
constexpr int kFpsCapCount = 4;

// vsync_fps_cap's REXCVAR_DEFINE_INT32 lives in command_processor.cpp, which
// is compiled separately into each GPU plugin - not into rexui, where this
// file lives. REXCVAR_QUERY (goes through the registry by name) rather than
// REXCVAR_GET (a direct symbol call) is the documented way to read a cvar
// whose defining TU isn't on the reader's own link line; the same is true of
// SetFlagByName below in place of REXCVAR_SET.
int32_t VsyncFpsCap() { return REXCVAR_QUERY(int32_t, vsync_fps_cap); }
void SetVsyncFpsCap(int32_t value) {
  rex::cvar::SetFlagByName("vsync_fps_cap", std::to_string(value));
}

int FpsCapIndexFromCvar() {
  const int32_t v = VsyncFpsCap();
  for (int i = 0; i < kFpsCapCount; ++i) {
    if (kFpsCapValues[i] == v) return i;
  }
  return 0;
}

enum class PauseAction { kNone, kResume, kReturnToLauncher };

// The Start-triggered pause overlay: raises this window over the game (it
// stays hidden the rest of the time - see RunLoop) and lets Back+Start's own
// destination, "leave", be reached from a menu instead of only a held combo.
// Everything it touches is a REXCVAR the game's own systems already read, so
// there is nothing here the game doesn't already know how to react to.
PauseAction DrawPauseMenu(int& sel, int real_pad) {
  constexpr const char* kItems[] = {
      "Resume", "Side panels", "FPS display", "FPS limiter",
      "Return to launcher",
  };
  constexpr int kItemCount = 5;

  // Raylib's own IsGamepadButtonPressed/IsKeyPressed are already press-edges
  // (unlike PadState in GatherPadState, which is a level read for 1:1
  // forwarding) - exactly what a menu wants, and free of the debouncing that
  // would otherwise need doing by hand here.
  const bool pad_here = real_pad >= 0;
  const bool up = IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W) ||
                  (pad_here && IsGamepadButtonPressed(real_pad, GAMEPAD_BUTTON_LEFT_FACE_UP));
  const bool down = IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S) ||
                    (pad_here && IsGamepadButtonPressed(real_pad, GAMEPAD_BUTTON_LEFT_FACE_DOWN));
  const bool left = IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A) ||
                    (pad_here && IsGamepadButtonPressed(real_pad, GAMEPAD_BUTTON_LEFT_FACE_LEFT));
  const bool right = IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D) ||
                     (pad_here && IsGamepadButtonPressed(real_pad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT));
  const bool activate =
      IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
      (pad_here && IsGamepadButtonPressed(real_pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));

  if (up) sel = (sel + kItemCount - 1) % kItemCount;
  if (down) sel = (sel + 1) % kItemCount;

  PauseAction action = PauseAction::kNone;

  switch (sel) {
    case 0:
      if (activate) action = PauseAction::kResume;
      break;
    case 1:
      if (activate || left || right)
        REXCVAR_SET(show_side_panels, !REXCVAR_GET(show_side_panels));
      break;
    case 2:
      if (activate || left || right)
        REXCVAR_SET(show_fps, !REXCVAR_GET(show_fps));
      break;
    case 3:
      if (left || right) {
        const int idx = (FpsCapIndexFromCvar() +
                         (right ? 1 : kFpsCapCount - 1)) %
                        kFpsCapCount;
        SetVsyncFpsCap(kFpsCapValues[idx]);
      }
      break;
    case 4:
      if (activate) action = PauseAction::kReturnToLauncher;
      break;
  }

  BeginDrawing();
  ClearBackground(Color{0, 0, 0, 200});
  const int margin = 24;
  DrawText("PAUSED", margin, margin, 32, RAYWHITE);
  for (int i = 0; i < kItemCount; ++i) {
    std::string label = kItems[i];
    if (i == 1)
      label += REXCVAR_GET(show_side_panels) ? ": Shown" : ": Hidden";
    if (i == 2) label += REXCVAR_GET(show_fps) ? ": On" : ": Off";
    if (i == 3) {
      const int v = VsyncFpsCap();
      label += ": < " + (v == 0 ? std::string("Off") : std::to_string(v)) + " >";
    }
    const int y = margin + 50 + i * 36;
    const bool is_sel = i == sel;
    if (is_sel) DrawRectangle(margin - 6, y - 4, 300, 30,
                              Color{40, 110, 120, 255});
    DrawText(label.c_str(), margin, y, 22, RAYWHITE);
  }
  DrawText("Up/Down select, Left/Right change, Enter/A confirm",
           margin, GetScreenHeight() - 30, 14, GRAY);
  EndDrawing();

  return action;
}

// Input-only: no window is ever shown, except for the Start-triggered pause
// overlay above - the game's own SDL3/Vulkan window stays the only thing on
// screen otherwise. REX_RAYLIB_INPUT_WINDOW=1 keeps the window visible
// instead, so scripted input (which needs a focused window) can drive it.
void RunLoop(const std::function<void()>& on_close) {
  VirtPad pad;
  const bool pad_ok = pad.Create();
  if (!pad_ok) {
    REXLOG_WARN("raylib_display: virtual pad unavailable; input reading is a no-op");
  }

  static const bool window_visible = [] {
    const char* value = getenv("REX_RAYLIB_INPUT_WINDOW");
    return value && *value == '1';
  }();
  if (!window_visible) {
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
  }
  InitWindow(320, 240, "rex-raylib-input");
  if (!IsWindowReady()) {
    REXLOG_ERROR("raylib_display: InitWindow failed (no display?)");
    return;
  }
  SetTargetFPS(60);
  REXLOG_INFO("raylib_display: reading input (REX_RAYLIB_DISPLAY=1), pad {}",
              pad_ok ? "virtual x360" : "OFF");

  // Back+Start held together is a fallback clean-exit gesture, kept even now
  // that "Return to launcher" exists in the pause menu below: it works even
  // if the overlay itself never opens for some reason. Held, not tapped, so
  // it can't be hit in the middle of ordinary play.
  double quit_combo_held_since = 0.0;
  constexpr double kQuitComboHoldSeconds = 1.0;

  bool paused = false;
  bool start_prev = false;
  int pause_sel = 0;

  while (!WindowShouldClose() && !stop_requested.load(std::memory_order_relaxed)) {
    if (pad_ok) {
      const int real_pad = FindRealGamepad();
      const PadState s = GatherPadState(real_pad);
      const bool start_edge = s.start && !start_prev;
      start_prev = s.start;

      if (start_edge) {
        paused = !paused;
        if (paused) {
          pause_sel = 0;
          ClearWindowState(FLAG_WINDOW_HIDDEN);
        } else {
          SetWindowState(FLAG_WINDOW_HIDDEN);
        }
      }

      if (paused) {
        const PauseAction action = DrawPauseMenu(pause_sel, real_pad);
        if (action == PauseAction::kResume) {
          paused = false;
          SetWindowState(FLAG_WINDOW_HIDDEN);
        } else if (action == PauseAction::kReturnToLauncher) {
          REXLOG_INFO("raylib_display: Return to launcher selected");
          break;
        }
        // Paused: the pad is not forwarded, so menu navigation never leaks
        // into the game as movement - it is frozen exactly where it was.
      } else {
        if (s.back && s.start) {
          const double now = GetTime();
          if (quit_combo_held_since == 0.0) quit_combo_held_since = now;
          if (now - quit_combo_held_since >= kQuitComboHoldSeconds) {
            REXLOG_INFO("raylib_display: Back+Start held - clean exit requested");
            break;
          }
        } else {
          quit_combo_held_since = 0.0;
        }
        pad.Abs(ABS_X, s.lx);
        pad.Abs(ABS_Y, s.ly);
        pad.Abs(ABS_RX, s.rx);
        pad.Abs(ABS_RY, s.ry);
        pad.Abs(ABS_Z, s.lt);
        pad.Abs(ABS_RZ, s.rt);
        pad.Abs(ABS_HAT0X, s.hatx);
        pad.Abs(ABS_HAT0Y, s.haty);
        pad.Key(BTN_SOUTH, s.a);
        pad.Key(BTN_EAST, s.b);
        pad.Key(BTN_WEST, s.x);
        pad.Key(BTN_NORTH, s.y);
        pad.Key(BTN_TL, s.lb);
        pad.Key(BTN_TR, s.rb);
        pad.Key(BTN_SELECT, s.back);
        pad.Key(BTN_START, s.start);
        pad.Key(BTN_THUMBL, s.l3);
        pad.Key(BTN_THUMBR, s.r3);
        pad.Sync();
      }
    }
    // No display outside the pause overlay: WindowShouldClose() above already
    // pumps events (it calls PollInputEvents internally), which is what keeps
    // gamepad/keyboard state current either way. Pace the loop when there is
    // nothing to draw - SetTargetFPS only applies to EndDrawing, so without a
    // sleep here this spins a core flat out.
    if (!paused) WaitTime(0.001);
  }

  CloseWindow();
  REXLOG_INFO("raylib_display: input loop stopped");
  if (on_close) {
    on_close();
  }
}

}  // namespace

bool Enabled() {
  static const bool enabled = [] {
    const char* value = getenv("REX_RAYLIB_DISPLAY");
    return value && *value == '1';
  }();
  return enabled;
}

void RunOnMainThread(std::function<void()> on_close) {
  RunLoop(on_close);
}

void Stop() { stop_requested.store(true, std::memory_order_relaxed); }

}  // namespace raylib_display
}  // namespace ui
}  // namespace rex
