/**
 * @file        rex/ui/overlay/achievements_overlay.h
 * @brief       Default ImGui achievements overlay dialog.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <functional>

#include <rex/system/achievement_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class ImmediateDrawer;
class ImmediateTexture;

class AchievementsOverlayDialog : public ImGuiDialog {
 public:
  /**
   * @param on_close_requested  Run when the player closes the list with the
   *   pad. The dialog does not delete itself: ReXApp holds it in a unique_ptr,
   *   and ImGuiDialog::Close() deletes from inside Draw(), which would leave
   *   that pointer dangling for the next toggle to free a second time.
   */
  AchievementsOverlayDialog(ImGuiDrawer* imgui_drawer, ImmediateDrawer* immediate_drawer,
                            rex::Runtime* runtime, rex::system::AchievementManager* achievements,
                            std::function<void()> on_close_requested = {});
  ~AchievementsOverlayDialog() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Lazily loads icon_path, or icons/<image_id>.png when icon_path is empty.
  ImmediateTexture* GetIcon(const rex::system::AchievementInfo& achievement);

  // Reads the pad directly and asks to be closed on B or Back.
  void PollPad();

  rex::system::AchievementManager* achievements_ = nullptr;
  AchievementIconCache icon_cache_;
  rex::Runtime* runtime_ = nullptr;
  std::function<void()> on_close_requested_;
  // Edge detection: the list must close on the press, not go on closing for
  // every frame the button is held. The first poll only records what is held,
  // because a button already down when the list opened is not a press - and
  // treating it as one shuts the list on the same frame it appeared.
  uint16_t last_buttons_ = 0;
  bool seen_pad_ = false;
  // How tall the list came out last frame, used to centre it in this one.
  // ImGui cannot say how tall content will be before drawing it, and one
  // frame's lag is invisible on a list that does not change while it is up.
  float last_content_height_ = 0.0f;
};

}  // namespace rex::ui
