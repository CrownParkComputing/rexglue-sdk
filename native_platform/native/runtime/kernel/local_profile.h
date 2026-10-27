#pragma once
#include <filesystem>
#include <mutex>
#include <string>
#include "loader/title_metadata.h"
struct sqlite3;
namespace xn {
// One persistent local user, shared across every native conversion and launcher.
class LocalProfile {
 public:
  ~LocalProfile();
  bool Open(const std::filesystem::path& root, std::string& error);
  bool RegisterTitle(const XexImage& image, const std::filesystem::path& saves, std::string& error);
  bool Unlock(uint32_t title, uint32_t id, uint64_t filetime);
  std::vector<AchievementInfo> Achievements(uint32_t title);
  uint32_t Gamerscore(uint32_t title = 0);
  uint32_t EarnedCount(uint32_t title = 0);
  uint64_t xuid() const { return xuid_; }
  std::string gamertag() const;
  bool SetGamertag(const std::string& name, std::string& error);
 private:
  sqlite3* db_ = nullptr;
  mutable std::mutex mutex_;
  uint64_t xuid_ = 0xE03A2B7C0F4D1234ull;
  std::string gamertag_ = "Player";
};
std::filesystem::path ProfileRoot();
LocalProfile& LocalUser();
}
