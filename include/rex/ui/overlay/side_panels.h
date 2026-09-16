/**
 * @file        rex/ui/overlay/side_panels.h
 * @brief       RetroRecomp branded side rails + FPS readout, shared by every
 *              title. The game renders letterboxed between two rails (via the
 *              present_side_panel_percent cvar): the left rail is a generic
 *              controls legend, the right rail is the RetroRecomp mark plus the
 *              title's own information, read from cvars so no port needs bespoke
 *              code. F8 toggles the rails, F10 toggles the FPS readout; both are
 *              on by default. See side_panels.cpp for the detail.
 */
#pragma once

#include <memory>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/immediate_drawer.h>

namespace rex::ui {

// A single dialog that owns the RetroRecomp rails and the FPS readout. ReXApp
// creates one and adds it to the imgui drawer; a title customises the right
// rail purely through cvars (side_panel_title / side_panel_info).
class SidePanelsDialog final : public ImGuiDialog {
 public:
  SidePanelsDialog(ImGuiDrawer* drawer, ImmediateDrawer* immediate,
                   DebugOverlayDialog::FrameStatsProvider stats = {});
  ~SidePanelsDialog() override;

 private:
  void OnDraw(ImGuiIO& io) override;
  void DrawRail(ImGuiIO& io, bool right);
  void DrawFps(ImGuiIO& io, bool panels_visible);

  bool panels_visible_ = true;
  bool fps_visible_ = true;
  std::unique_ptr<ImmediateTexture> logo_;
  DebugOverlayDialog::FrameStatsProvider stats_;
};

}  // namespace rex::ui
