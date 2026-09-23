/**
 * @file        rex/ui/style.h
 *
 * @brief       Per-overlay styling that ImGuiStyle cannot express.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     ImGuiDrawer owns one Style; overlays read their own section
 *              through ImGuiDialog::imgui_drawer()->style(). Consumers patch
 *              it from ReXApp::OnConfigureStyle. Every default matches the
 *              value the corresponding overlay shipped with.
 */
#pragma once
#include <imgui.h>

namespace rex::ui {

/**
 * How much to scale ImGui's font for a display of this height.
 *
 * ImGui's built-in font is a desk size: legible at 1080p on a monitor an arm's
 * length away, and far too small on a 1440p ultrawide or on a TV across a
 * room. Everything the player reads while the game is up - the rails, the
 * achievements list - is drawn through this, so the two agree with each other
 * and grow together instead of each carrying its own hand-tuned multiplier.
 *
 * Referenced to 540 lines, which puts 1080p at 2.0 and leaves a small handheld
 * panel near 1.0. Clamped so neither a tiny nor an enormous mode is unusable.
 */
inline float UiFontScale(float display_height) {
  const float scale = display_height / 540.0f;
  if (scale < 1.0f) return 1.0f;
  if (scale > 4.0f) return 4.0f;
  return scale;
}

struct AchievementsStyle {
  ImVec4 unlocked_title{0.45f, 1.00f, 0.55f, 1.00f};
  ImVec4 unlocked_desc{0.70f, 0.85f, 0.72f, 1.00f};
  ImVec4 locked_title{0.78f, 0.80f, 0.84f, 1.00f};
  ImVec4 locked_desc{0.50f, 0.52f, 0.56f, 1.00f};
  ImVec4 badge_gamerscore{1.00f, 0.82f, 0.30f, 1.00f};
  ImVec4 row_unlocked_bg{0.16f, 0.30f, 0.18f, 0.55f};
  ImVec4 header_text{0.60f, 0.85f, 1.00f, 1.00f};
  ImVec4 progress_bar{0.30f, 0.80f, 0.40f, 1.00f};
  ImVec4 unlocked_icon_tint{1.00f, 1.00f, 1.00f, 1.00f};
  ImVec4 locked_icon_tint{0.45f, 0.45f, 0.45f, 0.80f};
  ImVec2 window_padding{18.0f, 16.0f};
  ImVec2 item_spacing{10.0f, 8.0f};
  float icon_size = 56.0f;
  float row_rounding = 3.0f;
};
// NOTE: this struct is embedded by value in objects a port compiles for
// itself, so adding a field to it breaks every port binary that is not rebuilt
// against the new header - and the failure does not surface anywhere near
// here (a stale port binary dies inside glibc thread-priority assertions).
// Anything new that only this overlay needs belongs beside the overlay, not
// in here.

struct ToastStyle {
  /// Alpha is the fade envelope, applied on top of these at draw time.
  ImVec4 text{1.00f, 1.00f, 1.00f, 1.00f};
  ImVec4 title{1.00f, 0.85f, 0.20f, 1.00f};
  float rounding = 6.0f;
  float background_alpha = 0.88f;
  float icon_size = 44.0f;
};

struct ConsoleStyle {
  /// Indexed by spdlog::level::level_enum: trace, debug, info, warn, err,
  /// critical, off.
  ImVec4 level[7] = {
      {0.5f, 0.5f, 0.5f, 1.0f},  // trace
      {0.4f, 0.9f, 0.9f, 1.0f},  // debug
      {1.0f, 1.0f, 1.0f, 1.0f},  // info
      {1.0f, 1.0f, 0.0f, 1.0f},  // warn
      {1.0f, 0.4f, 0.4f, 1.0f},  // err
      {1.0f, 0.0f, 0.0f, 1.0f},  // critical
      {1.0f, 1.0f, 1.0f, 1.0f},  // off
  };
  ImVec4 completion_bg{0.10f, 0.10f, 0.10f, 0.95f};
  ImVec4 completion_highlight{0.235f, 0.353f, 0.549f, 0.784f};
};

struct DebugOverlayStyle {
  ImVec4 muted_text{1.00f, 1.00f, 1.00f, 0.50f};
};

struct SettingsStyle {
  ImVec4 warning{1.00f, 0.80f, 0.20f, 1.00f};
  /// Cvar lifecycle badges: [live], [restart], [init-only].
  ImVec4 lifecycle_live{0.40f, 1.00f, 0.40f, 1.00f};
  ImVec4 lifecycle_restart{1.00f, 1.00f, 0.40f, 1.00f};
  ImVec4 lifecycle_init_only{1.00f, 0.40f, 0.40f, 1.00f};
  ImVec4 lifecycle_unknown{1.00f, 1.00f, 1.00f, 1.00f};
};

struct Style {
  AchievementsStyle achievements;
  ToastStyle toast;
  ConsoleStyle console;
  DebugOverlayStyle debug;
  SettingsStyle settings;
};

}  // namespace rex::ui
