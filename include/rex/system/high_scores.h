/**
 * @file        rex/system/high_scores.h
 * @brief       A title's local high-score table.
 *
 * The score board a recompiled title cannot have any other way. Its own
 * Leaderboards screen is a Live screen: it reads through XUserReadStats against
 * a live session, and cannot be fed local numbers without emulating the service
 * behind it. So the board lives here, beside the achievements, and the title's
 * Leaderboards menu item opens it instead.
 *
 * Keyed by title id and stored under the user data root, exactly as achievement
 * unlocks are, so two titles cannot collide and a newly recompiled one needs no
 * new code to get a working board.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace rex::system {

struct HighScore {
  uint64_t score = 0;
  /// What the score was made in - a mode name, a level, a lap. Free text
  /// because only the title knows what its score means.
  std::string context;
  /// Win32 filetime, as achievement unlocks are stamped.
  uint64_t filetime = 0;
};

class HighScores {
 public:
  /// Where the board persists. Empty disables saving, which is what a run
  /// with no user data root gets.
  void SetSavePath(std::filesystem::path path);

  void Load();
  void Save() const;

  /// Records a score if it belongs on the board. Returns true when the board
  /// changed, so a caller can say so.
  bool Submit(uint64_t score, std::string context = {});

  /// Highest first, at most kMaxEntries.
  std::vector<HighScore> List() const;

  uint64_t Best() const;

  static constexpr size_t kMaxEntries = 10;

 private:
  mutable std::mutex mutex_;
  mutable std::mutex save_mutex_;
  std::filesystem::path save_path_;
  std::vector<HighScore> scores_;
};

/**
 * The board for the title that is running.
 *
 * A free accessor over a function-local static rather than a member of
 * KernelState: that class is defined in a public header and embedded by value,
 * so widening it breaks every port binary that is not rebuilt against the new
 * header - and it breaks them a long way from here. See the note in ui/style.h.
 */
HighScores& TitleHighScores();

}  // namespace rex::system
