// xenon-native: deterministic scripted pad input (oracle harness, W5).
//
//   --script "<t:button[:hold]> ..."      e.g. "5:Start 9:A 12:lx=-1:2 14:RT:0.5"
//
// t is seconds since the title was launched; hold defaults to 0.4 s (the same
// as tools/headless_play.sh in the emulated port, whose xdotool key names are
// accepted too: Return=Start, space=A, Tab/z=Back, w/a/s/d=left stick,
// e=RT, q=LT, Up/Down/Left/Right=right stick). Axes: lx=,ly=,rx=,ry= in
// -1..1; triggers lt=,rt= in 0..1. The pure parser/evaluator lives here so it
// can be unit-tested without the kernel; imports_xam.cpp merges the result
// into the pad XamInputGetState reports.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace xn {

struct ScriptedPad {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool active = false;  // at least one step is currently held
};

struct InputStep {
  double at = 0;        // seconds
  double hold = 0.4;    // seconds
  std::string name;     // canonical: Start, A, lx, ...
  uint16_t buttons = 0; // XINPUT bits set while held
  int axis = -1;        // 0 lx, 1 ly, 2 rx, 3 ry, 4 lt, 5 rt; -1 none
  float value = 0;      // axis value (-1..1, triggers 0..1)
  bool fired = false;   // runtime: first-edge reported
};

class InputScript {
 public:
  // Returns false and sets `error` on a malformed script.
  bool Parse(const std::string& text, std::string& error);
  bool empty() const { return steps_.empty(); }
  const std::vector<InputStep>& steps() const { return steps_; }
  // Pad state at time t. Steps whose leading edge is crossed for the first time
  // are appended to `newly_pressed` (for logging the swap index they hit).
  ScriptedPad Evaluate(double t, std::vector<const InputStep*>* newly_pressed = nullptr);

 private:
  std::vector<InputStep> steps_;
};

// Maps a script token (XInput name or xdotool key name) to a step; false if unknown.
bool ResolveInputName(const std::string& token, InputStep& step);

}  // namespace xn
