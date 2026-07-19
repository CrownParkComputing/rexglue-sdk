/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
namespace rex::input {

// Synthetic input injection — a bringup/testing aid for headless runs. Holds a
// named controller button for a time window so a run can advance past a screen
// that waits for input (e.g. Split/Second's "PRESS B TO SKIP" content prompt)
// without a real gamepad or a focused window. The bits are OR'd into the merged
// device state, so this composes with any real input.
//   --synth_button=<a|b|x|y|start|back|lb|rb|ls|rs|dup|ddown|dleft|dright>
//   --synth_start_ms / --synth_end_ms : window (ms since first poll) to hold it
//   --synth_period_ms : if >0, repeat the [start,end) window every period_ms
REXCVAR_DEFINE_STRING(synth_button, "", "Input",
                      "Hold this controller button during the synthetic-input window (test aid)");
REXCVAR_DEFINE_UINT32(synth_start_ms, 0, "Input",
                      "Start of synthetic-input window, ms since first poll");
REXCVAR_DEFINE_UINT32(synth_end_ms, 0, "Input",
                      "End of synthetic-input window, ms since first poll");
REXCVAR_DEFINE_BOOL(log_input, false, "Input",
                    "Log the merged gamepad state delivered to the guest (sticks, buttons) - use "
                    "to check whether a controller's right stick reaches a twin-stick game.");
REXCVAR_DEFINE_UINT32(synth_rstick, 0, "Input",
                      "Synthetic right-thumbstick deflection (0 = off, max 32767). Rotates once "
                      "per synth_rstick_period_ms so twin-stick firing can be driven headlessly.");
REXCVAR_DEFINE_UINT32(synth_rstick_period_ms, 4000, "Input",
                      "Period of one full synthetic right-stick rotation, in milliseconds.");
REXCVAR_DEFINE_UINT32(synth_period_ms, 0, "Input",
                      "If >0, repeat the synthetic-input window every this many ms");

namespace {

uint16_t SynthButtonBit(const std::string& name) {
  if (name == "a") return X_INPUT_GAMEPAD_A;
  if (name == "b") return X_INPUT_GAMEPAD_B;
  if (name == "x") return X_INPUT_GAMEPAD_X;
  if (name == "y") return X_INPUT_GAMEPAD_Y;
  if (name == "start") return X_INPUT_GAMEPAD_START;
  if (name == "back") return X_INPUT_GAMEPAD_BACK;
  if (name == "lb") return X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (name == "rb") return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (name == "ls") return X_INPUT_GAMEPAD_LEFT_THUMB;
  if (name == "rs") return X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (name == "dup") return X_INPUT_GAMEPAD_DPAD_UP;
  if (name == "ddown") return X_INPUT_GAMEPAD_DPAD_DOWN;
  if (name == "dleft") return X_INPUT_GAMEPAD_DPAD_LEFT;
  if (name == "dright") return X_INPUT_GAMEPAD_DPAD_RIGHT;
  return 0;
}

// Returns the synthetic button bits to hold right now (0 if outside the window
// or unconfigured). Only affects controller 0.
uint16_t SyntheticButtons(uint32_t user_index) {
  if (user_index != 0) return 0;
  const std::string& name = REXCVAR_GET(synth_button);
  if (name.empty()) return 0;
  uint16_t bit = SynthButtonBit(name);
  if (!bit) return 0;

  static const auto t0 = std::chrono::steady_clock::now();
  uint64_t ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count());

  uint32_t start = REXCVAR_GET(synth_start_ms);
  uint32_t end = REXCVAR_GET(synth_end_ms);
  uint32_t period = REXCVAR_GET(synth_period_ms);
  if (period > 0) {
    ms %= period;
  }
  return (ms >= start && ms < end) ? bit : 0;
}

// Returns the synthetic right-thumbstick deflection to apply right now, or
// false if unconfigured. Twin-stick games (Geometry Wars) fire with the right
// stick, so a button-only synth cannot exercise shooting at all - every
// headless run scores zero no matter whether the game logic works. The stick
// rotates once per synth_rstick_period_ms so it sweeps all firing directions.
bool SyntheticRightStick(uint32_t user_index, int16_t& x_out, int16_t& y_out) {
  if (user_index != 0) {
    return false;
  }
  const uint32_t magnitude = REXCVAR_GET(synth_rstick);
  if (!magnitude) {
    return false;
  }
  static const auto t0 = std::chrono::steady_clock::now();
  const uint64_t ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count());
  const uint32_t period = std::max(REXCVAR_GET(synth_rstick_period_ms), UINT32_C(1));
  const double angle = 2.0 * 3.14159265358979323846 * double(ms % period) / double(period);
  const double amp = double(std::min(magnitude, UINT32_C(32767)));
  x_out = static_cast<int16_t>(amp * std::cos(angle));
  y_out = static_cast<int16_t>(amp * std::sin(angle));
  return true;
}

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  for (auto& driver : drivers_) {
    X_RESULT result = driver->GetCapabilities(user_index, flags, out_caps);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  bool first_result = true;
  X_INPUT_STATE merged = {};

  for (auto& driver : drivers_) {
    X_INPUT_STATE state = {};
    X_RESULT result = driver->GetState(user_index, &state);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      if (first_result) {
        merged = state;
        first_result = false;
      } else {
        // Merge: OR buttons, max triggers, max-magnitude sticks
        merged.gamepad.buttons = static_cast<uint16_t>(merged.gamepad.buttons) |
                                 static_cast<uint16_t>(state.gamepad.buttons);
        merged.gamepad.left_trigger =
            std::max(merged.gamepad.left_trigger, state.gamepad.left_trigger);
        merged.gamepad.right_trigger =
            std::max(merged.gamepad.right_trigger, state.gamepad.right_trigger);

        auto merge_axis = [](int16_t a, int16_t b) -> int16_t {
          return (std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b))) ? a : b;
        };
        merged.gamepad.thumb_lx = merge_axis(merged.gamepad.thumb_lx, state.gamepad.thumb_lx);
        merged.gamepad.thumb_ly = merge_axis(merged.gamepad.thumb_ly, state.gamepad.thumb_ly);
        merged.gamepad.thumb_rx = merge_axis(merged.gamepad.thumb_rx, state.gamepad.thumb_rx);
        merged.gamepad.thumb_ry = merge_axis(merged.gamepad.thumb_ry, state.gamepad.thumb_ry);

        if (static_cast<uint32_t>(state.packet_number) >
            static_cast<uint32_t>(merged.packet_number)) {
          merged.packet_number = state.packet_number;
        }
      }
    }
  }

  // Synthetic input injection (test aid): OR a scripted button into the merged
  // state. Reports "connected" even if no real device is, so a headless run can
  // drive input-gated screens.
  bool synth_applied = false;
  if (uint16_t synth = SyntheticButtons(user_index)) {
    if (first_result) {
      merged = {};
      first_result = false;
    }
    merged.gamepad.buttons =
        static_cast<uint16_t>(static_cast<uint16_t>(merged.gamepad.buttons) | synth);
    synth_applied = true;
  }
  int16_t synth_rx = 0, synth_ry = 0;
  if (SyntheticRightStick(user_index, synth_rx, synth_ry)) {
    if (first_result) {
      merged = {};
      first_result = false;
    }
    merged.gamepad.thumb_rx = synth_rx;
    merged.gamepad.thumb_ry = synth_ry;
    synth_applied = true;
  }
  if (synth_applied) {
    static uint16_t synth_pkt = 0;
    merged.packet_number = ++synth_pkt;
  }

  // Diagnostic (--log_input=true): report what the guest actually receives.
  // Twin-stick aiming problems are invisible from the outside - this shows
  // whether the right stick reaches the title at all, and from which driver.
  if (REXCVAR_GET(log_input) && user_index == 0 && !first_result) {
    static uint64_t last_log_ms = 0;
    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const int16_t rx = static_cast<int16_t>(merged.gamepad.thumb_rx);
    const int16_t ry = static_cast<int16_t>(merged.gamepad.thumb_ry);
    static int16_t last_rx = 0, last_ry = 0;
    const bool moved = std::abs(int(rx) - int(last_rx)) > 2048 ||
                       std::abs(int(ry) - int(last_ry)) > 2048;
    if (moved || now_ms - last_log_ms >= 1000) {
      last_log_ms = now_ms;
      last_rx = rx;
      last_ry = ry;
      REXLOG_INFO("input: buttons=0x{:04X} L=({},{}) R=({},{}) lt={} rt={} drivers={}",
                  uint16_t(merged.gamepad.buttons), int16_t(merged.gamepad.thumb_lx),
                  int16_t(merged.gamepad.thumb_ly), rx, ry, merged.gamepad.left_trigger,
                  merged.gamepad.right_trigger, drivers_.size());
    }
  }

  if (first_result) {
    return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  for (auto& driver : drivers_) {
    X_RESULT result = driver->SetState(user_index, vibration);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  for (auto& driver : drivers_) {
    X_RESULT result = driver->GetKeystroke(user_index, flags, out_keystroke);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS || result == X_ERROR_EMPTY) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  return input;
}

}  // namespace rex::input
