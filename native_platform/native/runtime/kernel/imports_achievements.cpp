#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <rex/hook.h>
#include "kernel.h"
#include "local_profile.h"

using namespace xn;
namespace {
std::u16string Utf16(const std::string& text) {
  char* bytes = SDL_iconv_string("UTF-16LE", "UTF-8", text.c_str(), text.size() + 1);
  if (!bytes) return {};
  std::u16string out;
  const auto* p = reinterpret_cast<const uint8_t*>(bytes);
  while (p[0] || p[1]) { out.push_back(uint16_t(p[0]) | uint16_t(p[1]) << 8); p += 2; }
  SDL_free(bytes);
  return out;
}
struct Record {
  AchievementInfo info;
  std::u16string strings[3];
};
class AchievementEnumerator final : public Enumerator {
 public:
  std::vector<Record> records;
  uint32_t batch = 1, flags = 0, string_bytes = 0;
  size_t cursor = 0;
  std::mutex mutex;
  uint32_t BufferSize() const { return batch * (36 + string_bytes); }
  uint32_t Read(uint32_t buffer, uint32_t capacity, uint32_t& count) override {
    std::lock_guard lock(mutex);
    count = 0;
    if (cursor == records.size()) return 18;
    if (!buffer || capacity < BufferSize()) return X_ERROR_INSUFFICIENT_BUFFER;
    count = std::min(size_t(batch), records.size() - cursor);
    std::memset(G(buffer), 0, BufferSize());
    uint32_t strings = buffer + batch * 36;
    for (uint32_t i = 0; i < count; ++i) {
      const auto& r = records[cursor++];
      const auto& a = r.info;
      uint32_t row = buffer + i * 36;
      Store32(row, a.id);
      for (unsigned j = 0; j < 3; ++j) {
        if (!(flags & (1 << j))) continue;
        Store32(row + 4 + j * 4, strings);
        for (char16_t c : r.strings[j]) { Store16(strings, c); strings += 2; }
        Store16(strings, 0); strings += 2;
      }
      Store32(row + 16, a.image_id); Store32(row + 20, a.score);
      Store32(row + 24, uint32_t(a.earned_at)); Store32(row + 28, uint32_t(a.earned_at >> 32));
      // ACHIEVED only. Never claim the offline award was earned on Xbox Live.
      Store32(row + 32, a.flags | (a.earned_at ? 0x20000 : 0));
    }
    return X_ERROR_SUCCESS;
  }
};
}

u32 XamUserCreateAchievementEnumerator_entry(u32 title, u32 user, u64 xuid, u32 flags,
                                             u32 offset, u32 count, mapped_u32 size, mapped_u32 handle) {
  if (size) *size = 0;
  if (handle) *handle = 0;
  if (!size || !handle || !count || count > 10000) return X_ERROR_INVALID_PARAMETER;
  if (user != 0 || (xuid && xuid != LocalUser().xuid())) return X_ERROR_NO_SUCH_USER;
  auto e = std::make_shared<AchievementEnumerator>();
  e->batch = count; e->flags = flags; e->string_bytes = flags & 7 ? 464 : 0;
  auto achievements = LocalUser().Achievements(title ? uint32_t(title) : K().image.title_id);
  for (size_t i = offset; i < achievements.size(); ++i) {
    Record r{achievements[i], {Utf16(achievements[i].name), Utf16(achievements[i].description), Utf16(achievements[i].locked_description)}};
    uint32_t bytes = 0;
    for (unsigned j = 0; j < 3; ++j) if (flags & (1 << j)) bytes += uint32_t(r.strings[j].size() + 1) * 2;
    e->string_bytes = std::max(e->string_bytes, bytes);
    e->records.push_back(std::move(r));
  }
  *size = e->BufferSize(); *handle = K().objects.Add(e);
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamUserCreateAchievementEnumerator, XamUserCreateAchievementEnumerator_entry)
