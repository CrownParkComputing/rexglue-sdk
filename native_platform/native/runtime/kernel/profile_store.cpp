#include "profile_store.h"
#include "local_profile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>
#include "kernel.h"

namespace xn {

namespace {
constexpr uint32_t kMagic = 0x584E5052;  // "XNPR"
}

ProfileStore& Profile() {
  static ProfileStore store;
  return store;
}

void ProfileStore::Open(const std::filesystem::path& file) {
  std::lock_guard lock(mu_);
  file_ = file;
  settings_.clear();
  FILE* f = std::fopen(file.c_str(), "rb");
  if (!f) return;
  uint32_t magic = 0, count = 0;
  if (std::fread(&magic, 4, 1, f) != 1 || magic != kMagic || std::fread(&count, 4, 1, f) != 1) {
    std::fclose(f);
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint64_t key;
    ProfileSetting s;
    uint32_t len = 0;
    if (std::fread(&key, 8, 1, f) != 1 || std::fread(&s.type, 1, 1, f) != 1 ||
        std::fread(&s.scalar, 8, 1, f) != 1 || std::fread(&len, 4, 1, f) != 1)
      break;
    s.blob.resize(len);
    if (len && std::fread(s.blob.data(), 1, len, f) != len) break;
    settings_[key] = std::move(s);
  }
  std::fclose(f);
  REXKRNL_INFO("profile store {}: {} settings", file.string(), settings_.size());
}

void ProfileStore::Save() {
  if (file_.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(file_.parent_path(), ec);
  std::filesystem::path tmp = file_;
  tmp += ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    REXKRNL_WARN("profile store: cannot write {}", tmp.string());
    return;
  }
  uint32_t count = static_cast<uint32_t>(settings_.size());
  std::fwrite(&kMagic, 4, 1, f);
  std::fwrite(&count, 4, 1, f);
  for (auto& [key, s] : settings_) {
    uint32_t len = static_cast<uint32_t>(s.blob.size());
    std::fwrite(&key, 8, 1, f);
    std::fwrite(&s.type, 1, 1, f);
    std::fwrite(&s.scalar, 8, 1, f);
    std::fwrite(&len, 4, 1, f);
    if (len) std::fwrite(s.blob.data(), 1, len, f);
  }
  std::fclose(f);
  std::filesystem::rename(tmp, file_, ec);
}

bool ProfileStore::Get(uint32_t title_id, uint32_t id, ProfileSetting& out) {
  std::lock_guard lock(mu_);
  auto it = settings_.find((uint64_t(title_id) << 32) | id);
  if (it == settings_.end()) return false;
  out = it->second;
  return true;
}

void ProfileStore::Put(uint32_t title_id, uint32_t id, const ProfileSetting& s) {
  std::lock_guard lock(mu_);
  settings_[(uint64_t(title_id) << 32) | id] = s;
  Save();
}

namespace {

ProfileSetting Int32(uint32_t v) { ProfileSetting s; s.type = 1; s.scalar = v; return s; }
ProfileSetting Float(float v) {
  ProfileSetting s;
  s.type = 5;
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  s.scalar = bits;
  return s;
}
// A guest wstring is UTF-16 big-endian including the terminator; an empty one
// reads back as cbData 0 / pbData 0 (the emulator does the same).
ProfileSetting Wstring(const char* ascii) {
  ProfileSetting s;
  s.type = 4;
  for (const char* p = ascii; *p; p++) { s.blob.push_back(0); s.blob.push_back(uint8_t(*p)); }
  if (!s.blob.empty()) { s.blob.push_back(0); s.blob.push_back(0); }
  return s;
}

}  // namespace

bool DefaultProfileSetting(uint32_t id, ProfileSetting& out) {
  switch (id) {
    case 0x10040002: out = Int32(0); return true;           // GAMER_YAXIS_INVERSION
    case 0x10040003: out = Int32(3); return true;           // OPTION_CONTROLLER_VIBRATION
    case 0x10040004: out = Int32(0); return true;           // GAMERCARD_ZONE
    case 0x10040005: out = Int32(0); return true;           // GAMERCARD_REGION
    case 0x10040006: out = Int32(LocalUser().Gamerscore()); return true; // GAMERCARD_CRED
    case 0x5004000B: out = Float(0.0f); return true;        // GAMERCARD_REP
    case 0x1004000C: out = Int32(0); return true;           // OPTION_VOICE_MUTED
    case 0x1004000D: out = Int32(0); return true;           // OPTION_VOICE_THRU_SPEAKERS
    case 0x1004000E: out = Int32(0x64); return true;        // OPTION_VOICE_VOLUME
    case 0x4064000F: out = Wstring("gamercard_picture_key"); return true;
    case 0x402C0011: out = Wstring(""); return true;        // GAMERCARD_MOTTO
    case 0x10040012: out = Int32(1); return true;           // GAMERCARD_TITLES_PLAYED
    case 0x10040013: out = Int32(LocalUser().EarnedCount()); return true;
    case 0x10040015: out = Int32(0); return true;           // GAMER_DIFFICULTY
    case 0x10040018: out = Int32(0); return true;           // GAMER_CONTROL_SENSITIVITY
    case 0x1004001D: out = Int32(0xFFFF0000u); return true;  // preferred colour 1
    case 0x1004001E: out = Int32(0xFF00FF00u); return true;  // preferred colour 2
    case 0x10040022: out = Int32(1); return true;           // GAMER_ACTION_AUTO_AIM
    case 0x10040023: out = Int32(0); return true;           // GAMER_ACTION_AUTO_CENTER
    case 0x10040024: out = Int32(0); return true;           // GAMER_ACTION_MOVEMENT_CONTROL
    case 0x10040026: out = Int32(0); return true;           // GAMER_RACE_TRANSMISSION
    case 0x10040027: out = Int32(0); return true;           // GAMER_RACE_CAMERA_LOCATION
    case 0x10040028: out = Int32(0); return true;           // GAMER_RACE_BRAKE_CONTROL
    case 0x10040029: out = Int32(0); return true;           // GAMER_RACE_ACCELERATOR_CONTROL
    case 0x10040038: out = Int32(LocalUser().Gamerscore(K().image.title_id)); return true;
    case 0x10040039: out = Int32(LocalUser().EarnedCount(K().image.title_id)); return true;
    default: return false;
  }
}

}  // namespace xn
