#include "input_script.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace xn {
namespace {

constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
                   kStart = 0x0010, kBack = 0x0020, kLThumb = 0x0040, kRThumb = 0x0080,
                   kLShoulder = 0x0100, kRShoulder = 0x0200, kA = 0x1000, kB = 0x2000,
                   kX = 0x4000, kY = 0x8000;

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

struct Name { const char* token; const char* canonical; uint16_t buttons; int axis; float value; };
const Name kNames[] = {
    // XInput names.
    {"start", "Start", kStart, -1, 0}, {"back", "Back", kBack, -1, 0},
    {"a", "A", kA, -1, 0}, {"b", "B", kB, -1, 0}, {"x", "X", kX, -1, 0}, {"y", "Y", kY, -1, 0},
    {"lb", "LB", kLShoulder, -1, 0}, {"rb", "RB", kRShoulder, -1, 0},
    {"ls", "LS", kLThumb, -1, 0}, {"rs", "RS", kRThumb, -1, 0},
    {"dup", "DUp", kDpadUp, -1, 0}, {"ddown", "DDown", kDpadDown, -1, 0},
    {"dleft", "DLeft", kDpadLeft, -1, 0}, {"dright", "DRight", kDpadRight, -1, 0},
    {"lt", "lt", 0, 4, 1}, {"rt", "rt", 0, 5, 1},
    // xdotool key names as tools/headless_play.sh (mnk_mode) uses them.
    {"return", "Start", kStart, -1, 0}, {"space", "A", kA, -1, 0},
    {"tab", "Back", kBack, -1, 0}, {"z", "Back", kBack, -1, 0},
    {"w", "ly", 0, 1, 1}, {"s", "ly", 0, 1, -1}, {"d", "lx", 0, 0, 1},
    {"e", "rt", 0, 5, 1}, {"q", "lt", 0, 4, 1},
    {"up", "ry", 0, 3, 1}, {"down", "ry", 0, 3, -1}, {"left", "rx", 0, 2, -1}, {"right", "rx", 0, 2, 1},
};

}  // namespace

bool ResolveInputName(const std::string& token, InputStep& step) {
  std::string t = Lower(token);
  // Axis assignment: lx=-1, ry=0.5, rt=1.
  auto eq = t.find('=');
  if (eq != std::string::npos) {
    std::string axis = t.substr(0, eq);
    const char* names[] = {"lx", "ly", "rx", "ry", "lt", "rt"};
    for (int i = 0; i < 6; i++) {
      if (axis == names[i]) {
        char* end = nullptr;
        float v = std::strtof(t.c_str() + eq + 1, &end);
        if (end == t.c_str() + eq + 1 || *end) return false;
        step.name = names[i];
        step.axis = i;
        step.value = std::clamp(v, i >= 4 ? 0.f : -1.f, 1.f);
        step.buttons = 0;
        return true;
      }
    }
    return false;
  }
  // "a" is ambiguous between XInput A and the oracle's a=left-stick-left; the
  // XInput reading wins (the oracle harness scripts use A only for menus).
  for (const Name& n : kNames) {
    if (t == n.token) {
      step.name = n.canonical;
      step.buttons = n.buttons;
      step.axis = n.axis;
      step.value = n.value;
      return true;
    }
  }
  return false;
}

bool InputScript::Parse(const std::string& text, std::string& error) {
  steps_.clear();
  std::istringstream in(text);
  std::string tok;
  while (in >> tok) {
    // t:name[:hold]
    std::vector<std::string> parts;
    size_t pos = 0;
    while (true) {
      size_t c = tok.find(':', pos);
      parts.push_back(tok.substr(pos, c == std::string::npos ? std::string::npos : c - pos));
      if (c == std::string::npos) break;
      pos = c + 1;
    }
    if (parts.size() < 2 || parts.size() > 3) { error = "bad step '" + tok + "' (want t:name[:hold])"; return false; }
    InputStep step;
    char* end = nullptr;
    step.at = std::strtod(parts[0].c_str(), &end);
    if (end == parts[0].c_str() || *end || step.at < 0) { error = "bad time in '" + tok + "'"; return false; }
    if (!ResolveInputName(parts[1], step)) { error = "unknown input '" + parts[1] + "' in '" + tok + "'"; return false; }
    if (parts.size() == 3) {
      step.hold = std::strtod(parts[2].c_str(), &end);
      if (end == parts[2].c_str() || *end || step.hold <= 0) { error = "bad hold in '" + tok + "'"; return false; }
    }
    steps_.push_back(step);
  }
  std::stable_sort(steps_.begin(), steps_.end(), [](const InputStep& a, const InputStep& b) { return a.at < b.at; });
  return true;
}

ScriptedPad InputScript::Evaluate(double t, std::vector<const InputStep*>* newly_pressed) {
  ScriptedPad pad;
  for (InputStep& s : steps_) {
    if (t < s.at) break;  // sorted by time
    if (t >= s.at + s.hold) continue;
    if (!s.fired) { s.fired = true; if (newly_pressed) newly_pressed->push_back(&s); }
    pad.active = true;
    pad.buttons |= s.buttons;
    auto stick = [&](int16_t& a) { a = int16_t(std::lround(s.value * 32767.f)); };
    switch (s.axis) {
      case 0: stick(pad.lx); break;
      case 1: stick(pad.ly); break;
      case 2: stick(pad.rx); break;
      case 3: stick(pad.ry); break;
      case 4: pad.lt = uint8_t(std::lround(s.value * 255.f)); break;
      case 5: pad.rt = uint8_t(std::lround(s.value * 255.f)); break;
      default: break;
    }
  }
  return pad;
}

}  // namespace xn
