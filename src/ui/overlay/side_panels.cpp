/**
 * @file        ui/overlay/side_panels.cpp
 * @brief       See side_panels.h. Generalised from the Split/Second port so
 *              every title carries the RetroRecomp rails; the right rail's text
 *              is cvar-driven and the logo is embedded in the runtime.
 */
#include <rex/ui/overlay/side_panels.h>

#include <rex/input/input_system.h>
#include <rex/runtime.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/keybinds.h>
#include <rex/graphics/present_stats.h>

#include "embedded/retro_recomp_logo.h"

// The presenter letterboxes the guest image to leave room for the rails; this
// is the shared cvar it reads. 0 = full frame (rails hidden), 16 = a rail each
// side.
REXCVAR_DECLARE(int32_t, present_side_panel_percent);
// Set by the presenter once it has fitted the picture: the width free each
// side, per mille of the surface. The rails size themselves from this so they
// sit beside the game rather than over it.
REXCVAR_DECLARE(int32_t, present_side_panel_room_permille);



// Off by default: the full rails eat real screen width (up to 30%) for a
// controls legend and build info. A small logo badge (DrawLogoBadge below)
// carries the branding whenever the rails are off. F8 (or the pause menu's
// "Side panels" entry) still brings the full rails back, or a title's own
// config can force them on.
REXCVAR_DEFINE_BOOL(show_side_panels, false, "UI",
                    "Show the RetroRecomp side rails (controls + game info). F8 toggles.");
REXCVAR_DEFINE_BOOL(show_fps, true, "UI",
                    "Show the on-screen FPS readout. F10 toggles.");

// The right rail's content. Empty title falls back to the window title; the info
// block is newline-separated lines. A port sets these in its config; the
// defaults are generic so an unconfigured port still reads correctly.
REXCVAR_DEFINE_STRING(side_panel_title, "", "UI",
                      "Right-rail heading; empty uses the window title.");
REXCVAR_DEFINE_STRING(side_panel_info,
                      "Static recompilation\n"
                      "PowerPC to native code\n"
                      "\n"
                      "Native kernel\n"
                      "Vulkan renderer\n"
                      "Native audio\n"
                      "\n"
                      "No emulation",
                      "UI", "Right-rail body, newline-separated lines.");
// A rail is read from across a room. The size comes from the display through
// UiFontScale; this multiplies it, for a screen that wants more or less than
// the reference.
REXCVAR_DEFINE_DOUBLE(side_panel_font_scale, 1.0, "UI",
                      "Multiplier on the display-derived side-rail font scale.");

namespace rex::ui {

namespace {
// Superseded by the side_panel_font_scale cvar; kept as the floor so a bad
// value in a config cannot render the rails unreadable.
constexpr float kRailFontScaleMin = 0.75f;
// The picture gets the rest: 70% of the width, and the full height wherever
// the display is wide enough for a 70%-width box to be taller than the
// picture needs (an ultrawide, or a handheld in landscape).
constexpr float kRailWidthFraction = 0.15f;
constexpr int32_t kRailPercent = 15;

// How many characters of the rail's own type should span its width. The
// legend's two columns and the info lines are all written to sit inside this.
constexpr float kRailColumns = 26.0f;

float RailFontScale(const ImGuiIO& io, float rail_width);

// The width one rail actually has, which the presenter works out from the
// fitted picture. Falls back to the floor before the first present.
float RailWidth(const ImGuiIO& io) {
  const int32_t permille = REXCVAR_GET(present_side_panel_room_permille);
  if (permille > 0) {
    return io.DisplaySize.x * float(permille) / 1000.0f;
  }
  return io.DisplaySize.x * kRailWidthFraction;
}

/*
 * Type sized to the rail, not to the display.
 *
 * The picture takes the full height it can and the rail is whatever width is
 * left over - a quarter of a 32:9 screen and a sliver of a 16:9 one. Sizing
 * from the rail's own width puts the same number of characters across it
 * either way.
 */
float RailFontScale(const ImGuiIO& io, float rail_width) {
  // A character of the default font is about half its height wide.
  const float glyph = std::max(ImGui::GetFontSize() * 0.5f, 1.0f);
  const float fit = std::max(rail_width - 24.0f, 1.0f) / (kRailColumns * glyph);
  const float wanted = fit * static_cast<float>(REXCVAR_GET(side_panel_font_scale));
  // Never so large that the rail's own content runs off the bottom.
  const float room = io.DisplaySize.y / (30.0f * ImGui::GetFontSize());
  return std::clamp(wanted, kRailFontScaleMin, std::max(room, kRailFontScaleMin));
}
}  // namespace

SidePanelsDialog::SidePanelsDialog(ImGuiDrawer* drawer, ImmediateDrawer* immediate,
                                   DebugOverlayDialog::FrameStatsProvider stats)
    : ImGuiDialog(drawer), stats_(std::move(stats)) {
  int w = 0, h = 0;
  auto pixels = DecodeImageRGBA(kRetroRecompLogoPng, kRetroRecompLogoPngSize, w, h);
  if (!pixels.empty() && immediate) {
    logo_ = immediate->CreateTexture(w, h, ImmediateTextureFilter::kLinear, false, pixels.data());
  }
  panels_visible_ = REXCVAR_GET(show_side_panels);
  fps_visible_ = REXCVAR_GET(show_fps);
  REXCVAR_SET(present_side_panel_percent, panels_visible_ ? kRailPercent : 0);

  RegisterBind("bind_side_panels", "F8", "Toggle RetroRecomp side panels",
               [this] { TogglePanels(); });
  RegisterBind("bind_fps", "F10", "Toggle FPS readout", [this] { ToggleFps(); });
}

void SidePanelsDialog::TogglePanels() {
  panels_visible_ = !panels_visible_;
  REXCVAR_SET(show_side_panels, panels_visible_);
  REXCVAR_SET(present_side_panel_percent, panels_visible_ ? kRailPercent : 0);
}

void SidePanelsDialog::ToggleFps() {
  fps_visible_ = !fps_visible_;
  REXCVAR_SET(show_fps, fps_visible_);
}

/*
 * The same two toggles, on the pad.
 *
 * A handheld has no F8 and no F10, so on the device these are unreachable.
 * The stick clicks rather than face or shoulder buttons: R3 and L3 are the two
 * a twin-stick shooter never asks for, so neither can be hit while playing.
 *
 * Edge-triggered - on the frame the stick goes down, not for every frame it is
 * held, which would strobe the panel at the frame rate.
 */
void SidePanelsDialog::PollPad() {
  auto* runtime = rex::Runtime::instance();
  auto* input =
      runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!input) {
    return;
  }
  rex::input::X_INPUT_STATE state{};
  if (input->GetStateRaw(0, &state) != X_ERROR_SUCCESS) {
    return;
  }
  const uint16_t buttons = static_cast<uint16_t>(state.gamepad.buttons);
  const uint16_t pressed = static_cast<uint16_t>(buttons & ~last_buttons_);
  last_buttons_ = buttons;
  if (!seen_pad_) {
    seen_pad_ = true;
    return;
  }
  if (pressed & rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB) {
    TogglePanels();
  }
  if (pressed & rex::input::X_INPUT_GAMEPAD_LEFT_THUMB) {
    ToggleFps();
  }
}

SidePanelsDialog::~SidePanelsDialog() {
  UnregisterBind("bind_side_panels");
  UnregisterBind("bind_fps");
  REXCVAR_SET(present_side_panel_percent, 0);
}

// The badge the full rails' branding shrinks to once they are off: the logo
// alone, small, bottom-left, out of the way of anything the game itself
// draws there. No text - at this size it would be unreadable.
void SidePanelsDialog::DrawLogoBadge(ImGuiIO& io) {
  if (!logo_) return;
  constexpr float kBadgeWidth = 240.0f;
  const float h = kBadgeWidth * logo_->height / logo_->width;
  constexpr float kMargin = 10.0f;
  ImGui::SetNextWindowPos(ImVec2(kMargin, io.DisplaySize.y - h - kMargin));
  ImGui::SetNextWindowSize(ImVec2(kBadgeWidth, h));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoBackground;
  ImGui::Begin("##logo_badge", nullptr, flags);
  ImGui::Image(reinterpret_cast<ImTextureID>(logo_.get()), ImVec2(kBadgeWidth, h));
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

void SidePanelsDialog::OnDraw(ImGuiIO& io) {
  PollPad();
  if (panels_visible_) {
    DrawRail(io, false);
    DrawRail(io, true);
  } else {
    if (REXCVAR_GET(present_side_panel_percent) != 0) {
      // Kept in sync if something else cleared the flag.
      REXCVAR_SET(present_side_panel_percent, 0);
    }
    DrawLogoBadge(io);
  }
  if (fps_visible_) {
    DrawFps(io, panels_visible_);
  }
}


// A legend row: what to press on the left, what it does on the right.
static void LegendRow(const char* action, const std::string& keys) {
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(keys.c_str());
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", action);
}

/*
 * The controls, keyboard and pad kept apart.
 *
 * Split because on a twin-stick game it matters which stick is which. Two
 * headed blocks, each a plain what-to-press / what-it-does table.
 */
static void DrawControlsLegend() {
  const ImVec4 heading(0.35f, 0.8f, 1.0f, 1.0f);
  constexpr auto table_flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
  // No wrapping inside the table: a wrap position set for the rail as a
  // whole applies to every column, which breaks a two-word key one letter
  // per line once the column is narrower than the rail.
  ImGui::PushTextWrapPos(-1.0f);

  /*
   * The pad only.
   *
   * These run on a handheld, where there is no keyboard to press, so the
   * keyboard half of the legend is not worth the space here. The keys still
   * work and are still read from their binds - they are simply not shown.
   */
  ImGui::TextColored(heading, "CONTROLS");
  ImGui::Spacing();
  if (ImGui::BeginTable("##pad", 2, table_flags)) {
    // The key column takes exactly what its longest label needs; without
    // this the proportional split wrapped "Left stick" under its own action.
    ImGui::TableSetupColumn("##key", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthStretch);
    LegendRow("Move", "L stick");
    LegendRow("Aim", "R stick");
    LegendRow("Confirm", "A");
    LegendRow("Back", "B");
    LegendRow("Pause", "Start");
    LegendRow("Select", "Back");
    LegendRow("Fire", "LT RT");
    ImGui::EndTable();
  }
  ImGui::PopTextWrapPos();
}

void SidePanelsDialog::DrawRail(ImGuiIO& io, bool right) {
  const float width = RailWidth(io);
  ImGui::SetNextWindowPos(ImVec2(right ? io.DisplaySize.x - width : 0, 0));
  ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.025f, 0.035f, 0.065f, 1));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.87f, 0.91f, 0.98f, 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 18));
  constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
  ImGui::Begin(right ? "Game information##rail" : "Controls##rail", nullptr, flags);
  ImGui::SetWindowFontScale(RailFontScale(io, width));

  const float inner = std::max(width - 24.0f, 1.0f);
  if (logo_) {
    ImGui::Image(reinterpret_cast<ImTextureID>(logo_.get()),
                 ImVec2(inner, inner * logo_->height / logo_->width));
  } else {
    ImGui::TextWrapped("RETRO RECOMPILATION");
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::PushTextWrapPos(inner);
  if (right) {
    const std::string title = REXCVAR_GET(side_panel_title);
    if (!title.empty()) {
      ImGui::TextColored(ImVec4(1, 0.42f, 0.2f, 1), "%s", title.c_str());
      ImGui::Spacing();
    }
    const std::string info = REXCVAR_GET(side_panel_info);
    size_t start = 0;
    while (start <= info.size()) {
      size_t nl = info.find('\n', start);
      const std::string line =
          info.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
      if (line.empty()) {
        ImGui::Spacing();
      } else {
        ImGui::TextWrapped("%s", line.c_str());
      }
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
  } else {
    DrawControlsLegend();
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextWrapped("R3  panels");
  ImGui::TextWrapped("L3  FPS");
  ImGui::TextDisabled("F8 / F10 on a keyboard");
  ImGui::PopTextWrapPos();
  ImGui::SetWindowFontScale(1.0f);
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}

void SidePanelsDialog::DrawFps(ImGuiIO& io, bool panels_visible) {
  // Top-right. When the rails are visible the FPS sits just inside the right
  // rail's top; when they are hidden it floats in the corner of the full frame.
  const float rail = panels_visible ? RailWidth(io) : 0.0f;
  const float margin = 10.0f;
  // Sample the guest present counter over ~0.5 s for the real game frame rate;
  // fall back to the imgui host rate only until the first window elapses.
  static uint64_t last_count = 0;
  static double last_time = 0.0;
  static double shown_fps = 0.0;
  const uint64_t count = rex::graphics::GuestPresentCount();
  const double now = ImGui::GetTime();
  if (last_time == 0.0) {
    last_time = now;
    last_count = count;
    shown_fps = io.Framerate;
  } else if (now - last_time >= 0.5) {
    shown_fps = double(count - last_count) / (now - last_time);
    last_count = count;
    last_time = now;
  }
  if (stats_) {
    const auto s = stats_();
    if (s.frame_count > 0) shown_fps = s.fps;  // prefer a real provider if ever wired
  }
  char text[32];
  std::snprintf(text, sizeof(text), "%.0f FPS", shown_fps);

  /*
   * Straight into the foreground draw list, at the font's own size - a
   * draw-list call has no window to size, clamp or fight over, so the
   * readout cannot be clamped back inside the viewport by imgui.
   */
  // 1.6x the default UI font - readable at a glance.
  constexpr float kFpsFontScale = 1.6f;
  ImFont* font = ImGui::GetFont();
  const float font_size = ImGui::GetFontSize() * kFpsFontScale;
  const ImVec2 size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text);
  const ImVec2 at(io.DisplaySize.x - rail - size.x - margin, margin);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  // A shadow rather than a panel behind it, so it stays legible on a bright
  // frame without covering any of it.
  dl->AddText(font, font_size, ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 170), text);
  dl->AddText(font, font_size, at, IM_COL32(150, 255, 150, 255), text);
}

}  // namespace rex::ui
