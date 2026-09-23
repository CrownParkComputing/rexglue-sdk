// rayview - live debug viewer for rexglue-vmx ports.
//
// Two jobs, both without touching the runtime:
//
//   1. Frame display. The runtime dumps the presented guest frame as PPM when
//      REX_DUMP_FRAME=<prefix> is set (see src/ui/presenter.cpp). rayview
//      watches the dump directory and always shows the newest frame, so a
//      gamescope-headless run becomes watchable live.
//
//   2. Input injection. rayview creates a uinput virtual Xbox 360 pad. The
//      port's SDL input driver sees it as a real controller regardless of
//      window focus - no xdotool focus juggling. Keys pressed in the rayview
//      window, or a real pad read through raylib, are forwarded to it.
//
//   rayview <dump-dir>
//
// Keys: arrows/WASD left stick, IJKL right stick, Enter Start, Space A,
// LeftShift B, Tab Back, Q/R LT/RT, F/G LB/RB. A connected real pad is
// forwarded 1:1. Escape quits.
//
// NOTE: the runtime's PPM dump writes the first three bytes of each BGRA
// pixel, so red and blue arrive swapped. Cosmetic only; left as-is.

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Virtual Xbox 360 pad
// ---------------------------------------------------------------------------

struct VirtPad {
  int fd = -1;

  bool Create() {
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
      std::fprintf(stderr, "rayview: cannot open /dev/uinput: %s\n",
                   strerror(errno));
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
    snprintf(setup.name, sizeof(setup.name), "Microsoft X-Box 360 pad");
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x045e;
    setup.id.product = 0x028e;
    setup.id.version = 0x0114;
    ioctl(fd, UI_DEV_SETUP, &setup);
    // Axis ranges: sticks +/-32768, triggers 0..255, hats -1..1.
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
      std::fprintf(stderr, "rayview: UI_DEV_CREATE: %s\n", strerror(errno));
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

// ---------------------------------------------------------------------------
// PPM tail
// ---------------------------------------------------------------------------

struct FrameTail {
  std::string dir;
  std::string newest_name;
  fs::file_time_type newest_mtime{};
  Texture2D tex{};
  bool has_tex = false;
  uint64_t loads = 0;

  // P6 PPM -> raylib Image (manual parse; no format guesses).
  static bool LoadPpm(const std::string& path, Image* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[3] = {};
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0 ||
        fscanf(f, "%d %d", &w, &h) != 2 || fscanf(f, "%d", &maxv) != 1 ||
        w <= 0 || h <= 0 || w > 8192 || h > 8192) {
      fclose(f);
      return false;
    }
    fgetc(f);  // single whitespace after maxval
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    // A dump can race us (writer mid-write): a short read just means "retry
    // next frame".
    if (fread(rgb.data(), 1, rgb.size(), f) != rgb.size()) {
      fclose(f);
      return false;
    }
    fclose(f);
    // malloc'd: raylib Image data ownership is free()-based.
    uint8_t* rgba = static_cast<uint8_t*>(malloc(size_t(w) * h * 4));
    if (!rgba) return false;
    for (size_t i = 0; i < size_t(w) * h; ++i) {
      rgba[i * 4 + 0] = rgb[i * 3 + 0];
      rgba[i * 4 + 1] = rgb[i * 3 + 1];
      rgba[i * 4 + 2] = rgb[i * 3 + 2];
      rgba[i * 4 + 3] = 255;
    }
    out->data = rgba;
    out->width = w;
    out->height = h;
    out->format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    out->mipmaps = 1;
    return true;  // rgba leaked into out->data ownership
  }

  void Poll() {
    namespace fs = std::filesystem;
    std::string best;
    fs::file_time_type best_t{};
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (e.path().extension() != ".ppm") continue;
      auto t = e.last_write_time(ec);
      if (ec) continue;
      if (best.empty() || t > best_t ||
          (t == best_t && e.path().filename() > best)) {
        best = e.path().string();
        best_t = t;
      }
    }
    if (best.empty() || (best == newest_name && best_t == newest_mtime)) return;
    Image img{};
    if (!LoadPpm(best, &img)) return;
    Texture2D next = LoadTextureFromImage(img);
    // LoadPpm hands ownership of pixels to img; texture upload copies them.
    free(img.data);
    if (next.id == 0) return;
    if (has_tex) UnloadTexture(tex);
    tex = next;
    has_tex = true;
    newest_name = best;
    newest_mtime = best_t;
    ++loads;
  }
};

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: rayview <dump-dir> [--press-test]\n");
    return 1;
  }
  FrameTail tail{argv[1]};
  // --press-test: 2s in, hold Start for 500ms. Proves the input chain
  // (rayview -> uinput -> evdev -> SDL -> guest XamInputGetState) without a
  // person at the keyboard.
  const bool press_test = argc > 2 && std::string(argv[2]) == "--press-test";

  VirtPad pad;
  const bool pad_ok = pad.Create();
  if (!pad_ok) {
    std::fprintf(stderr, "rayview: virtual pad unavailable; display-only.\n");
  }
  // uinput devices appear asynchronously; give SDL in the game a moment.
  usleep(300 * 1000);

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(1280, 760, "rayview");
  SetTargetFPS(60);

  // Logical pad state gathered each frame from keyboard + real pad.
  struct State {
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint8_t lt = 0, rt = 0;
    int8_t hatx = 0, haty = 0;
    bool a, b, x, y, lb, rb, back, start, l3, r3;
  };

  auto axis16 = [](float v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767);
  };

  while (!WindowShouldClose()) {
    tail.Poll();

    // Guest presented FPS: the dump filename carries the presentation
    // counter, so d(counter)/dt is the guest's own frame rate regardless of
    // REX_DUMP_FRAME_EVERY.
    static double fps_window_start = 0.0;
    static long fps_window_first_n = -1;
    static float guest_fps = 0.0f;
    if (tail.has_tex) {
      long n = -1;
      const size_t us = tail.newest_name.rfind('_');
      if (us != std::string::npos) n = atol(tail.newest_name.c_str() + us + 1);
      const double now = GetTime();
      if (n >= 0 && fps_window_first_n < 0) {
        fps_window_first_n = n;
        fps_window_start = now;
      } else if (n >= 0 && now - fps_window_start >= 1.0) {
        guest_fps =
            float(n - fps_window_first_n) / float(now - fps_window_start);
        fps_window_first_n = n;
        fps_window_start = now;
      }
    }

    State s{};
    // Keyboard.
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
    if (IsKeyDown(KEY_ENTER)) s.start = true;
    if (press_test) {
      const double t = GetTime();
      if (t >= 2.0 && t <= 2.5) s.start = true;
    }
    s.lt = IsKeyDown(KEY_Q) ? 255 : 0;
    s.rt = IsKeyDown(KEY_R) ? 255 : 0;
    // D-pad on T/G/H... keep hats separate: U/O left/right, P/semicolon up/dn.
    if (IsKeyDown(KEY_U)) s.hatx = -1;
    if (IsKeyDown(KEY_O)) s.hatx = 1;
    if (IsKeyDown(KEY_P)) s.haty = -1;
    if (IsKeyDown(KEY_SEMICOLON)) s.haty = 1;

    // A real pad forwarded 1:1 wins over the keyboard.
    if (IsGamepadAvailable(0)) {
      s.lx = axis16(GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X));
      s.ly = axis16(GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y));
      s.rx = axis16(GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_X));
      s.ry = axis16(GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_Y));
      s.lt = (uint8_t)(std::clamp(
                  GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_TRIGGER), 0.0f,
                  1.0f) *
              255);
      s.rt = (uint8_t)(std::clamp(
                  GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_TRIGGER), 0.0f,
                  1.0f) *
              255);
      s.a = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
      s.b = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
      s.x = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
      s.y = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_UP);
      s.lb = IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_TRIGGER_1);
      s.rb = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_TRIGGER_1);
      s.back = IsGamepadButtonDown(0, GAMEPAD_BUTTON_MIDDLE_LEFT);
      s.start = IsGamepadButtonDown(0, GAMEPAD_BUTTON_MIDDLE_RIGHT);
      s.l3 = IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_THUMB);
      s.r3 = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_THUMB);
      s.hatx = IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT) ? -1
               : IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) ? 1
                                                                        : 0;
      s.haty = IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_UP) ? -1
               : IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN) ? 1
                                                                       : 0;
    }

    if (press_test) {
      // After the real-pad block so an attached pad cannot mask it. Pulses
      // Start for 300ms every 2s so a watcher can attach at any time.
      const double t = GetTime();
      const double phase = t - std::floor(t / 2.0) * 2.0;
      if (phase < 0.3) s.start = true;
    }

    if (pad_ok) {
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

    BeginDrawing();
    ClearBackground(BLACK);
    if (tail.has_tex) {
      const float scale = std::min((float)GetScreenWidth() / tail.tex.width,
                                   (float)(GetScreenHeight() - 40) /
                                       tail.tex.height);
      DrawTextureEx(tail.tex, {0, 40}, 0.0f, scale, WHITE);
    } else {
      DrawText("waiting for frames...", 12, 60, 20, GRAY);
    }
    DrawText(TextFormat("%s  loads=%llu  guest %5.1f fps  view %d fps  pad:%s",
                        tail.newest_name.c_str(),
                        (unsigned long long)tail.loads, guest_fps, GetFPS(),
                        pad_ok ? "virtual x360" : "OFF"),
             8, 8, 20, RAYWHITE);
    DrawText("arrows/WASD stick  IJKL rstick  Enter=Start Space=A LShift=B "
             "Tab=Back  Q/R=LT/RT  F/G=LB/RB",
             8, GetScreenHeight() - 18, 10, GRAY);
    EndDrawing();
  }

  CloseWindow();
  return 0;
}
