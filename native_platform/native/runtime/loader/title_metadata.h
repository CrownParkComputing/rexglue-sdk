#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "xex.h"

namespace xn {
struct AchievementInfo {
  uint32_t id = 0, score = 0, image_id = 0, flags = 0;
  std::string name, description, locked_description;
  uint64_t earned_at = 0; // FILETIME; zero means locked.
};
// Image pulled from XDBF section 2. The id field carries the XDBF
// image id. In rexglue-built XEXs (the convention this repo deals
// with) the layout is:
//   0x8000   = the game title tile / box art  (the one the launcher
//              picks for the rail)
//   0x1 .. ~0xd (per title) = achievement art; the achievement row's
//              image_id field is the lookup key
//   0x10000+, 0x20000+ = save icons / other ancillary images
// The Microsoft Xbox 360 SDK convention (1 = small tile, 2 = medium,
// 3 = box, 0x1000+ = achievement) does NOT apply to rexglue output -
// the ids have been remapped by the rebuild tool. The data is the PNG
// bytes as embedded.
struct TitleImage {
  uint64_t id = 0;
  std::vector<uint8_t> png;
};
struct TitleMetadata {
  std::string name;
  std::string description;              // XDBF string id 0x8001 (may be empty)
  std::string help;                      // XDBF string id 0x8002 (may be empty)
  std::vector<AchievementInfo> achievements;
  std::vector<TitleImage> images;        // tile + achievement art
  std::unordered_map<uint64_t, std::vector<uint8_t>> image_lookup;
};
// Read the title's own XDBF resource. Never trust another port's copied metadata.
TitleMetadata ReadTitleMetadata(const XexImage& image);
}
