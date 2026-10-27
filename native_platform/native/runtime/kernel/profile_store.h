// Persistent per-title profile settings (XamUser{Read,Write}ProfileSettings).
// Geometry Wars keeps its local high score here; Live leaderboards are
// intentionally not implemented (see imports_xam.cpp: stats enumerator and
// sessions answer "not found").
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

namespace xn {

struct ProfileSetting {
  uint8_t type = 0;            // XUSER_DATA_TYPE: 1 int32, 2 int64, 3 double, 4 wstring, 5 float, 6 binary, 7 datetime
  uint64_t scalar = 0;         // int32/int64/double/float/datetime bits
  std::vector<uint8_t> blob;   // wstring/binary bytes (guest byte order)
};

class ProfileStore {
 public:
  void Open(const std::filesystem::path& file);
  bool Get(uint32_t title_id, uint32_t id, ProfileSetting& out);
  void Put(uint32_t title_id, uint32_t id, const ProfileSetting& s);

 private:
  void Save();
  std::filesystem::path file_;
  std::mutex mu_;
  std::map<uint64_t, ProfileSetting> settings_;  // (title_id << 32 | id)
};

ProfileStore& Profile();

// A profile setting id packs { id:14, unk:2, size:12, type:4 }. Ids with
// 0x3F00 set in the id field are the title's own (XPROFILE_TITLE_SPECIFIC1..3
// and friends); everything else is a console-wide gamer setting that belongs
// to the profile, not to a title. The emulator keys them the same way
// (UserProfile::Setting::is_title_specific), and titles rely on the
// distinction: a title-specific setting reads back "from title" (2), a
// console one reads back "from profile" (1).
inline bool IsTitleSpecificSetting(uint32_t id) { return (id & 0x3F00) == 0x3F00; }

// The console-wide settings a signed-in profile always has, with the same
// values the emulator's UserProfile hands out, so both runtimes answer a title
// identically. Returns false for ids the console does not know (including
// title-specific ones, which are only ever "set" once the title writes them).
//
// This matters: Geometry Wars 2 reads XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
// (0x10040024) at boot and puts up "The game was unable to load your profile"
// if it comes back unset. Geometry Wars 1 reads the same id and tolerates it.
bool DefaultProfileSetting(uint32_t id, ProfileSetting& out);

}  // namespace xn
