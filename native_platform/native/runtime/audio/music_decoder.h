// Streaming file audio, converted to the runtime's 48 kHz stereo float format.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace xn::audio {
class MusicDecoder {
 public:
  MusicDecoder();
  ~MusicDecoder();
  bool Open(const std::string& path, std::string& error);
  void Close();
  // Returns frames read; zero is EOF or a decode error (reported in error).
  uint32_t Read(float* stereo, uint32_t frames, std::string& error);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace xn::audio
