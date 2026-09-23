/**
 * @file        rex/ui/overlay/high_scores_overlay.h
 * @brief       Local high-score page, the blade's answer to a title's
 *              Leaderboards menu item.
 *
 * Drawn in the same language as the achievements list - centred, opaque, text
 * sized from the display - because to the player they are two pages of the same
 * thing.
 */
#pragma once

#include <functional>

#include <rex/ui/imgui_dialog.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class HighScoresOverlayDialog : public ImGuiDialog {
 public:
  /// @param on_close_requested  See AchievementsOverlayDialog: the dialog does
  ///   not delete itself, because ReXApp holds it in a unique_ptr.
  HighScoresOverlayDialog(ImGuiDrawer* imgui_drawer, rex::Runtime* runtime,
                          std::function<void()> on_close_requested = {});
  ~HighScoresOverlayDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void PollPad();

  rex::Runtime* runtime_ = nullptr;
  std::function<void()> on_close_requested_;
  uint16_t last_buttons_ = 0;
  bool seen_pad_ = false;
};

}  // namespace rex::ui
