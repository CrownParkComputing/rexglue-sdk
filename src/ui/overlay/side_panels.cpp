/**
 * @file        ui/overlay/side_panels.cpp
 * @brief       See side_panels.h. Generalised from the Split/Second port so
 *              every title carries the RetroRecomp rails; the right rail's text
 *              is cvar-driven and the logo is embedded in the runtime.
 */
#include <rex/ui/overlay/side_panels.h>

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


// Shown by default so a fresh download is branded and self-explaining. Each is
// toggleable at runtime and can be forced off per title in config.
REXCVAR_DEFINE_BOOL(show_side_panels, true, "UI",
                    "Show the RetroRecomp side rails (controls + game info). F8 toggles.");
REXCVAR_DEFINE_BOOL(show_fps, true, "UI",
                    "Show the on-screen FPS readout. F10 toggles.");

// The right rail's content. Empty title falls back to the window title; the info
// block is newline-separated lines. A port sets these in its config; the
// defaults are generic so an unconfigured port still reads correctly.
REXCVAR_DEFINE_STRING(side_panel_title, "", "UI",
                      "Right-rail heading; empty uses the window title.");
REXCVAR_DEFINE_STRING(side_panel_info,
                      "Native recompilation\nVulkan renderer\nNative audio\nWork in progress", "UI",
                      "Right-rail body, newline-separated lines.");

namespace rex::ui {

namespace {
// Larger than the Split/Second original (which drew at the default 1.0 scale).
constexpr float kRailFontScale = 1.35f;
constexpr float kRailWidthFraction = 0.16f;
constexpr int32_t kRailPercent = 16;
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

  RegisterBind("bind_side_panels", "F8", "Toggle RetroRecomp side panels", [this] {
    panels_visible_ = !panels_visible_;
    REXCVAR_SET(show_side_panels, panels_visible_);
    REXCVAR_SET(present_side_panel_percent, panels_visible_ ? kRailPercent : 0);
  });
  RegisterBind("bind_fps", "F10", "Toggle FPS readout",
               [this] { fps_visible_ = !fps_visible_; REXCVAR_SET(show_fps, fps_visible_); });
}

SidePanelsDialog::~SidePanelsDialog() {
  UnregisterBind("bind_side_panels");
  UnregisterBind("bind_fps");
  REXCVAR_SET(present_side_panel_percent, 0);
}

void SidePanelsDialog::OnDraw(ImGuiIO& io) {
  if (panels_visible_) {
    DrawRail(io, false);
    DrawRail(io, true);
  } else if (REXCVAR_GET(present_side_panel_percent) != 0) {
    // Kept in sync if something else cleared the flag.
    REXCVAR_SET(present_side_panel_percent, 0);
  }
  if (fps_visible_) {
    DrawFps(io, panels_visible_);
  }
}

void SidePanelsDialog::DrawRail(ImGuiIO& io, bool right) {
  const float width = io.DisplaySize.x * kRailWidthFraction;
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
  ImGui::SetWindowFontScale(kRailFontScale);

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
  ImGui::PushTextWrapPos(0);
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
    ImGui::TextColored(ImVec4(0.35f, 0.8f, 1, 1), "CONTROLS");
    ImGui::TextWrapped("Keyboard / controller");
    ImGui::Spacing();
    ImGui::TextWrapped("Enter / Start\nStart / pause");
    ImGui::TextWrapped("Space / A\nConfirm");
    ImGui::TextWrapped("Backspace / B\nBack");
    ImGui::TextWrapped("W A S D / left stick\nMove");
    ImGui::TextWrapped("E / RT   Q / LT\nTriggers");
    ImGui::TextWrapped("Arrows / D-pad\nMenus");
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextWrapped("F8  panels   F10  FPS");
  ImGui::PopTextWrapPos();
  ImGui::SetWindowFontScale(1.0f);
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}

void SidePanelsDialog::DrawFps(ImGuiIO& io, bool panels_visible) {
  // Top-right. When the rails are visible the FPS sits just inside the right
  // rail's top; when they are hidden it floats in the corner of the full frame.
  const float rail = panels_visible ? io.DisplaySize.x * kRailWidthFraction : 0.0f;
  const float margin = 14.0f;
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
  const ImVec2 size = ImGui::CalcTextSize(text);
  const float scale = 1.4f;
  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x - rail - size.x * scale - margin * 2.0f, margin));
  ImGui::SetNextWindowBgAlpha(panels_visible ? 0.0f : 0.35f);
  constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_AlwaysAutoResize;
  ImGui::Begin("##fps", nullptr, flags);
  ImGui::SetWindowFontScale(scale);
  ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "%s", text);
  ImGui::SetWindowFontScale(1.0f);
  ImGui::End();
}

}  // namespace rex::ui
