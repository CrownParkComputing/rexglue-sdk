// XAM: input (SDL3 gamepad/keyboard), notifications, profile, XMP messages, and
// the offline answers for everything Live/UI.
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/chrono/clock.h>

#include "kernel.h"
#include "profile_store.h"
#include "local_profile.h"
#include "stats_store.h"
#include "input_script.h"
#include "xmp.h"
#include "gfx/command_processor.h"
#include "gfx/frame_recorder.h"

using namespace xn;

// This standalone runtime has no voice-chat process or microphone service.
u32 XamVoiceIsActiveProcess_entry() { return 0; }
XN_IMPORT(__imp__XamVoiceIsActiveProcess, XamVoiceIsActiveProcess_entry)

u32 XamVoiceCreate_entry(u32 process, u32 flags, mapped_u32 voice) {
  if (voice) *voice = 0;
  // No host voice-chat service is registered. A successful creation with an
  // unwritten output leaves Forza using a stale pointer as its voice object.
  // The linked XHV wrapper tests the result as a signed HRESULT (827A49FC).
  return voice ? 0x80070005u : 0x80070057u; // E_ACCESSDENIED / E_INVALIDARG
}
XN_IMPORT(__imp__XamVoiceCreate, XamVoiceCreate_entry)

u32 XamUserGetDeviceContext_entry(u32 user, u32 kind, mapped_u32 output) {
  if (!output) return X_E_INVALIDARG;
  *output = 0;  // The single local user has the default device context.
  return user == 0 || (user & 0xFF) == 0xFF ? 0 : 0x8007048Fu;
}
XN_IMPORT(__imp__XamUserGetDeviceContext, XamUserGetDeviceContext_entry)

u32 XamIsUIActive_entry() { return 0; }
XN_IMPORT(__imp__XamIsUIActive, XamIsUIActive_entry)

void XamGetActiveDashAppInfo_entry(u32 output) {
  // No dashboard app is active. X_DASH_APP_INFO is a 16-byte record; Forza
  // initializes/reads its first QWORD and the DWORD at +8 (0x827BBDD0).
  if (output) std::memset(G(output), 0, 16);
}
XN_IMPORT(__imp__XamGetActiveDashAppInfo, XamGetActiveDashAppInfo_entry)

u32 XamNuiGetDeviceStatus_entry(u32 status) {
  // X_NUI_DEVICE_STATUS is four big-endian DWORDs, including flags at +12.
  // Forza's 0x82759670 reads those flags from a 16-byte stack output. With no
  // Kinect, all fields are zero and the service reports the device unavailable.
  if (!status) return X_E_INVALIDARG;
  std::memset(G(status), 0, 16);
  return 0xC0050006u;
}
XN_IMPORT(__imp__XamNuiGetDeviceStatus, XamNuiGetDeviceStatus_entry)

u32 XamNuiCameraTiltSetCallback_entry(u32 callback) {
  // Registration does not require a connected device. Retain the callback,
  // including a null unregister, without emitting absent-camera events.
  K().nui_tilt_callback.store(callback, std::memory_order_relaxed);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__XamNuiCameraTiltSetCallback, XamNuiCameraTiltSetCallback_entry)

u32 PsCamDeviceRequest_entry(u32 request) {
  // The normal camera path is Forza's fallback when XStudio is unavailable.
  // No console camera is attached; preserve the request's input buffer.
  return 0xC000009Du; // STATUS_DEVICE_NOT_CONNECTED
}
XN_IMPORT(__imp__PsCamDeviceRequest, PsCamDeviceRequest_entry)

u32 XamGetCurrentTitleId_entry() { return K().image.title_id; }
XN_IMPORT(__imp__XamGetCurrentTitleId, XamGetCurrentTitleId_entry)

namespace {

// An online XUID has the 0x0009 prefix the console uses for Live accounts; an
// offline one is 0xE0-prefixed. Which one we hand out follows the sign-in
// state we claim, because titles check the shape.
constexpr uint64_t kOnlineXuid = 0x0009041CBAADF00Dull;
constexpr uint32_t kXNotifySysSignInChanged = 0x0000000A;

// XN_LIVE=1 makes user 0 look signed in to Xbox LIVE instead of only locally.
// Off by default: a title that believes it is online takes code paths whose
// network calls this runtime answers with failure. See docs (W6 experiment).
bool LiveSignIn() {
  static const bool on = [] {
    const char* v = std::getenv("XN_LIVE");
    return v && v[0] == '1';
  }();
  return on;
}
uint64_t CurrentXuid() { return LiveSignIn() ? kOnlineXuid : LocalUser().xuid(); }

// Hex dump of a guest buffer, for the XAM message log.
std::string HexDump(uint32_t ptr, uint32_t len) {
  std::string out;
  char tmp[8];
  for (uint32_t i = 0; i < len && i < 64; i++) {
    std::snprintf(tmp, sizeof tmp, "%02X", Load8(ptr + i));
    out += tmp;
    if ((i & 3) == 3) out += ' ';
  }
  return out;
}

// XINPUT button bits.
constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
                   kStart = 0x0010, kBack = 0x0020, kLThumb = 0x0040, kRThumb = 0x0080,
                   kLShoulder = 0x0100, kRShoulder = 0x0200, kA = 0x1000, kB = 0x2000,
                   kX = 0x4000, kY = 0x8000;

struct PadState {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

std::mutex g_input_mutex;
PadState g_pad;              // merged gamepad + keyboard, refreshed by PumpInput()
uint32_t g_packet = 0;
bool g_pad_present = false;
// XamInputSetState runs on a guest thread; SDL is driven from the host main
// thread, so the requested rumble is parked here and applied in PumpInput.
std::atomic<uint32_t> g_rumble{0};   // left << 16 | right
SDL_Gamepad* g_gamepad = nullptr;

// Scripted input (--script): evaluated against the launch clock in PumpInput.
InputScript g_script;
std::chrono::steady_clock::time_point g_script_t0{};
std::atomic<bool> g_host_input_captured{false};
std::atomic<uint64_t> g_host_input_release_ns{0}, g_host_start_until_ns{0};
std::atomic<void (*)()> g_guest_input_poll_callback{nullptr};

}  // namespace

namespace xn {

void SetGuestInputPollCallback(void (*callback)()) {
  g_guest_input_poll_callback.store(callback, std::memory_order_release);
}

void SetHostInputCaptured(bool captured) {
  if (!captured && g_host_input_captured.load(std::memory_order_relaxed))
    g_host_input_release_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()+200000000;
  g_host_input_captured=captured;
}
void SendHostStartButton() {
  g_host_start_until_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count()+150000000;
}
void SetInputScript(const InputScript& script) {
  g_script = script;
  g_script_t0 = std::chrono::steady_clock::now();
}

// Called from the host main thread (SDL event owner) once per frame.
void PumpInput() {
  PadState s;
  bool present = false;
  SDL_Gamepad*& pad = g_gamepad;
  if (!pad) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids && count > 0) pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
  }
  if (pad && SDL_GamepadConnected(pad)) {
    present = true;
    auto btn = [&](SDL_GamepadButton b, uint16_t bit) { if (SDL_GetGamepadButton(pad, b)) s.buttons |= bit; };
    btn(SDL_GAMEPAD_BUTTON_SOUTH, kA); btn(SDL_GAMEPAD_BUTTON_EAST, kB);
    btn(SDL_GAMEPAD_BUTTON_WEST, kX); btn(SDL_GAMEPAD_BUTTON_NORTH, kY);
    btn(SDL_GAMEPAD_BUTTON_START, kStart); btn(SDL_GAMEPAD_BUTTON_BACK, kBack);
    btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kLShoulder); btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kRShoulder);
    btn(SDL_GAMEPAD_BUTTON_LEFT_STICK, kLThumb); btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK, kRThumb);
    btn(SDL_GAMEPAD_BUTTON_DPAD_UP, kDpadUp); btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDpadDown);
    btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT, kDpadLeft); btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kDpadRight);
    s.lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    s.ly = int16_t(-SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) - 1);
    s.rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    s.ry = int16_t(-SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) - 1);
    s.lt = uint8_t(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    s.rt = uint8_t(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    // Whatever XamInputSetState last asked for. Re-sent every pump with a
    // duration that outlives one frame, so a held effect does not stutter.
    uint32_t r = g_rumble.load(std::memory_order_relaxed);
    SDL_RumbleGamepad(pad, uint16_t(r >> 16), uint16_t(r & 0xFFFF), 250);
  } else if (pad) {
    SDL_CloseGamepad(pad);
    pad = nullptr;
  }
  // Keyboard: WASD = left stick, arrows = right stick, space = RT (bomb),
  // enter = Start, esc = Back, Z/X/C/V = A/B/X/Y.
  const bool* keys = SDL_GetKeyboardState(nullptr);
  if (keys) {
    auto axis = [](bool neg, bool pos) -> int16_t { return neg ? -32767 : pos ? 32767 : 0; };
    if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_D]) s.lx = axis(keys[SDL_SCANCODE_A], keys[SDL_SCANCODE_D]);
    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_S]) s.ly = axis(keys[SDL_SCANCODE_S], keys[SDL_SCANCODE_W]);
    if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_RIGHT]) s.rx = axis(keys[SDL_SCANCODE_LEFT], keys[SDL_SCANCODE_RIGHT]);
    if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_DOWN]) s.ry = axis(keys[SDL_SCANCODE_DOWN], keys[SDL_SCANCODE_UP]);
    if (keys[SDL_SCANCODE_SPACE]) s.rt = 255;
    // Alt+Enter belongs to the host fullscreen toggle, not the game's Start.
    if (keys[SDL_SCANCODE_RETURN] && !keys[SDL_SCANCODE_LALT] && !keys[SDL_SCANCODE_RALT])
      s.buttons |= kStart;
    if (keys[SDL_SCANCODE_BACKSPACE]) s.buttons |= kBack;
    if (keys[SDL_SCANCODE_Z]) s.buttons |= kA;
    if (keys[SDL_SCANCODE_X]) s.buttons |= kB;
    if (keys[SDL_SCANCODE_C]) s.buttons |= kX;
    if (keys[SDL_SCANCODE_V]) s.buttons |= kY;
    present = true;  // the keyboard always counts as pad 0
  }
  if (!g_script.empty()) {
    // Controlled performance replays may opt out of physical input so a
    // connected controller cannot change the measured camera or route.
    static const bool script_only = std::getenv("XN_SCRIPT_ONLY") != nullptr;
    if (script_only) s = {};
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_script_t0).count();
    std::vector<const InputStep*> pressed;
    ScriptedPad sp = g_script.Evaluate(t, &pressed);
    for (const InputStep* step : pressed) {
      uint64_t frame = K().gpu ? K().gpu->swaps() : 0;
      REXLOG_INFO("script: t={:.3f} press {} hold={:.2f} frame={}", t, step->name, step->hold, frame);
      FrameRecorderNoteEvent(step->name, t, frame);
    }
    present = true;  // a scripted pad is always connected
    if (sp.active) {
      s.buttons |= sp.buttons;
      if (sp.lt) s.lt = sp.lt;
      if (sp.rt) s.rt = sp.rt;
      if (sp.lx) s.lx = sp.lx;
      if (sp.ly) s.ly = sp.ly;
      if (sp.rx) s.rx = sp.rx;
      if (sp.ry) s.ry = sp.ry;
    }
  }
  const uint64_t host_now=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  if (g_host_input_captured.load(std::memory_order_relaxed) ||
      host_now<g_host_input_release_ns.load(std::memory_order_relaxed)) s={};
  if (host_now<g_host_start_until_ns.load(std::memory_order_relaxed)) s.buttons|=kStart;
  std::lock_guard lock(g_input_mutex);
  if (std::memcmp(&s, &g_pad, sizeof s) != 0) g_packet++;
  g_pad = s;
  g_pad_present = present;
}

}  // namespace xn

u32 XamInputGetState_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_STATE> state) {
  if (auto callback = g_guest_input_poll_callback.load(std::memory_order_acquire)) callback();
  // Every early return zeroes the state first, as the console does: a caller
  // that ignores the status - and plenty do for pad 0 - must not read stale
  // pad data as live input.
  auto not_connected = [&]() -> uint32_t {
    if (state) std::memset(state.host_address(), 0, sizeof(X_INPUT_STATE));
    return X_ERROR_DEVICE_NOT_CONNECTED;
  };
  if ((flags & 0xFF) && !(flags & 0x01)) return not_connected();
  uint32_t user = ((user_index & 0xFF) == 0xFF || (flags & 0x40000000)) ? 0 : user_index;
  if (user != 0) return not_connected();
  std::lock_guard lock(g_input_mutex);
  if (!g_pad_present) return not_connected();
  if (state) {
    state->packet_number = g_packet;
    state->gamepad.buttons = g_pad.buttons;
    state->gamepad.left_trigger = g_pad.lt;
    state->gamepad.right_trigger = g_pad.rt;
    state->gamepad.thumb_lx = g_pad.lx;
    state->gamepad.thumb_ly = g_pad.ly;
    state->gamepad.thumb_rx = g_pad.rx;
    state->gamepad.thumb_ry = g_pad.ry;
  }
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamInputGetState, XamInputGetState_entry)

// X_INPUT_CAPABILITIES { u8 type; u8 sub_type; be16 flags; X_INPUT_GAMEPAD
// gamepad; X_INPUT_VIBRATION vibration; } - the gamepad/vibration members are
// masks of what the device can report, not live state, so a wired pad answers
// "everything" the way the console does.
u32 XamInputGetCapabilities_entry(u32 user_index, u32 flags, u32 caps_ptr) {
  if ((flags & 0xFF) && !(flags & 0x01)) return X_ERROR_DEVICE_NOT_CONNECTED;
  uint32_t user = ((user_index & 0xFF) == 0xFF || (flags & 0x40000000)) ? 0 : user_index;
  if (user != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
  {
    std::lock_guard lock(g_input_mutex);
    if (!g_pad_present) return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!caps_ptr) return X_ERROR_INVALID_PARAMETER;
  std::memset(G(caps_ptr), 0, 0x14);
  Store8(caps_ptr + 0, 1);       // XINPUT_DEVTYPE_GAMEPAD
  Store8(caps_ptr + 1, 1);       // XINPUT_DEVSUBTYPE_GAMEPAD
  Store16(caps_ptr + 2, 0);      // no XINPUT_CAPS_VOICE_SUPPORTED
  Store16(caps_ptr + 4, 0xF3FF); // buttons the pad can report
  Store8(caps_ptr + 6, 0xFF);    // left trigger resolution
  Store8(caps_ptr + 7, 0xFF);    // right trigger resolution
  Store16(caps_ptr + 8, 0xFFC0);  Store16(caps_ptr + 10, 0xFFC0);  // thumb resolutions
  Store16(caps_ptr + 12, 0xFFC0); Store16(caps_ptr + 14, 0xFFC0);
  Store16(caps_ptr + 16, 0xFFFF); Store16(caps_ptr + 18, 0xFFFF);  // both motors
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamInputGetCapabilities, XamInputGetCapabilities_entry)

u32 XamInputGetCapabilitiesEx_entry(u32 unused, u32 user, u32 flags, u32 capabilities) {
  return XamInputGetCapabilities_entry(user, flags, capabilities);
}
XN_IMPORT(__imp__XamInputGetCapabilitiesEx, XamInputGetCapabilitiesEx_entry)

// X_INPUT_VIBRATION { be16 left_motor; be16 right_motor; } - 0..65535 each,
// driven onto the real pad when SDL has one.
u32 XamInputSetState_entry(u32 user_index, u32 flags, u32 vibration_ptr) {
  uint32_t user = ((user_index & 0xFF) == 0xFF) ? 0 : user_index;
  if (user != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (!vibration_ptr) return X_ERROR_INVALID_PARAMETER;
  uint16_t left = Load16(vibration_ptr + 0), right = Load16(vibration_ptr + 2);
  g_rumble.store((uint32_t(left) << 16) | right, std::memory_order_relaxed);
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamInputSetState, XamInputSetState_entry)

u32 XamInputGetKeystrokeEx_entry(mapped_u32 user, u32 flags, u32 keystroke) {
  if (!user || !keystroke) return X_ERROR_INVALID_PARAMETER;
  std::memset(G(keystroke), 0, 8);
  if ((flags & 0xFF) && !(flags & 1)) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (*user != 0 && (*user & 0xFF) != 0xFF && !(flags & 0x40000000))
    return X_ERROR_DEVICE_NOT_CONNECTED;
  // Input currently comes from XamInputGetState, with no buffered keystrokes.
  // Success here would claim an event and leave the SDK's drain loop spinning.
  return 4306; // ERROR_EMPTY
}
XN_IMPORT(__imp__XamInputGetKeystrokeEx, XamInputGetKeystrokeEx_entry)

u32 XamNotifyCreateListener_entry(u64 mask, u32 max_version) {
  auto listener = std::make_shared<NotifyListener>(mask ? mask : ~0ull);
  uint32_t handle = K().objects.Add(listener);
  // A console that just booted with a signed-in profile reports the sign-in.
  listener->Enqueue(kXNotifySysSignInChanged, 1);
  REXKRNL_INFO("XamNotifyCreateListener mask={:#x} -> {:#x}", mask, handle);
  return handle;
}
XN_IMPORT(__imp__XamNotifyCreateListener, XamNotifyCreateListener_entry)
XN_IMPORT(__imp__XNotifyCreateListener, XamNotifyCreateListener_entry)

u32 XNotifyGetNext_entry(u32 handle, u32 match_id, mapped_u32 id_ptr, mapped_u32 param_ptr) {
  if (param_ptr) *param_ptr = 0;
  if (!id_ptr) return 0;
  *id_ptr = 0;
  auto listener = K().objects.Lookup<NotifyListener>(handle);
  if (!listener) return 0;
  uint32_t id = 0, param = 0;
  if (!listener->Dequeue(match_id, &id, &param)) return 0;
  *id_ptr = id;
  if (param_ptr) *param_ptr = param;
  REXKRNL_DEBUG("XNotifyGetNext -> id={:#x} param={:#x}", id, param);
  return 1;
}
XN_IMPORT(__imp__XNotifyGetNext, XNotifyGetNext_entry)

u32 XamUserGetSigninState_entry(u32 user_index) {
  // 0 = not signed in, 1 = signed in locally, 2 = signed in to Xbox LIVE.
  if (user_index != 0 && (user_index & 0xFF) != 0xFF) return 0;
  return LiveSignIn() ? 2 : 1;
}
XN_IMPORT(__imp__XamUserGetSigninState, XamUserGetSigninState_entry)

u32 XamUserGetSigninInfo_entry(u32 user, u32 flags, u32 info) {
  if (!info) return X_E_INVALIDARG;
  std::memset(G(info), 0, 40);
  if (user != 0) return X_E_NO_SUCH_USER;
  Store64(info, CurrentXuid());
  Store32(info + 12, LiveSignIn() ? 2 : 1);
  const auto& name = LocalUser().gamertag();
  std::memcpy(G(info + 24), name.data(), std::min<size_t>(15, name.size()));
  return 0;
}
XN_IMPORT(__imp__XamUserGetSigninInfo, XamUserGetSigninInfo_entry)

u32 XamUserGetXUID_entry(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr) {
  if (!xuid_ptr) return X_E_INVALIDARG;
  if (user_index != 0) { *xuid_ptr = 0; return X_E_NO_SUCH_USER; }
  *xuid_ptr = CurrentXuid();
  return 0;
}
XN_IMPORT(__imp__XamUserGetXUID, XamUserGetXUID_entry)

// Profile settings back the local high score (ProfileStore, one file per
// title). Settings never written read back as "not set" (from = 0).
// XUSER_PROFILE_SETTING (40 bytes): +0 from, +8 user, +0x10 setting_id,
// +0x18 XUSER_DATA { +0 type u8; +8 union: int32/float/int64/double/datetime,
// or { +8 cbData, +0xC pbData } for wstring/binary }.
u32 XamUserReadProfileSettings_entry(u32 title_id, u32 user_index, u32 xuid_count, u32 xuids,
                                     u32 setting_count, mapped_u32 setting_ids,
                                     mapped_u32 buffer_size_ptr, u32 buffer, u32 overlapped) {
  if (setting_count < 1 || setting_count > 32 || !buffer_size_ptr) return X_ERROR_INVALID_PARAMETER;
  // A title id of 0 means "the running title" (the emulator always keys on the
  // running title id), so normalise it or a later read with the real id misses.
  if (title_id == 0) title_id = K().image.title_id;
  uint32_t needed = 8 + setting_count * 40;
  for (uint32_t i = 0; i < setting_count; i++) {
    uint32_t key = setting_ids[i];
    uint32_t type = (key >> 28) & 0xF;
    if (type == 4 /*WSTRING*/ || type == 6 /*BINARY*/) needed += (key >> 16) & 0xFFF;  // data size
  }
  uint32_t size = *buffer_size_ptr;
  if (!buffer || size < needed) {
    *buffer_size_ptr = needed;
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  // Only user 0 exists (XamUserGetSigninState/XamUserGetXUID agree), and the
  // emulator answers a by-index read for any other user this way.
  if (!(xuid_count && xuids) && user_index != 0) {
    if (overlapped) { CompleteOverlapped(overlapped, X_E_NO_SUCH_USER); return X_ERROR_IO_PENDING; }
    return X_E_NO_SUCH_USER;
  }
  std::memset(G(buffer), 0, size);
  Store32(buffer + 0, setting_count);
  Store32(buffer + 4, buffer + 8);
  uint32_t data_cursor = buffer + 8 + setting_count * 40;
  uint32_t found = 0;
  for (uint32_t i = 0; i < setting_count; i++) {
    uint32_t s = buffer + 8 + i * 40;
    uint32_t id = setting_ids[i];
    // The field at +0x08 is a union: the XUID when the caller read by XUID
    // (XUserReadProfileSettingsByXuid), the user index otherwise.
    if (xuid_count && xuids) Store64(s + 0x08, CurrentXuid()); else Store32(s + 0x08, user_index);
    Store32(s + 0x10, id);
    bool title_specific = IsTitleSpecificSetting(id);
    ProfileSetting ps;
    // Title-specific settings live under the title; console-wide gamer
    // settings belong to the profile (key 0) and fall back to the values a
    // signed-in console always has.
    bool set = Profile().Get(title_specific ? title_id : 0, id, ps);
    if (!set && !title_specific) set = DefaultProfileSetting(id, ps);
    if (!set) {
      Store32(s + 0x00, 0);  // from: not set
      continue;
    }
    found++;
    Store32(s + 0x00, title_specific ? 2 : 1);  // from: title / profile
    Store8(s + 0x18, ps.type);
    if (ps.type == 4 || ps.type == 6) {
      uint32_t cap = (id >> 16) & 0xFFF;
      uint32_t len = std::min<uint32_t>(cap, uint32_t(ps.blob.size()));
      // An empty string/blob reads back as cbData 0, pbData 0.
      if (len && data_cursor + len <= buffer + size) {
        std::memcpy(G(data_cursor), ps.blob.data(), len);
        Store32(s + 0x20, len);
        Store32(s + 0x24, data_cursor);
        data_cursor += cap;
      }
    } else if (ps.type == 1 || ps.type == 5) {
      Store32(s + 0x20, uint32_t(ps.scalar));
    } else {
      Store64(s + 0x20, ps.scalar);
    }
  }
  {
    char ids[32 * 10] = {0};
    size_t n = 0;
    for (uint32_t i = 0; i < setting_count && n + 10 < sizeof(ids); i++)
      n += size_t(std::snprintf(ids + n, sizeof(ids) - n, "%s%08X", i ? "," : "",
                                uint32_t(setting_ids[i])));
    REXKRNL_INFO("XamUserReadProfileSettings title={:08X} user={} ids=[{}] found={}/{}", title_id,
                 user_index, ids, found, setting_count);
  }
  if (overlapped) {
    CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamUserReadProfileSettings, XamUserReadProfileSettings_entry)

u32 XamUserWriteProfileSettings_entry(u32 title_id, u32 user_index, u32 setting_count, u32 settings,
                                      u32 overlapped) {
  if (setting_count < 1 || setting_count > 32 || !settings) return X_ERROR_INVALID_PARAMETER;
  if (title_id == 0) title_id = K().image.title_id;
  for (uint32_t i = 0; i < setting_count; i++) {
    uint32_t s = settings + i * 40;
    uint32_t id = Load32(s + 0x10);
    ProfileSetting ps;
    ps.type = Load8(s + 0x18);
    if (ps.type == 4 || ps.type == 6) {
      uint32_t len = Load32(s + 0x20), ptr = Load32(s + 0x24);
      if (ptr && len) ps.blob.assign(G(ptr), G(ptr) + len);
    } else if (ps.type == 1 || ps.type == 5) {
      ps.scalar = Load32(s + 0x20);
    } else {
      ps.scalar = (uint64_t(Load32(s + 0x20)) << 32) | Load32(s + 0x24);
    }
    uint32_t key_title = IsTitleSpecificSetting(id) ? title_id : 0;
    Profile().Put(key_title, id, ps);
    REXKRNL_INFO("XamUserWriteProfileSettings {} {:08X} id={:08X} type={} bytes={} scalar={:#x}",
                 key_title ? "title" : "profile", key_title ? key_title : title_id, id, ps.type,
                 ps.blob.size(), ps.scalar);
  }
  if (overlapped) { CompleteOverlapped(overlapped, X_ERROR_SUCCESS); return X_ERROR_IO_PENDING; }
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamUserWriteProfileSettings, XamUserWriteProfileSettings_entry)

// XAM app messages. A title can send the same message two ways - asynchronously
// through XMsgStartIORequest with an overlapped, or synchronously through
// XMsgInProcessCall - and both must answer identically, so the answer lives in
// one place. Geometry Wars 1 uses the async form for the media player;
// Geometry Wars 2 uses the in-process form for the same message.
//
// Answering a message means *writing its output buffer*. Returning a status and
// leaving the buffer alone is the worst of both worlds: Geometry Wars 2's
// wrapper (0x820E8F78) treats any non-negative status as success, so with
// X_ERROR_NOT_FOUND it read whatever was on its stack as the playback
// controller. That produced a 1 GB NtAllocateVirtualMemory and a fatal
// RtlRaiseException a few milliseconds later.
namespace {

constexpr uint32_t kXamAppXmp = 0xFA;              // the media player
constexpr uint32_t kXamAppXgi = 0xFB;              // gaming services: contexts, sessions, stats
constexpr uint32_t kXgiUserSetContextEx = 0x000B0006;
constexpr uint32_t kXgiUserReadStats = 0x000B0021;

// XUserReadStats result layout, sizes taken from the title's own XDK wrapper
// (0x82029318), which computes
//   8 + specs * (16 + xuids * 52) + sum(spec.columns * xuids * 28)
// before it sends the message:
//   X_USER_STATS_READ_RESULTS { u32 num_views; u32 views_ptr; }          8
//   X_USER_STATS_VIEW         { u32 view_id, total_rows, num_rows, rows } 16
//   X_USER_STATS_ROW          { u64 xuid; u32 rank; u32 pad; u64 rating;
//                               char gamertag[16]; u32 columns; u32 cols_ptr;
//                               u32 pad; }                                52
// Geometry Wars asks for a view with zero columns, so the score is carried by
// the row's rating and nothing else is needed. The layout is confirmed by the
// title itself: with a seeded row the Evolved Leaderboard screen renders
// "1. Player  500" from exactly these offsets.
//
// Measured, so nobody has to repeat the runs: the score the HUD shows as
// HIGHSCORE is *not* read from here. The title only sends this message while
// its Leaderboards screen is open - not at boot, not at game start, not at
// game over - so a stored row changes the leaderboard screen and nothing
// else. The 10,000 on the HUD is the title's own constant.
constexpr uint32_t kStatsSpecSize = 136;  // { u32 view_id; u32 columns; u16 ids[64]; }
constexpr uint32_t kStatsViewSize = 16;
constexpr uint32_t kStatsRowSize = 52;

// XGI messages. Every branch that answers must fill the buffer it was given:
// a status with an untouched output buffer leaves the title reading its own
// stack (that is how Geometry Wars 2's media-player poll produced a 1 GB
// allocation), so an answer we cannot write is not an answer.
// Returns false when we do not know the message, leaving it to the caller; a
// message we do know always leaves with both its buffer and its status set,
// and says so with a real error rather than a hollow success when it cannot
// fill the buffer.
bool AnswerXgiMessage(uint32_t message, uint32_t buffer, uint32_t buffer_length,
                      uint32_t* status) {
  switch (message) {
    case 0x000B0008: { // XUserWriteAchievements: { count, {user_index,id}* }
      *status = 0x80070057u;
      if (!buffer || (buffer_length && buffer_length < 8)) return true;
      uint32_t count = Load32(buffer), entries = Load32(buffer + 4);
      if (count > 1000 || (count && (!entries || uint64_t(entries) + count * 8ull > 0x100000000ull))) return true;
      for (uint32_t i = 0; i < count; ++i) {
        if (Load32(entries + i * 8) != 0) return true;
      }
      *status = X_ERROR_SUCCESS;
      for (uint32_t i = 0; i < count; ++i) {
        if (!LocalUser().Unlock(K().image.title_id, Load32(entries + i * 8 + 4), rex::chrono::Clock::QueryGuestSystemTime()))
          *status = 0x80004005u;
      }
      return true;
    }
    case kXgiUserSetContextEx: {
      // { u32 user; u32 unk; u64 unk; u32 context_id; u32 value; } - all input,
      // there is no output field to write. Presence/game type/game mode.
      if (buffer && buffer_length >= 24) {
        REXKRNL_DEBUG("XGI SetContext user={} context={:#x} value={}", Load32(buffer + 0),
                      Load32(buffer + 16), Load32(buffer + 20));
      }
      *status = X_ERROR_SUCCESS;
      return true;
    }
    case kXgiUserReadStats: {
      // { u32 title_id; u32 num_xuids; u32 xuids_ptr; u32 num_specs;
      //   u32 specs_ptr; u32 results_size; u32 results_ptr; }
      if (!buffer || buffer_length < 28) return false;
      uint32_t title_id = Load32(buffer + 0);
      uint32_t num_xuids = Load32(buffer + 4);
      uint32_t xuids_ptr = Load32(buffer + 8);
      uint32_t num_specs = Load32(buffer + 12);
      uint32_t specs_ptr = Load32(buffer + 16);
      uint32_t results_size = Load32(buffer + 20);
      uint32_t results = Load32(buffer + 24);
      if (!results || !specs_ptr || num_specs > 8 || num_xuids > 4) {
        REXKRNL_WARN("XUserReadStats: implausible request specs={} xuids={} results={:#x}",
                     num_specs, num_xuids, results);
        *status = X_ERROR_INVALID_PARAMETER;
        return true;
      }
      if (title_id == 0) title_id = K().image.title_id;
      uint32_t need = 8 + num_specs * (kStatsViewSize + num_xuids * kStatsRowSize);
      if (results_size < need) {
        // Say so instead of returning success over a buffer we did not write:
        // the title's own wrapper raises the same error when it sizes the
        // request itself, so this is an answer it already knows how to take.
        REXKRNL_WARN("XUserReadStats: buffer {} < {} needed", results_size, need);
        *status = X_ERROR_INSUFFICIENT_BUFFER;
        return true;
      }
      std::memset(G(results), 0, results_size);
      uint32_t views = results + 8;
      Store32(results + 0, num_specs);
      Store32(results + 4, views);
      uint32_t row_cursor = views + num_specs * kStatsViewSize;
      for (uint32_t i = 0; i < num_specs; i++) {
        uint32_t spec = specs_ptr + i * kStatsSpecSize;
        uint32_t view_id = Load32(spec + 0);
        uint32_t columns = Load32(spec + 4);
        uint32_t view = views + i * kStatsViewSize;
        uint64_t rating = 0;
        bool ranked = Stats().Get(title_id, view_id, rating);
        // A column the local store cannot fill would have to be written as
        // something, and inventing leaderboard columns is worse than saying
        // the user has no row. Only the bare (rank, rating) shape is answered.
        uint32_t rows = (ranked && columns == 0 && num_xuids) ? 1 : 0;
        Store32(view + 0, view_id);
        Store32(view + 4, rows);   // total rows on the board
        Store32(view + 8, rows);   // rows returned
        Store32(view + 12, rows ? row_cursor : 0);
        if (rows) {
          uint64_t xuid = xuids_ptr ? Load64(xuids_ptr) : CurrentXuid();
          Store64(row_cursor + 0, xuid);
          Store32(row_cursor + 8, 1);          // rank
          Store64(row_cursor + 16, rating);    // the score
          std::memset(G(row_cursor + 24), 0, 16);
          const auto name=LocalUser().gamertag();
          std::memcpy(G(row_cursor + 24),name.data(),std::min(size_t(15),name.size()));
          Store32(row_cursor + 40, 0);         // no columns
          Store32(row_cursor + 44, 0);
        }
        row_cursor += num_xuids * kStatsRowSize;
        REXKRNL_INFO("XUserReadStats title={:08X} view={:#x} columns={} -> {} row(s) rating={}",
                     title_id, view_id, columns, rows, rating);
      }
      *status = X_ERROR_SUCCESS;
      return true;
    }
    // Session/stats calls we do not answer yet, logged at INFO so a title's
    // leaderboard traffic is visible on normal runs. Returning false leaves
    // them on the unhandled path (NoteUnhandled + buffer zero) exactly as
    // before - this is a probe, not an implementation.
    case 0x000B0010: {  // XGISessionCreateImpl
      if (buffer && buffer_length >= 28) {
        REXKRNL_DEBUG("[stats-probe] XSessionCreate flags={:08X} slots={}+{} xuid={:08X} "
                     "session={:08X} info={:08X} nonce={:08X}",
                     Load32(buffer + 4), Load32(buffer + 8), Load32(buffer + 12),
                     Load32(buffer + 16), Load32(buffer + 0), Load32(buffer + 20),
                     Load32(buffer + 24));
      }
      return false;
    }
    case 0x000B0011: {  // XGISessionDelete
      if (buffer && buffer_length >= 16) {
        REXKRNL_DEBUG("[stats-probe] XSessionDelete obj={:08X} flags={:08X} nonce={:016X}",
                     Load32(buffer + 0), Load32(buffer + 4), Load64(buffer + 8));
      }
      return false;
    }
    case 0x000B0012: {  // XGISessionJoinLocal / JoinRemote
      if (buffer && buffer_length >= 20) {
        REXKRNL_DEBUG("[stats-probe] XSessionJoin{} obj={:08X} users={} xuids={:08X}",
                     Load32(buffer + 8) ? "Remote" : "Local", Load32(buffer + 0),
                     Load32(buffer + 4), Load32(buffer + 8));
      }
      return false;
    }
    case 0x000B0014: {  // XSessionStart
      if (buffer && buffer_length >= 16) {
        REXKRNL_DEBUG("[stats-probe] XSessionStart obj={:08X} flags={:08X} nonce={:016X}",
                     Load32(buffer + 0), Load32(buffer + 4), Load64(buffer + 8));
      }
      return false;
    }
    case 0x000B0015: {  // XSessionEnd
      if (buffer && buffer_length >= 16) {
        REXKRNL_DEBUG("[stats-probe] XSessionEnd obj={:08X} flags={:08X} nonce={:016X}",
                     Load32(buffer + 0), Load32(buffer + 4), Load64(buffer + 8));
      }
      return false;
    }
    case 0x000B0025: {  // XSessionWriteStats
      // 24-byte header, 8-aligned: { u32 obj_ptr; u32 pad; u64 xuid; u32
      // num_views; u32 views_ptr } -> views are XSESSION_VIEW_PROPERTIES
      // { u32 view_id; u32 num_properties; u32 properties_ptr } (12 bytes)
      // and properties are XUSER_PROPERTY { u32 property_id; pad4;
      // XUSER_DATA value } (24 bytes; XUSER_DATA = u8 type @8, union @16:
      // i32/i64/double/float/FILETIME, or { u32 cbData; u32 pbData } for
      // unicode/binary). INT32/INT64 properties are recorded into the local
      // stats store; Geometry Wars submits its score as title-defined INT64
      // property 0x20000003 on view 0.
      if (buffer && buffer_length >= 20) {
        const uint32_t obj = Load32(buffer + 0);
        const uint64_t xuid = Load64(buffer + 8);
        const uint32_t num_views = Load32(buffer + 16);
        const uint32_t views_ptr = Load32(buffer + 20);
        REXKRNL_INFO("[stats] XSessionWriteStats xuid={:016X} views={} len={}", xuid, num_views,
                     buffer_length);
        REXKRNL_DEBUG("[stats-probe] XSessionWriteStats len={} obj={:08X} xuid={:016X} views={} @{:08X}",
                      buffer_length, obj, xuid, num_views, views_ptr);
        if (views_ptr && num_views && num_views <= 32) {
          for (uint32_t v = 0; v < num_views; ++v) {
            const uint32_t view = views_ptr + v * 12;
            const uint32_t view_id = Load32(view + 0);
            const uint32_t num_props = Load32(view + 4);
            const uint32_t props_ptr = Load32(view + 8);
            REXKRNL_DEBUG("[stats-probe]   view={:08X} properties={} @{:08X}", view_id, num_props,
                         props_ptr);
            if (!props_ptr || num_props > 64) continue;
            if (num_props)
              REXKRNL_DEBUG("[stats-probe]   prop0 raw=[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
                           Load32(props_ptr + 0), Load32(props_ptr + 4), Load32(props_ptr + 8),
                           Load32(props_ptr + 12), Load32(props_ptr + 16), Load32(props_ptr + 20));
            for (uint32_t i = 0; i < num_props; ++i) {
              const uint32_t prop = props_ptr + i * 24;
              const uint32_t prop_id = Load32(prop + 0);
              const uint32_t type = Load8(prop + 8);
              switch (type) {
                case 1:
                  REXKRNL_DEBUG("[stats-probe]     prop={:08X} i32={}", prop_id,
                               int32_t(Load32(prop + 16)));
                  break;
                case 2:
                  REXKRNL_DEBUG("[stats-probe]     prop={:08X} i64={}", prop_id,
                               int64_t(Load64(prop + 16)));
                  break;
                case 3: {
                  uint64_t bits = Load64(prop + 16);
                  double d;
                  std::memcpy(&d, &bits, 8);
                  REXKRNL_DEBUG("[stats-probe]     prop={:08X} double={}", prop_id, d);
                  break;
                }
                case 5: {
                  uint32_t bits = Load32(prop + 16);
                  float f;
                  std::memcpy(&f, &bits, 4);
                  REXKRNL_DEBUG("[stats-probe]     prop={:08X} float={}", prop_id, f);
                  break;
                }
                case 7:
                  REXKRNL_DEBUG("[stats-probe]     prop={:08X} datetime={:016X}", prop_id,
                               Load64(prop + 16));
                  break;
                case 4: {
                  const uint32_t cb = Load32(prop + 16);
                  const uint32_t ptr = Load32(prop + 20);
                  std::string text;
                  if (ptr && cb && cb <= 512) {
                    for (uint32_t c = 0; c < cb / 2; ++c) {
                      const uint16_t u = Load16(ptr + c * 2);
                      text += (u >= 0x20 && u < 0x7F) ? char(u) : '?';
                    }
                  }
                  REXKRNL_DEBUG("[stats-probe]     prop={:08X} unicode={}bytes \"{}\"", prop_id,
                               cb, text);
                  break;
                }
                case 6:
                  REXKRNL_DEBUG("[stats-probe]     prop={:08X} binary={}bytes @{:08X}", prop_id,
                               Load32(prop + 16), Load32(prop + 20));
                  break;
                default:
                  REXKRNL_DEBUG("[stats-probe]     prop={:08X} type={} raw={:016X}", prop_id, type,
                               Load64(prop + 16));
                  break;
              }
              if (type == 1 || type == 2) {
                const int64_t value = type == 1 ? int32_t(Load32(prop + 16))
                                                : int64_t(Load64(prop + 16));
                if (value > 0) {
                  const uint32_t title_id = K().image.title_id;
                  const bool changed =
                      Stats().Put(title_id, view_id, static_cast<uint64_t>(value));
                  REXKRNL_INFO("[stats] recorded title={:08X} view={} rating={} (changed={})",
                               title_id, view_id, value, changed);
                }
              }
            }
          }
        }
      }
      // The message is answered now: same SUCCESS the unhandled path returned,
      // same buffer zeroing it applied before returning.
      if (buffer && buffer_length) std::memset(G(buffer), 0, buffer_length);
      *status = X_ERROR_SUCCESS;
      return true;
    }
    case 0x000B0026: {  // XSessionFlushStats (same 24-byte, 8-aligned header)
      if (buffer && buffer_length >= 20) {
        REXKRNL_DEBUG("[stats-probe] XSessionFlushStats len={} obj={:08X} xuid={:016X} views={} @{:08X}",
                     buffer_length, Load32(buffer + 0), Load64(buffer + 8), Load32(buffer + 16),
                     Load32(buffer + 20));
      }
      return false;
    }
    default:
      return false;
  }
}

void NoteUnhandledMessage(const char* form, uint32_t app, uint32_t message, uint32_t buffer,
                          uint32_t length) {
  // Log each distinct (app, message) a few times with its buffer, not the
  // first N calls overall: the messages that matter arrive late and rarely.
  // App 0xFB is XGI - the XDK wrappers can send 0x000B0006 (XUserSetContextEx),
  // 0x000B0010/0011/0012/0014/0015 (session create/delete/join/start/end) and
  // 0x000B0021 (XUserReadStats, the leaderboard read: Geometry Wars sends it
  // the moment its Leaderboards screen opens with a LIVE sign-in). We answer
  // all of them SUCCESS and store nothing.
  static std::mutex seen_mutex;
  static std::map<uint64_t, uint32_t> seen;
  uint64_t key = (uint64_t(app) << 32) | message;
  std::lock_guard lock(seen_mutex);
  uint32_t& n = seen[key];
  if (n < 8) {
    n++;
    REXKRNL_WARN("{} app={:#x} msg={:#x} len={} buf=[{}] answered SUCCESS, buffer untouched", form,
                 app, message, length, buffer ? HexDump(buffer, length ? length : 32) : "");
  }
}

// Fills the message's output buffer and returns the status. buffer_length 0
// means "not carried" (the in-process form has no length argument).
uint32_t AnswerXamMessage(const char* form, uint32_t app, uint32_t message, uint32_t buffer,
                          uint32_t buffer_length) {
  if (app == kXamAppXmp) return XmpMessage(message, buffer, buffer_length);
  // Kinect camera/identity services are absent. These blocks carry inputs as
  // well as outputs, and the synchronous call has no size. Preserve the block
  // and report failure instead of enabling NUI on uninitialized reply fields.
  if (app == 0xFE && message >= 0x2B000 && message < 0x2D000)
    return 0xC0050006u;
  if (app == 0xFC) {  // XLiveBase: offline equivalents of the SDK service messages.
    switch (message) {
      case 0x00058004:  // Local logon identity (the in-process form has no length).
        if (!buffer || (buffer_length && buffer_length != 4)) return 0x80070057u;
        Store32(buffer, 1);
        return X_ERROR_SUCCESS;
      case 0x00058046:  // Forza's signed-in-profile notification, input only.
      case 0x00058037:  // Presence initialization, input only.
        return X_ERROR_SUCCESS;
      case 0x00058006:  // NAT discovery and service lookup require a LIVE connection.
      case 0x00058007:
        return 0x80151802u;  // ERROR_CONNECTION_INVALID
      case 0x00058020:  // No friends enumerator or accepted LIVE invitation offline.
      case 0x00058023:
      default:
        REXKRNL_DEBUG("{} offline XLiveBase msg={:#x}", form, message);
        return 0x80004005u;  // E_FAIL, never a successful uninitialized output.
    }
  }
  if (app == kXamAppXgi) {
    uint32_t status = X_ERROR_SUCCESS;
    if (AnswerXgiMessage(message, buffer, buffer_length, &status)) return status;
  }
  // We do not know this message's output layout, but we do know that leaving
  // the buffer alone hands the caller whatever was there before - which is how
  // a stack local became a "playback controller" and cost a fatal 1 GB
  // allocation. Zero is a defined "none" for every XAM out-structure; stale
  // memory is not. Dump it BEFORE zeroing so the log still shows what the
  // caller passed in.
  NoteUnhandledMessage(form, app, message, buffer, buffer_length);
  if (buffer && buffer_length) {
    std::memset(G(buffer), 0, buffer_length);
  } else if (buffer) {
    // The in-process form carries no length, so there is nothing safe to zero.
    static std::mutex once_mutex;
    static bool once = false;
    std::lock_guard lock(once_mutex);
    if (!once) {
      once = true;
      REXKRNL_WARN("{}: no buffer length for app={:#x} msg={:#x}; output buffer left UNWRITTEN - a "
                   "caller that reads it sees stale memory",
                   form, app, message);
    }
  }
  return X_ERROR_SUCCESS;
}

}  // namespace

u32 XMsgStartIORequest_entry(u32 app, u32 message, u32 overlapped, u32 buffer, u32 buffer_length) {
  uint32_t result = AnswerXamMessage("XMsgStartIORequest", app, message, buffer, buffer_length);
  if (overlapped) {
    CompleteOverlapped(overlapped, result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}
XN_IMPORT(__imp__XMsgStartIORequest, XMsgStartIORequest_entry)

u32 XMsgStartIORequestEx_entry(u32 app, u32 message, u32 overlapped, u32 buffer,
                               u32 buffer_length, u32 options) {
  return XMsgStartIORequest_entry(app, message, overlapped, buffer, buffer_length);
}
XN_IMPORT(__imp__XMsgStartIORequestEx, XMsgStartIORequestEx_entry)

void XNotifyPositionUI_entry(u32 position) {
  REXKRNL_DEBUG("Notification position {}", position);
}
XN_IMPORT(__imp__XNotifyPositionUI, XNotifyPositionUI_entry)

// The synchronous form: arg1 is the message buffer, arg2 a second dword (0 for
// every message this title sends). Geometry Wars 2 polls
// XMPGetPlaybackController through here about thirty times a second.
u32 XMsgInProcessCall_entry(u32 app, u32 message, u32 arg1, u32 arg2) {
  return AnswerXamMessage("XMsgInProcessCall", app, message, arg1, /*buffer_length=*/0);
}
XN_IMPORT(__imp__XMsgInProcessCall, XMsgInProcessCall_entry)

u32 XamContentGetLicenseMask_entry(mapped_u32 mask_ptr, u32 overlapped) {
  if (mask_ptr) *mask_ptr = 1;  // licensed
  if (overlapped) { CompleteOverlapped(overlapped, X_ERROR_SUCCESS); return X_ERROR_IO_PENDING; }
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamContentGetLicenseMask, XamContentGetLicenseMask_entry)

u32 XamAlloc_entry(u32 unk, u32 size, mapped_u32 out_ptr) {
  uint32_t p = K().arena.SystemAlloc(size, 16);
  if (out_ptr) *out_ptr = p;   // written on both paths, failure included
  if (!p) {
    REXKRNL_WARN("XamAlloc({} bytes) failed", size);
    return X_E_INVALIDARG;
  }
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamAlloc, XamAlloc_entry)
u32 XamFree_entry(u32 ptr) { K().arena.SystemFree(ptr); return X_ERROR_SUCCESS; }
XN_IMPORT(__imp__XamFree, XamFree_entry)

u32 XamGetExecutionId_entry(mapped_u32 info_ptr) {
  Kernel& k = K();
  if (!k.image.execution_info_offset) {
    if (info_ptr) *info_ptr = 0;  // never leave the caller a stale pointer
    return X_STATUS_UNSUCCESSFUL;
  }
  *info_ptr = k.xex_header_guest + k.image.execution_info_offset;
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__XamGetExecutionId, XamGetExecutionId_entry)

u32 XamEnumerate_entry(u32 handle, u32 flags, u32 buffer, u32 buffer_length, mapped_u32 items, u32 overlapped) {
  if (items) *items = 0;
  auto object = K().objects.Lookup(handle);
  auto enumerator = std::dynamic_pointer_cast<Enumerator>(object);
  uint32_t count = 0;
  uint32_t result = !object ? 6 : enumerator ? enumerator->Read(buffer, buffer_length, count) : 0x12;
  if (items) *items = count;
  if (overlapped) { CompleteOverlapped(overlapped, result, count); return X_ERROR_IO_PENDING; }
  return result;
}
XN_IMPORT(__imp__XamEnumerate, XamEnumerate_entry)

// Private providers (for example the system music playlist) aren't registered.
// Fail creation explicitly, clearing the handle consumed by the SDK wrapper.
u32 XamCreateEnumeratorHandle_entry(u32 user, u32 app, u32 open_message,
                                   u32 close_message, u32 item_size, u32 count,
                                   u32 flags, mapped_u32 handle) {
  if (handle) *handle = 0;
  REXKRNL_WARN("XamCreateEnumeratorHandle: provider {:#x}/{:#x} unavailable", app, open_message);
  return X_ERROR_INVALID_PARAMETER;
}
XN_IMPORT(__imp__XamCreateEnumeratorHandle, XamCreateEnumeratorHandle_entry)

// PGR4's private media enumerator is not a host-side provider. Do not return
// success with an uninitialized guest pointer: the caller checks this failure
// and skips enumerating unavailable dashboard content.
u32 XamGetPrivateEnumStructureFromHandle_entry(u32 handle, mapped_u32 structure_out) {
  if (structure_out) *structure_out = 0;
  REXKRNL_DEBUG("XamGetPrivateEnumStructureFromHandle({:#x}): no private provider", handle);
  return X_ERROR_NOT_FOUND;
}
XN_IMPORT(__imp__XamGetPrivateEnumStructureFromHandle, XamGetPrivateEnumStructureFromHandle_entry)

// XAM task scheduling is only used for console/dashboard work in this title.
// Fail scheduling explicitly so the guest's negative-status branch is taken;
// never claim a callback ran when we have not invoked it.
XN_IMPORT_RAW(__imp__XamTaskSchedule) {
  REXKRNL_WARN("XamTaskSchedule: dashboard task queue unavailable (task={:#010x})", ctx.r4.u32);
  if (ctx.r6.u32) Store32(ctx.r6.u32, 0);
  ctx.r3.u64 = X_E_INVALIDARG;  // HRESULT-style failure, so the guest takes its signed-error path
}
XN_IMPORT_RAW(__imp__XamTaskShouldExit) {
  ctx.r3.u64 = 0;  // no scheduled task is running or has been cancelled
}
XN_IMPORT_RAW(__imp__XamTaskCloseHandle) {
  REXKRNL_DEBUG("XamTaskCloseHandle({:#x}): no host task handle", ctx.r3.u32);
  ctx.r3.u64 = X_ERROR_SUCCESS;
}
XN_IMPORT_RAW(__imp__XamShowPlayersUI) {
  REXKRNL_WARN("XamShowPlayersUI: dashboard UI is unavailable; dismissing offline");
  ctx.r3.u64 = X_ERROR_SUCCESS;
}
XN_IMPORT_RAW(__imp__XamLoaderTerminateTitle) {
  REXKRNL_INFO("XamLoaderTerminateTitle: title exiting");
  K().title_running = false;
  while (true) std::this_thread::sleep_for(std::chrono::hours(1));
}
// XamLoaderLaunchTitle(path, flags) never returns on a console: it replaces the
// running title. Returning 0 said "launched" and then carried on executing the
// caller, which is a lie the caller cannot detect. Report the failure instead.
u32 XamLoaderLaunchTitle_entry(u32 path_ptr, u32 flags) {
  REXKRNL_WARN("XamLoaderLaunchTitle('{}', flags={:#x}) refused: this runtime hosts one title",
               path_ptr ? reinterpret_cast<const char*>(G(path_ptr)) : "", flags);
  return X_ERROR_NOT_FOUND;
}
XN_IMPORT(__imp__XamLoaderLaunchTitle, XamLoaderLaunchTitle_entry)

// Live session objects. Both have out-parameters that the guest reads whether
// or not it checks the status: Geometry Wars 2's 0x820E8658 does
//   XamSessionRefObjByHandle(h, &obj); ... ObDereferenceObject(obj);
// unconditionally, so leaving *obj alone hands ObDereferenceObject whatever was
// on the stack. Zero them and keep the failure status.
u32 XamSessionCreateHandle_entry(mapped_u32 handle_out) {
  if (handle_out) *handle_out = 0;
  return X_ERROR_NOT_FOUND;
}
XN_IMPORT(__imp__XamSessionCreateHandle, XamSessionCreateHandle_entry)

u32 XamSessionRefObjByHandle_entry(u32 handle, mapped_u32 object_out) {
  if (object_out) *object_out = 0;
  return X_ERROR_NOT_FOUND;
}
XN_IMPORT(__imp__XamSessionRefObjByHandle, XamSessionRefObjByHandle_entry)
// Leaderboard read. Logged with its arguments while we work out whether the
// title ever asks: (title_id, user, xuid_count, spec_count, specs, buffer_size
// out, handle out).
u32 XamUserCreateStatsEnumerator_entry(u32 title_id, u32 user_index, u32 xuid_count,
                                       u32 spec_count, u32 specs_ptr, mapped_u32 buffer_size_ptr,
                                       mapped_u32 handle_ptr) {
  REXKRNL_WARN("XamUserCreateStatsEnumerator title={:08X} user={} xuids={} specs={} [{}]", title_id,
               user_index, xuid_count, spec_count,
               specs_ptr ? HexDump(specs_ptr, spec_count * 16) : "");
  if (buffer_size_ptr) *buffer_size_ptr = 0;
  if (handle_ptr) *handle_ptr = 0;
  return X_ERROR_NOT_FOUND;
}
XN_IMPORT(__imp__XamUserCreateStatsEnumerator, XamUserCreateStatsEnumerator_entry)
// The blades. None of these can be shown, so each returns "the user dismissed
// it immediately". That is a fabrication either way - there is no honest
// "no UI exists" answer, because on a console there always is one - so the
// rule is: fabricate the *least surprising* outcome and write every output.
//
// XamShowMessageBoxUIEx(user, title_text, message_text, button_count,
//                       button_text_ptrs, active_button, flags, unknown,
//                       result_ptr, overlapped)
// Ten arguments: result_ptr and overlapped are the two stack ones, at
// r1+0x54 and r1+0x5C, which is how Geometry Wars 2's 0x82095CB0 passes them
// (it then waits on the overlapped if the call returns ERROR_IO_PENDING).
// Answer "the focused button was pressed" and complete the overlapped, so a
// caller that does wait is released instead of hanging.
u32 XamShowMessageBoxUIEx_entry(u32 user_index, u32 title_text, u32 message_text, u32 button_count,
                                u32 button_text_ptrs, u32 active_button, u32 flags, u32 unknown,
                                mapped_u32 result_ptr, u32 overlapped) {
  REXKRNL_WARN("XamShowMessageBoxUIEx: no UI; answering button {} of {}", active_button,
               button_count);
  if (result_ptr) *result_ptr = active_button;
  if (overlapped) {
    CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamShowMessageBoxUIEx, XamShowMessageBoxUIEx_entry)

// The rest have no output buffer, so there is nothing to leave stale. They do
// have a second contract we do not honour - the console posts XN_SYS_UI
// (notification 0x09) 1 when a blade opens and 0 when it closes - so a title
// that gates on those would wait forever. No title has yet; the log says so
// per call rather than once, because if one ever does, this line is the reason.
#define XN_UI_STUB(name)                                             \
  XN_IMPORT_RAW(__imp__##name) {                                     \
    (void)base;                                                      \
    REXKRNL_WARN(#name ": no UI shown, no XN_SYS_UI notification posted"); \
    ctx.r3.u64 = 0;                                                  \
  }
XN_UI_STUB(XamShowFriendsUI)
// The other five blades this macro used to cover now live in imports_ui.cpp
// with their real arities and their output buffers written. Do not re-add them
// here: XamShowDirtyDiscErrorUI in particular must not return.
#undef XN_UI_STUB

// ---- identity, version, enumerators (second title) ---------------------------------

// The gamertag the offline profile reports. One place, so XamUserGetName and
// anything later that shows a name agree.

u32 XamUserGetName_entry(u32 user_index, u32 buffer_ptr, u32 buffer_length) {
  if (((user_index & 0xFF) != 0xFF) && user_index != 0) return X_ERROR_NO_SUCH_USER;
  if (!buffer_ptr || buffer_length == 0) return X_ERROR_INVALID_PARAMETER;
  const auto& name = LocalUser().gamertag();
  size_t n = std::min<size_t>(name.size(), buffer_length - 1);
  std::memcpy(G(buffer_ptr), name.data(), n);
  Store8(buffer_ptr + uint32_t(n), 0);
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamUserGetName, XamUserGetName_entry)

// The packed dashboard version the XNet/XOnline startup wrappers compare
// against. This is the console's software version, not the title's: reporting
// a plausible retail dashboard keeps those wrappers on the path a real console
// takes, where returning 0 makes them treat the console as pre-release.
u32 XamGetSystemVersion_entry() { return 0x20006000; }  // 2.0.6000.0
XN_IMPORT(__imp__XamGetSystemVersion, XamGetSystemVersion_entry)

// XMsgCancelIORequest(overlapped, wait). xapilib's wrapper (0x820E8000) always
// passes wait = 1 and then returns 0 whatever we say. Overlapped operations in
// this runtime complete through the kernel timer thread about 32 ms late (that
// deferral is what let Geometry Wars 1 leave its leaderboard screen), so there
// genuinely is a window in which one is in flight and cancellable.
u32 XMsgCancelIORequest_entry(u32 overlapped, u32 wait) {
  const bool cancelled = overlapped ? CancelOverlapped(overlapped) : false;
  REXKRNL_DEBUG("XMsgCancelIORequest(overlapped={:#010x}, wait={}) -> {}", overlapped, wait,
                cancelled ? "cancelled" : "nothing in flight");
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XMsgCancelIORequest, XMsgCancelIORequest_entry)
