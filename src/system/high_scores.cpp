/**
 * @file        system/high_scores.cpp
 * @brief       Local high-score table. See high_scores.h for why it exists.
 */
#include <rex/system/high_scores.h>

#include <algorithm>
#include <fstream>

#include <fmt/format.h>
#include <toml++/toml.h>

#include <rex/logging.h>

namespace rex::system {

void HighScores::SetSavePath(std::filesystem::path path) {
  std::lock_guard lock(mutex_);
  save_path_ = std::move(path);
}

void HighScores::Load() {
  std::filesystem::path path;
  {
    std::lock_guard lock(mutex_);
    path = save_path_;
  }
  if (path.empty() || !std::filesystem::exists(path)) {
    return;
  }

  std::vector<HighScore> loaded;
  try {
    auto table = toml::parse_file(path.string());
    if (auto* entries = table["score"].as_array()) {
      for (auto& node : *entries) {
        auto* entry = node.as_table();
        if (!entry) continue;
        HighScore score;
        score.score = (*entry)["score"].value_or<uint64_t>(0);
        score.context = (*entry)["context"].value_or<std::string>("");
        score.filetime = (*entry)["filetime"].value_or<uint64_t>(0);
        if (score.score) loaded.push_back(std::move(score));
      }
    }
  } catch (const toml::parse_error& error) {
    // Refused rather than ignored: a board that silently comes back empty is
    // indistinguishable from one that was never written, and the next Save
    // would overwrite whatever was really in there.
    REXSYS_ERROR("High scores: {} will not parse ({}); leaving it alone", path.string(),
                 error.description());
    return;
  }

  std::sort(loaded.begin(), loaded.end(),
            [](const HighScore& a, const HighScore& b) { return a.score > b.score; });
  if (loaded.size() > kMaxEntries) loaded.resize(kMaxEntries);

  std::lock_guard lock(mutex_);
  scores_ = std::move(loaded);
}

void HighScores::Save() const {
  std::lock_guard save_lock(save_mutex_);

  std::filesystem::path path;
  std::vector<HighScore> scores;
  {
    std::lock_guard lock(mutex_);
    path = save_path_;
    scores = scores_;
  }
  if (path.empty()) {
    return;
  }

  std::error_code ec;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  if (ec) {
    REXSYS_WARN("High scores: cannot create {}: {}", path.parent_path().string(), ec.message());
    return;
  }

  std::string content = "# Local high scores - managed by the ReXGlue runtime\n\n";
  for (const auto& score : scores) {
    content += fmt::format("[[score]]\nscore = {}\nfiletime = {}\ncontext = \"{}\"\n\n", score.score,
                           score.filetime, score.context);
  }

  // Temp and rename, so an interrupted write cannot leave a half-written board.
  std::filesystem::path temporary_path = path;
  temporary_path += ".tmp";
  {
    std::ofstream file(temporary_path, std::ios::binary);
    if (!file) {
      REXSYS_WARN("High scores: cannot write {}", temporary_path.string());
      return;
    }
    file << content;
  }
  std::filesystem::rename(temporary_path, path, ec);
  if (ec) {
    REXSYS_WARN("High scores: cannot replace {}: {}", path.string(), ec.message());
  }
}

bool HighScores::Submit(uint64_t score, std::string context) {
  if (!score) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    // The same score twice is the same run reported twice, which is what a
    // title flushing its stats on both pause and game over looks like.
    for (const auto& existing : scores_) {
      if (existing.score == score && existing.context == context) {
        return false;
      }
    }
    if (scores_.size() >= kMaxEntries && score <= scores_.back().score) {
      return false;
    }
    HighScore entry;
    entry.score = score;
    entry.context = std::move(context);
    entry.filetime = 0;
    scores_.push_back(std::move(entry));
    std::sort(scores_.begin(), scores_.end(),
              [](const HighScore& a, const HighScore& b) { return a.score > b.score; });
    if (scores_.size() > kMaxEntries) scores_.resize(kMaxEntries);
  }
  Save();
  return true;
}

std::vector<HighScore> HighScores::List() const {
  std::lock_guard lock(mutex_);
  return scores_;
}

uint64_t HighScores::Best() const {
  std::lock_guard lock(mutex_);
  return scores_.empty() ? 0 : scores_.front().score;
}

HighScores& TitleHighScores() {
  static HighScores scores;
  return scores;
}

}  // namespace rex::system
