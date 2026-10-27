// rex::tee::Channel: lines land in REX_TEE_LOG per frame and key, keys are
// sorted, the same bytes give the same hash and different bytes do not.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <rex/system/tee.h>

int main() {
  namespace fs = std::filesystem;
  fs::path log = fs::temp_directory_path() / ("tee_test_" + std::to_string(::getpid()) + ".jsonl");
  setenv("REX_TEE_LOG", log.c_str(), 1);
  int failures = 0;
  auto expect = [&](bool c, const char* m) { if (!c) { std::fprintf(stderr, "FAIL %s\n", m); ++failures; } else std::printf("ok   %s\n", m); };

  expect(rex::tee::Enabled(), "enabled through REX_TEE_LOG");
  {
    rex::tee::Channel d3d("d3d");
    rex::tee::Channel state("state");
    uint32_t args[4] = {1, 2, 3, 4};
    d3d.Digest("draw", args, sizeof(args));
    d3d.Digest("draw", args, sizeof(args));
    d3d.DigestValue(rex::tee::CurrentFrame(), "clear", 0xFFu);
    state.Digest("rs", "abc", 3);
    rex::tee::EndFrame();  // frame 0 -> 1
    d3d.Digest("draw", args, sizeof(args));
    d3d.Digest("draw", args, sizeof(args));
    args[0] = 9;
    d3d.Digest("zz", args, sizeof(args));
    d3d.Digest("aa", args, sizeof(args));
    rex::tee::EndFrame();  // frame 1 -> 2
    expect(rex::tee::CurrentFrame() == 2, "EndFrame advances the shared frame");
  }
  rex::tee::Flush();
  std::ifstream in(log);
  std::vector<std::string> lines;
  for (std::string l; std::getline(in, l);) lines.push_back(l);
  for (auto& l : lines) std::printf("  %s\n", l.c_str());
  expect(lines.size() == 6, "six lines: 3 keys in frame 0, 3 in frame 1");
  expect(lines.size() >= 2 && lines[0].find("\"key\":\"d3d/clear\"") != std::string::npos &&
             lines[1].find("\"key\":\"d3d/draw\"") != std::string::npos, "keys sorted within a channel");
  auto hash_of = [&](const std::string& l) { auto p = l.find("\"hash\":\""); return l.substr(p + 8, 16); };
  std::string draw0, draw1;
  for (auto& l : lines) {
    if (l.find("\"frame\":0") != std::string::npos && l.find("d3d/draw") != std::string::npos) draw0 = hash_of(l);
    if (l.find("\"frame\":1") != std::string::npos && l.find("d3d/draw") != std::string::npos) draw1 = hash_of(l);
  }
  expect(!draw0.empty() && draw0 == draw1, "same bytes in two frames give the same hash");
  expect(lines.size() == 6 && lines[3].find("d3d/aa") != std::string::npos && lines[5].find("d3d/zz") != std::string::npos, "frame 1 keys sorted: aa, draw, zz");
  expect(lines.size() >= 2 && lines[1].find("\"n\":2") != std::string::npos, "n counts Digest calls per key");
  fs::remove(log);
  if (!failures) std::printf("tee_test: all passed\n");
  return failures ? 1 : 0;
}
