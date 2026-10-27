#include "stats_store.h"

#include <cstdio>

#include <rex/logging.h>

namespace xn {

namespace {
constexpr uint32_t kMagic = 0x54534E58;  // "XNST"
}

StatsStore& Stats() {
  static StatsStore store;
  return store;
}

void StatsStore::Open(const std::filesystem::path& file) {
  std::lock_guard lock(mu_);
  file_ = file;
  rows_.clear();
  FILE* f = std::fopen(file.c_str(), "rb");
  if (!f) return;
  uint32_t magic = 0, count = 0;
  if (std::fread(&magic, 4, 1, f) != 1 || magic != kMagic || std::fread(&count, 4, 1, f) != 1) {
    std::fclose(f);
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint64_t key = 0, rating = 0;
    if (std::fread(&key, 8, 1, f) != 1 || std::fread(&rating, 8, 1, f) != 1) break;
    rows_[key] = rating;
  }
  std::fclose(f);
  REXKRNL_INFO("stats store {}: {} rows", file.string(), rows_.size());
}

void StatsStore::Save() {
  if (file_.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(file_.parent_path(), ec);
  std::filesystem::path tmp = file_;
  tmp += ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    REXKRNL_WARN("stats store: cannot write {}", tmp.string());
    return;
  }
  uint32_t count = static_cast<uint32_t>(rows_.size());
  std::fwrite(&kMagic, 4, 1, f);
  std::fwrite(&count, 4, 1, f);
  for (auto& [key, rating] : rows_) {
    std::fwrite(&key, 8, 1, f);
    std::fwrite(&rating, 8, 1, f);
  }
  std::fclose(f);
  std::filesystem::rename(tmp, file_, ec);
}

bool StatsStore::Get(uint32_t title_id, uint32_t view_id, uint64_t& rating) {
  std::lock_guard lock(mu_);
  auto it = rows_.find((uint64_t(title_id) << 32) | view_id);
  if (it == rows_.end()) return false;
  rating = it->second;
  return true;
}

bool StatsStore::Put(uint32_t title_id, uint32_t view_id, uint64_t rating) {
  std::lock_guard lock(mu_);
  uint64_t key = (uint64_t(title_id) << 32) | view_id;
  auto it = rows_.find(key);
  if (it != rows_.end() && it->second >= rating) return false;
  rows_[key] = rating;
  Save();
  return true;
}

}  // namespace xn
