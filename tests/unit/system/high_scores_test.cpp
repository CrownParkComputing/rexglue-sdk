/**
 * @file        high_scores_test.cpp
 * @brief       Unit tests for the local high-score table's per-view boards.
 */

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/high_scores.h>

namespace {

class TempDirectory {
 public:
  explicit TempDirectory(std::string_view name) {
    static std::atomic<uint64_t> next_id{0};
    auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            (std::string(name) + "_" + std::to_string(suffix) + "_" + std::to_string(next_id++));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("high scores keep one board per view", "[high_scores]") {
  TempDirectory dir("rex_hs");
  rex::system::HighScores scores;
  scores.SetSavePath(dir.path() / "584107ED.toml");
  scores.Load();

  REQUIRE(scores.Submit(1000, 0, "solo"));
  REQUIRE(scores.Submit(500, 1, "multi"));
  REQUIRE(scores.Submit(300, 0, "solo"));

  CHECK(scores.List().size() == 3);
  CHECK(scores.List(0).size() == 2);
  CHECK(scores.List(1).size() == 1);
  CHECK(scores.List(0).front().score == 1000);
  CHECK(scores.Best(1) == 500);
  CHECK(scores.Best(2) == 0);
  CHECK((scores.Views() == std::vector<uint32_t>{0, 1}));
}

TEST_CASE("high scores cap is per view", "[high_scores]") {
  TempDirectory dir("rex_hs");
  rex::system::HighScores scores;
  scores.SetSavePath(dir.path() / "584107ED.toml");
  scores.Load();

  for (uint64_t i = 1; i <= 12; ++i) scores.Submit(i * 100, 0, "a");
  scores.Submit(7, 1, "b");
  CHECK(scores.List(0).size() == rex::system::HighScores::kMaxEntries);
  CHECK(scores.List(0).back().score == 300);
  CHECK(scores.List(1).size() == 1);
  // A worse score than the view's worst is refused, but only on that view.
  CHECK_FALSE(scores.Submit(50, 0, "a"));
  CHECK(scores.Submit(50, 2, "c"));
}

TEST_CASE("high scores persist views through toml", "[high_scores]") {
  TempDirectory dir("rex_hs");
  const auto path = dir.path() / "584107ED.toml";
  {
    rex::system::HighScores scores;
    scores.SetSavePath(path);
    scores.Load();
    scores.Submit(14400, 0, "prop 20000003");
    scores.Submit(9000, 5, "prop 20000003");
  }
  {
    rex::system::HighScores scores;
    scores.SetSavePath(path);
    scores.Load();
    CHECK((scores.Views() == std::vector<uint32_t>{0, 5}));
    CHECK(scores.List(5).front().score == 9000);
    CHECK(scores.List(5).front().context == "prop 20000003");
  }

  // A board written before the view field existed loads as view 0.
  std::ofstream(path) << "[[score]]\nscore = 1234\nfiletime = 0\ncontext = \"old\"\n";
  {
    rex::system::HighScores scores;
    scores.SetSavePath(path);
    scores.Load();
    REQUIRE(scores.List().size() == 1);
    CHECK(scores.List().front().view == 0);
  }
}
