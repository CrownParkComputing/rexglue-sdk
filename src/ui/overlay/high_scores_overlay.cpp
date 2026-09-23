/**
 * @file        ui/overlay/high_scores_overlay.cpp
 * @brief       Local high-score page. See high_scores_overlay.h.
 */
#include <rex/ui/overlay/high_scores_overlay.h>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/system/high_scores.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/style.h>

REXCVAR_DECLARE(int32_t, present_side_panel_percent);

namespace rex::ui {

namespace {
constexpr float kPanelWidthFraction = 0.34f;
constexpr ImVec4 kRankColor{0.60f, 0.85f, 1.00f, 1.00f};
constexpr ImVec4 kScoreColor{1.00f, 0.82f, 0.30f, 1.00f};
}  // namespace

HighScoresOverlayDialog::HighScoresOverlayDialog(ImGuiDrawer* imgui_drawer, rex::Runtime* runtime,
                                                 std::function<void()> on_close_requested)
    : ImGuiDialog(imgui_drawer),
      runtime_(runtime),
      on_close_requested_(std::move(on_close_requested)) {
  rex::input::SetGuestInputSuppressed(true);
}

HighScoresOverlayDialog::~HighScoresOverlayDialog() {
  rex::input::SetGuestInputSuppressed(false);
}

// B or Back closes, same as the achievements list. The first poll only records
// what is held: a title's own Leaderboards menu was selected with a button that
// may still be down when this page appears, and treating that as a press would
// shut the page on the frame it opened.
void HighScoresOverlayDialog::PollPad() {
  auto* input = runtime_ ? static_cast<rex::input::InputSystem*>(runtime_->input_system()) : nullptr;
  if (!input) {
    return;
  }
  rex::input::X_INPUT_STATE state{};
  if (input->GetStateRaw(0, &state) != X_ERROR_SUCCESS) {
    return;
  }
  const uint16_t buttons = static_cast<uint16_t>(state.gamepad.buttons);
  constexpr uint16_t kCloseMask = rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_BACK;
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

void HighScoresOverlayDialog::OnDraw(ImGuiIO& io) {
  const AchievementsStyle& style = imgui_drawer()->style().achievements;

  const float width = io.DisplaySize.x * kPanelWidthFraction;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 0.0f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(1.0f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, style.window_padding);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, style.item_spacing);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

  constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav;

  PollPad();

  if (ImGui::Begin("High scores##overlay", nullptr, flags)) {
    const float scale = UiFontScale(io.DisplaySize.y);
    ImGui::SetWindowFontScale(scale);

    ImGui::PushStyleColor(ImGuiCol_Text, style.header_text);
    ImGui::TextUnformatted("HIGH SCORES");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("On this machine");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const auto scores = rex::system::TitleHighScores().List();
    if (scores.empty()) {
      // Said plainly rather than left blank: an empty panel reads as a page
      // that failed to load, and the honest answer is that nothing has been
      // scored yet.
      ImGui::TextDisabled("No scores yet.");
      ImGui::TextDisabled("Finish a game and it will appear here.");
    } else if (ImGui::BeginTable("##scores", 3,
                                 ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_NoSavedSettings)) {
      int rank = 1;
      for (const auto& entry : scores) {
        ImGui::TableNextColumn();
        ImGui::TextColored(kRankColor, "%d", rank++);
        ImGui::TableNextColumn();
        ImGui::TextColored(kScoreColor, "%llu", static_cast<unsigned long long>(entry.score));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", entry.context.c_str());
      }
      ImGui::EndTable();
    }

    ImGui::SetWindowFontScale(1.0f);
  }
  ImGui::End();

  ImGui::PopStyleVar(3);
}

}  // namespace rex::ui
