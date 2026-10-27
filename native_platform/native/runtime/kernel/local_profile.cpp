#include "local_profile.h"
#include <algorithm>
#include <sqlite3.h>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <rex/logging.h>

namespace xn {
namespace {
struct Statement {
  sqlite3_stmt* s = nullptr;
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
  }
  ~Statement() { sqlite3_finalize(s); }
  void Int(int p, int64_t v) { sqlite3_bind_int64(s, p, v); }
  void Text(int p, const std::string& v) { sqlite3_bind_text(s, p, v.c_str(), -1, SQLITE_TRANSIENT); }
  bool Row() { int r = sqlite3_step(s); if (r != SQLITE_ROW && r != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(s))); return r == SQLITE_ROW; }
  std::string Text(int c) { const auto* v = sqlite3_column_text(s, c); return v ? reinterpret_cast<const char*>(v) : ""; }
  int64_t Int(int c) { return sqlite3_column_int64(s, c); }
};
void Exec(sqlite3* db, const char* sql) {
  if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}
}
LocalProfile& LocalUser() { static LocalProfile profile; return profile; }
std::filesystem::path ProfileRoot() {
  if (const char* override = std::getenv("XN_PROFILE_ROOT"); override && *override) return override;
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return std::filesystem::path(xdg) / "xenon-native";
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home ? home : ".") / ".local/share/xenon-native";
}
LocalProfile::~LocalProfile() { if (db_) sqlite3_close(db_); }
std::string LocalProfile::gamertag() const {
  std::lock_guard lock(mutex_);
  return gamertag_;
}
bool LocalProfile::SetGamertag(const std::string& name, std::string& error) {
  if (name.empty() || name.size()>15 || name.front()==' ' || name.back()==' ' ||
      std::any_of(name.begin(),name.end(),[](unsigned char c) { return c<32 || c>126; })) {
    error="Use 1–15 printable characters without leading or trailing spaces.";
    return false;
  }
  std::lock_guard lock(mutex_);
  if (!db_) { error="Local profile is not open."; return false; }
  try {
    Statement update(db_,"UPDATE identity SET gamertag=? WHERE singleton=1");
    update.Text(1,name); update.Row();
    gamertag_=name;
    return true;
  } catch (const std::exception& e) { error=e.what(); return false; }
}
bool LocalProfile::Open(const std::filesystem::path& root, std::string& error) {
  std::lock_guard lock(mutex_);
  try {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
    std::filesystem::create_directories(root);
    if (sqlite3_open((root / "profile.sqlite3").c_str(), &db_) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db_));
    sqlite3_busy_timeout(db_, 5000);
    Exec(db_, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;");
    Exec(db_, R"sql(
      CREATE TABLE IF NOT EXISTS identity(singleton INTEGER PRIMARY KEY CHECK(singleton=1), xuid TEXT NOT NULL, gamertag TEXT NOT NULL);
      INSERT OR IGNORE INTO identity VALUES(1,'E03A2B7C0F4D1234','Player');
      CREATE TABLE IF NOT EXISTS titles(id INTEGER PRIMARY KEY, name TEXT NOT NULL, save_dir TEXT NOT NULL);
      CREATE TABLE IF NOT EXISTS achievements(title_id INTEGER NOT NULL REFERENCES titles(id), id INTEGER NOT NULL,
        name TEXT NOT NULL, description TEXT NOT NULL, locked_description TEXT NOT NULL, score INTEGER NOT NULL,
        image_id INTEGER NOT NULL, flags INTEGER NOT NULL, PRIMARY KEY(title_id,id));
      CREATE TABLE IF NOT EXISTS unlocks(title_id INTEGER NOT NULL, achievement_id INTEGER NOT NULL, earned_at INTEGER NOT NULL,
        PRIMARY KEY(title_id,achievement_id), FOREIGN KEY(title_id,achievement_id) REFERENCES achievements(title_id,id));
      PRAGMA user_version=1;
    )sql");
    Statement s(db_, "SELECT xuid,gamertag FROM identity WHERE singleton=1");
    if (!s.Row()) throw std::runtime_error("Local profile identity missing");
    xuid_ = std::stoull(s.Text(0), nullptr, 16); gamertag_ = s.Text(1);
    return true;
  } catch (const std::exception& e) { error = e.what(); return false; }
}
bool LocalProfile::RegisterTitle(const XexImage& image, const std::filesystem::path& saves, std::string& error) {
  auto metadata = ReadTitleMetadata(image);
  std::lock_guard lock(mutex_);
  if (!db_) { error = "Local profile is not open"; return false; }
  try {
    Exec(db_, "BEGIN IMMEDIATE");
    Statement title(db_, "INSERT INTO titles VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET name=excluded.name,save_dir=excluded.save_dir");
    title.Int(1, image.title_id); title.Text(2, metadata.name); title.Text(3, saves.string()); title.Row();
    for (const auto& a : metadata.achievements) {
      Statement row(db_, "INSERT INTO achievements VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(title_id,id) DO UPDATE SET name=excluded.name,description=excluded.description,locked_description=excluded.locked_description,score=excluded.score,image_id=excluded.image_id,flags=excluded.flags");
      row.Int(1, image.title_id); row.Int(2, a.id); row.Text(3, a.name); row.Text(4, a.description);
      row.Text(5, a.locked_description); row.Int(6, a.score); row.Int(7, a.image_id); row.Int(8, a.flags); row.Row();
    }
    Exec(db_, "COMMIT");
    return true;
  } catch (const std::exception& e) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); error = e.what(); return false; }
}
bool LocalProfile::Unlock(uint32_t title, uint32_t id, uint64_t filetime) {
  std::lock_guard lock(mutex_);
  if (!db_ || !filetime) return false;
  try {
    Statement s(db_, "INSERT OR IGNORE INTO unlocks SELECT title_id,id,? FROM achievements WHERE title_id=? AND id=?");
    s.Int(1, filetime); s.Int(2, title); s.Int(3, id); s.Row();
    if (sqlite3_changes(db_)) REXKRNL_INFO("Achievement unlocked: title {:08X} id {}", title, id);
    // Repeated awards succeed without changing the original time or score.
    Statement found(db_, "SELECT 1 FROM unlocks WHERE title_id=? AND achievement_id=?");
    found.Int(1, title); found.Int(2, id);
    return found.Row();
  } catch (const std::exception& e) { REXKRNL_ERROR("Achievement persistence: {}", e.what()); return false; }
}
std::vector<AchievementInfo> LocalProfile::Achievements(uint32_t title) {
  std::lock_guard lock(mutex_);
  std::vector<AchievementInfo> out;
  if (!db_) return out;
  Statement s(db_, "SELECT a.id,a.name,a.description,a.locked_description,a.score,a.image_id,a.flags,COALESCE(u.earned_at,0) FROM achievements a LEFT JOIN unlocks u ON u.title_id=a.title_id AND u.achievement_id=a.id WHERE a.title_id=? ORDER BY a.id");
  s.Int(1, title);
  while (s.Row()) {
    AchievementInfo a; a.id=s.Int(0); a.name=s.Text(1); a.description=s.Text(2); a.locked_description=s.Text(3);
    a.score=s.Int(4); a.image_id=s.Int(5); a.flags=s.Int(6); a.earned_at=s.Int(7); out.push_back(std::move(a));
  }
  return out;
}
uint32_t LocalProfile::Gamerscore(uint32_t title) {
  std::lock_guard lock(mutex_);
  if (!db_) return 0;
  Statement s(db_, "SELECT COALESCE(SUM(a.score),0) FROM achievements a JOIN unlocks u ON u.title_id=a.title_id AND u.achievement_id=a.id WHERE ?=0 OR a.title_id=?");
  s.Int(1, title); s.Int(2, title);
  return s.Row() ? uint32_t(s.Int(0)) : 0;
}
uint32_t LocalProfile::EarnedCount(uint32_t title) {
  std::lock_guard lock(mutex_);
  if (!db_) return 0;
  Statement s(db_, "SELECT COUNT(*) FROM unlocks WHERE ?=0 OR title_id=?");
  s.Int(1, title); s.Int(2, title);
  return s.Row() ? uint32_t(s.Int(0)) : 0;
}
}
