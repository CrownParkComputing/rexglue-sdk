// XDBF SPA layout follows ReXGlue/Xenia xdbf_utils (BSD-3-Clause; see notices).
#include "title_metadata.h"
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <span>

namespace xn {
namespace {
using Bytes = std::span<const uint8_t>;
uint16_t U16(Bytes s, size_t p) { return uint16_t(s[p]) << 8 | s[p+1]; }
uint32_t U32(Bytes s, size_t p) { return uint32_t(U16(s, p)) << 16 | U16(s, p+2); }
uint64_t U64(Bytes s, size_t p) { return uint64_t(U32(s, p)) << 32 | U32(s, p+4); }
Bytes Slice(Bytes s, size_t p, size_t n) { return p <= s.size() && n <= s.size() - p ? s.subspan(p, n) : Bytes{}; }
}
TitleMetadata ReadTitleMetadata(const XexImage& image) {
  TitleMetadata result;
  result.name = std::format("Title {:08X}", image.title_id);
  uint32_t offset = 0;
  if (!XexGetOptHeader(image.header.data(), image.header.size(), 0x2FF, &offset)) return result;
  auto header = Slice(image.header, offset, image.header.size() - std::min(size_t(offset), image.header.size()));
  if (header.size() < 4) return result;
  auto resources = Slice(header, 0, U32(header, 0));
  Bytes db;
  const std::string resource_name = std::format("{:08X}", image.title_id);
  for (size_t p = 4; p + 16 <= resources.size(); p += 16) {
    if (std::memcmp(resources.data() + p, resource_name.data(), 8)) continue;
    uint32_t address = U32(resources, p+8), size = U32(resources, p+12);
    if (address >= image.base_address) db = Slice(image.image, address - image.base_address, size);
  }
  if (db.size() < 24 || U32(db, 0) != 0x58444246) return result;
  size_t slots = U32(db, 8), used = U32(db, 12), free_slots = U32(db, 16);
  uint64_t start = 24ull + slots * 18ull + free_slots * 8ull;
  if (used > slots || start > db.size()) return result;
  auto entry = [&](uint16_t section, uint64_t id) -> Bytes {
    for (size_t i = 0; i < used; ++i) {
      size_t p = 24 + i * 18;
      if (U16(db, p) == section && U64(db, p+2) == id) return Slice(db, start + U32(db, p+10), U32(db, p+14));
    }
    return {};
  };
  Bytes strings = entry(3, 1); // English, falling back to the title's default.
  if (strings.empty()) {
    Bytes defaults = entry(1, 0x58535443);
    if (defaults.size() >= 16) strings = entry(3, U32(defaults, 12));
  }
  std::map<uint16_t, std::string> text;
  if (strings.size() >= 14 && U32(strings, 0) == 0x58535452) {
    size_t p = 14;
    for (unsigned i = 0; i < U16(strings, 12) && p + 4 <= strings.size(); ++i) {
      uint16_t id = U16(strings, p), length = U16(strings, p+2); p += 4;
      auto value = Slice(strings, p, length);
      if (value.size() != length) break;
      text[id] = std::string(reinterpret_cast<const char*>(value.data()), length);
      p += length;
    }
  }
  if (!text[0x8000].empty()) result.name = text[0x8000];

  // Parse the achievement rows first: the description heuristic below needs
  // to know which string ids are achievement text so it never picks one.
  Bytes achievements = entry(1, 0x58414348);
  const bool have_xach = achievements.size() >= 14 && U32(achievements, 0) == 0x58414348;
  std::set<uint16_t> achievement_strings;
  if (have_xach) {
    for (unsigned i = 0; i < U16(achievements, 12); ++i) {
      Bytes row = Slice(achievements, 14 + size_t(i) * 36, 36);
      if (row.empty()) break;
      AchievementInfo a;
      a.id = U16(row, 0); a.name = text[U16(row, 2)];
      a.description = text[U16(row, 4)]; a.locked_description = text[U16(row, 6)];
      a.image_id = U32(row, 8); a.score = U16(row, 12); a.flags = U32(row, 16) & ~0x30000u;
      achievement_strings.insert(U16(row, 2));
      achievement_strings.insert(U16(row, 4));
      achievement_strings.insert(U16(row, 6));
      result.achievements.push_back(std::move(a));
    }
  }

  // Description: 0x8001 is the XBLA convention, but the rexglue-rebuilt XEXs
  // ship it empty. The blurb then sits in the low marketplace block -
  // usually 0x0004 (verified on MCLA), but the block is not laid out
  // consistently (Banjo's blurb is at 0x0007, and some titles put help text
  // or achievement conditions at 0x0004). Take 0x8001 first, then 0x0004,
  // then the longest remaining candidate in the block - and only a string
  // that looks like marketing copy: long enough, not a "<...>" template
  // placeholder, not referenced by an achievement row.
  auto usable = [&](const std::string& v, uint16_t id) {
    return v.size() >= 48 && v[0] != '<' && !achievement_strings.count(id);
  };
  if (!text[0x8001].empty()) result.description = text[0x8001];
  if (result.description.empty() && text.count(0x0004) && usable(text[0x0004], 0x0004))
    result.description = text[0x0004];
  if (result.description.empty()) {
    for (const auto& [id, v] : text) {
      if (id < 0x0002 || id > 0x0009 || !usable(v, id)) continue;
      if (v.size() > result.description.size()) result.description = v;
    }
  }
  if (!text[0x8002].empty()) result.help = text[0x8002];
  // Images are independent of the achievement catalog. Titles without XACH
  // can still carry a dashboard tile (and must not lose it in the launcher).
  // Section 2: image resources, PNG bytes. In rexglue-built XEXs
  // (this repo), id 0x8000 carries the title tile / box art and the
  // low numbered ids (0x1-0xd, varies per title) carry achievement
  // art - the achievement row's image_id field is the lookup key. The
  // Microsoft Xbox 360 SDK convention does not apply; see TitleImage
  // in title_metadata.h for the full id map. Enumerate whatever the
  // XDBF actually carries - the launcher picks 0x8000, falling back
  // to the largest image not referenced by any achievement row.
  for (size_t i = 0; i < used; ++i) {
    size_t p = 24 + i * 18;
    if (U16(db, p) != 2) continue;
    uint64_t img_id = U64(db, p + 2);
    Bytes png = Slice(db, start + U32(db, p + 10), U32(db, p + 14));
    if (png.size() >= 8 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G') {
      TitleImage img;
      img.id = img_id;
      img.png.assign(png.begin(), png.end());
      result.image_lookup[img_id] = img.png;
      result.images.push_back(std::move(img));
    }
  }
  return result;
}
}
