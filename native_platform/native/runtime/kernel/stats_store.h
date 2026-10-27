// Local leaderboard rows (XUserReadStats / XGI app 0xFB).
//
// Geometry Wars keeps its high score on the Xbox LIVE leaderboard, not in the
// profile: the only profile setting it ever writes is its 12-byte options
// blob. So "the score survives a restart" needs a store behind the stats seam,
// not the profile one. This is deliberately not a Live implementation - there
// is no network, no ranking against other players and no friends list. It is
// one row per (title, view, user): the best rating that title has submitted.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>

namespace xn {

class StatsStore {
 public:
  void Open(const std::filesystem::path& file);
  // The best rating stored for this view, or false when the user has never
  // been ranked on it.
  bool Get(uint32_t title_id, uint32_t view_id, uint64_t& rating);
  // Keeps the higher of the stored and submitted rating, the way a leaderboard
  // does; returns true when the row changed.
  bool Put(uint32_t title_id, uint32_t view_id, uint64_t rating);

 private:
  void Save();
  std::filesystem::path file_;
  std::mutex mu_;
  std::map<uint64_t, uint64_t> rows_;  // (title_id << 32 | view_id) -> rating
};

StatsStore& Stats();

}  // namespace xn
