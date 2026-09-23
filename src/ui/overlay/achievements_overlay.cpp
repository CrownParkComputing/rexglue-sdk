/**
 * @file        ui/overlay/achievements_overlay.cpp
 * @brief       Achievements overlay implementation. See achievements_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/achievements_overlay.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/ui/immediate_drawer.h>

REXCVAR_DECLARE(int32_t, present_side_panel_percent);

namespace rex::ui {

namespace {
// Kept here rather than in AchievementsStyle: see the note in style.h - that
// struct crosses the SDK/port boundary by value and cannot grow without every
// port being rebuilt.
constexpr ImVec4 kStateUnlocked{0.45f, 1.00f, 0.55f, 1.00f};
constexpr ImVec4 kStateLocked{0.52f, 0.55f, 0.60f, 1.00f};
// How much of the screen the column takes down one side.
// Wide enough for two readable columns; the rails still take their own share
// and the game is letterboxed between what is left.
constexpr float kPanelWidthFraction = 0.46f;
constexpr int kColumns = 2;
}  // namespace

AchievementsOverlayDialog::AchievementsOverlayDialog(
    ImGuiDrawer* imgui_drawer, ImmediateDrawer* immediate_drawer, rex::Runtime* runtime,
    rex::system::AchievementManager* achievements, std::function<void()> on_close_requested)
    : ImGuiDialog(imgui_drawer),
      achievements_(achievements),
      icon_cache_(immediate_drawer, runtime),
      runtime_(runtime),
      on_close_requested_(std::move(on_close_requested)) {
  // The list takes the pad for as long as it is up, so the press that scrolls
  // it does not also drive the menu behind it.
  rex::input::SetGuestInputSuppressed(true);
}

AchievementsOverlayDialog::~AchievementsOverlayDialog() {
  rex::input::SetGuestInputSuppressed(false);
}

// Closes on B or Back, the two buttons that mean "out of this" on a 360 pad.
// Start is deliberately not one of them: it is what a lot of titles use to
// open a pause menu, and a player holding it from the menu underneath would
// have the list shut the instant it appeared.
void AchievementsOverlayDialog::PollPad() {
  auto* input = runtime_ ? static_cast<rex::input::InputSystem*>(runtime_->input_system()) : nullptr;
  if (!input) {
    return;
  }
  rex::input::X_INPUT_STATE state{};
  if (input->GetStateRaw(0, &state) != X_ERROR_SUCCESS) {
    return;
  }
  const uint16_t buttons = static_cast<uint16_t>(state.gamepad.buttons);
  constexpr uint16_t kCloseMask =
      rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_BACK;
  const uint16_t pressed = static_cast<uint16_t>(buttons & ~last_buttons_);
  last_buttons_ = buttons;
  if (!seen_pad_) {
    seen_pad_ = true;
    return;
  }
  if ((pressed & kCloseMask) && on_close_requested_) {
    on_close_requested_();
  }
}

ImmediateTexture* AchievementsOverlayDialog::GetIcon(
    const rex::system::AchievementInfo& achievement) {
  return icon_cache_.GetIcon(achievement);
}

/*
 * The list, two columns wide, centred between the rails rather than a
 * floating box.
 *
 * Two columns so a title's whole set fits on screen at once. The child region
 * keeps its scrollbar for a title with more than fits - losing rows off the
 * bottom would be worse than a scrollbar. Everything is drawn at the rails'
 * font scale; the default ImGui size is a desk size.
 */
void AchievementsOverlayDialog::OnDraw(ImGuiIO& io) {
  const AchievementsStyle& style = imgui_drawer()->style().achievements;

  // Centred on the screen rather than pinned to a side. The rails are still
  // drawn either side of it; the panel is narrow enough to sit between them.
  const float width = io.DisplaySize.x * kPanelWidthFraction;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 0.0f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y), ImGuiCond_Always);
  // Opaque: the game showing through the text makes the list hard to read.
  ImGui::SetNextWindowBgAlpha(1.0f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.window_padding);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, style.item_spacing);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

  constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav;

  PollPad();

  if (ImGui::Begin("Achievements##overlay", nullptr, flags)) {
    const float scale = UiFontScale(io.DisplaySize.y);
    ImGui::SetWindowFontScale(scale);

    const auto achievements = achievements_->ListAchievements();

    int unlocked_count = 0;
    int total_gs = 0;
    int earned_gs = 0;
    for (const auto& a : achievements) {
      total_gs += static_cast<int>(a.gamerscore);
      if (achievements_->IsUnlocked(a.id)) {
        ++unlocked_count;
        earned_gs += static_cast<int>(a.gamerscore);
      }
    }
    const int total_count = static_cast<int>(achievements.size());

    // ---- Header ------------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_Text, style.header_text);
    ImGui::TextUnformatted("ACHIEVEMENTS");
    ImGui::PopStyleColor();

    ImGui::Text("%d of %d", unlocked_count, total_count);
    ImGui::SameLine();
    ImGui::TextColored(style.badge_gamerscore, "%dG / %dG", earned_gs, total_gs);

    const float frac = total_count > 0 ? static_cast<float>(unlocked_count) / total_count : 0.0f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, style.progress_bar);
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 8.0f), "");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- The list ----------------------------------------------------------
    // Centred vertically too: the two-column list is shorter than the screen,
    // and hard against the top with a screen of empty space under it reads as
    // something that failed to load.
    ImGui::BeginChild("##achlist", ImVec2(0.0f, 0.0f), false);
    const float avail = ImGui::GetContentRegionAvail().y;
    if (last_content_height_ > 0.0f && last_content_height_ < avail) {
      ImGui::Dummy(ImVec2(0.0f, (avail - last_content_height_) * 0.5f));
    }
    const float content_top = ImGui::GetCursorPosY();
    // The scale is per-window and a child is its own window: without this the
    // header draws at the right size and every row in the list stays at
    // ImGui's default.
    ImGui::SetWindowFontScale(scale);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Smaller than a single-column row's icon: two columns halve the width a
    // row has, and the words are what has to stay readable.
    const float icon_px = style.icon_size * scale * 0.7f;

    if (ImGui::BeginTable("##achgrid", kColumns,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
      for (const auto& a : achievements) {
        ImGui::TableNextColumn();
        const bool is_unlocked = achievements_->IsUnlocked(a.id);
        const std::string& desc = is_unlocked ? a.description : a.unachieved_description;

        ImGui::PushID(static_cast<int>(a.id));
        const ImVec2 row_top = ImGui::GetCursorScreenPos();

        ImmediateTexture* icon = GetIcon(a);
        if (icon) {
          const ImVec4 tint = is_unlocked ? style.unlocked_icon_tint : style.locked_icon_tint;
          ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(icon), ImVec2(icon_px, icon_px),
                             ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint);
        } else {
          ImGui::Dummy(ImVec2(icon_px, icon_px));
        }
        ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::TextWrapped("%s", a.label.c_str());
        // The state, said in a word rather than punctuation: a row has to read
        // as unlocked or not without the reader learning a key first.
        ImGui::TextColored(is_unlocked ? kStateUnlocked : kStateLocked, "%s",
                           is_unlocked ? "UNLOCKED" : "LOCKED");
        ImGui::SameLine();
        ImGui::TextColored(style.badge_gamerscore, "%dG", static_cast<int>(a.gamerscore));
        ImGui::PushStyleColor(ImGuiCol_Text, is_unlocked ? style.unlocked_desc : style.locked_desc);
        ImGui::TextWrapped("%s", desc.c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        // An accent down the left of an unlocked row. A filled band behind the
        // whole row fought with the icon and the text; a rule does not.
        if (is_unlocked) {
          const float row_bottom = ImGui::GetCursorScreenPos().y;
          draw_list->AddRectFilled(ImVec2(row_top.x - 6.0f, row_top.y),
                                   ImVec2(row_top.x - 2.0f, row_bottom),
                                   ImGui::GetColorU32(kStateUnlocked), style.row_rounding);
        }

        ImGui::Spacing();
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    last_content_height_ = ImGui::GetCursorPosY() - content_top;
    ImGui::EndChild();
    ImGui::SetWindowFontScale(1.0f);
  }
  ImGui::End();

  ImGui::PopStyleVar(3);
}

}  // namespace rex::ui
