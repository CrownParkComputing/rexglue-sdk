// Native XMP service. Message layouts follow the XDK wrappers in the generated
// title and the Xenia XMP protocol research; no emulator player is used.
#include "kernel/xmp.h"
#include "kernel/kernel.h"
#include "audio/music_decoder.h"
#include "audio/sdl_output.h"
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <random>

namespace xn {
namespace {
constexpr uint32_t kInvalid = 0x80070057, kFailed = 0x80004005;
constexpr uint32_t kStateChanged = 0x0A000001, kBehaviorChanged = 0x0A000002;
constexpr uint32_t kPlaylistChanged = 0x8A000005;
constexpr uint32_t kSamples = audio::kFrameSamples, kBytes = kSamples * 2 * sizeof(float);
struct Song {
  uint32_t handle = 0, track = 0, duration = 0, format = 0;
  std::filesystem::path path;
  std::array<std::u16string, 5> metadata;
};
struct Playlist { std::vector<Song> songs; };
struct Player {
  std::mutex mutex;
  std::map<uint32_t, Playlist> playlists;  // SDK handle is the caller's storage pointer
  audio::MusicDecoder decoder;
  uint32_t next_song = 1, playlist = 0, index = 0, state = 0;
  uint32_t mode = 0, repeat = 0, flags = 0;
  uint32_t callback = 0, context = 0, capture_ring = 0, capture_index = 0;
  bool title_render = false, shutdown = false;
  float volume = 1;
  std::array<float, kSamples * 2> frame{};
  bool mix_frame = false;
  std::mt19937 random{0x584D50};

  void State(uint32_t value) {
    if (state == value) return;
    state = value;
    BroadcastNotification(kStateChanged, state);
  }
  bool Start(uint32_t handle, uint32_t song_index) {
    auto it = playlists.find(handle);
    if (it == playlists.end() || song_index >= it->second.songs.size()) return false;
    const auto& song = it->second.songs[song_index];
    std::string error;
    if (!decoder.Open(song.path.string(), error)) {
      REXLOG_WARN("XMP: cannot play '{}': {}", song.path.string(), error);
      State(0);
      return false;
    }
    playlist = handle;
    index = song_index;
    State(1);
    REXLOG_INFO("XMP: playing '{}' (song {}, playlist {:#x})", song.path.filename().string(), song.handle, handle);
    return true;
  }
  bool Advance(bool previous, bool automatic) {
    auto it = playlists.find(playlist);
    if (it == playlists.end() || it->second.songs.empty()) return false;
    uint32_t count = uint32_t(it->second.songs.size());
    if (automatic && repeat == 1 && index + 1 == count) { decoder.Close(); State(0); return false; }
    uint32_t next = previous ? (index + count - 1) % count : (index + 1) % count;
    if (mode == 1 && count > 1 && !previous)
      next = (index + 1 + std::uniform_int_distribution<uint32_t>(0, count - 2)(random)) % count;
    return Start(playlist, next);
  }
};
Player& P() { static Player player; return player; }

bool ReadString(uint32_t address, std::u16string& value) {
  value.clear();
  if (!address) return true;
  for (uint32_t i = 0; i < 1024; ++i) {
    if (uint64_t(address) + i * 2 + 2 > 0x100000000ull) return false;
    char16_t c = Load16(address + i * 2);
    if (!c) return true;
    value += c;
  }
  return false;
}
void WriteString(uint32_t address, const std::u16string& value) {
  for (size_t i = 0; i < std::min(value.size(), size_t(39)); ++i) Store16(address + uint32_t(i * 2), value[i]);
}
}

uint32_t XmpMessage(uint32_t message, uint32_t buffer, uint32_t length) {
  auto& p = P();
  std::lock_guard lock(p.mutex);
  if (p.shutdown) return kFailed;
  auto valid = [&](uint32_t bytes) {
    return buffer && (!length || length >= bytes) && uint64_t(buffer) + bytes <= 0x100000000ull;
  };
  auto arg = [&](uint32_t offset) { return Load32(buffer + offset); };
  switch (message) {
    case 0x7002E: {  // GetTitlePlaylistBufferSize
      if (!valid(12) || !arg(8) || !arg(4) || arg(4) > 1024) return kInvalid;
      Store32(arg(8), 0x88 + arg(4) * 0x3E8);
      return 0;
    }
    case 0x7000D: {  // CreateTitlePlaylist; deep-copy stack descriptors and strings
      if (!valid(36) || !arg(32)) return kInvalid;
      uint32_t storage = arg(4), bytes = arg(8), songs = arg(12), count = arg(16);
      uint32_t handles = arg(28), output = arg(32);
      Store32(output, 0);
      if (!storage || !songs || !count || count > 1024 || bytes < 0x88 + count * 0x3E8 ||
          uint64_t(songs) + count * 36ull > 0x100000000ull ||
          (handles && uint64_t(handles) + count * 4ull > 0x100000000ull)) return kInvalid;
      Playlist list;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t descriptor = songs + i * 36;
        Song song;
        std::u16string path;
        if (!ReadString(Load32(descriptor), path) || path.empty()) return kInvalid;
        try {
          if (!ResolveHostPath(std::filesystem::path(path).string(), song.path)) return kInvalid;
        } catch (const std::filesystem::filesystem_error&) { return kInvalid; }
        for (uint32_t j = 0; j < 5; ++j)
          if (!ReadString(Load32(descriptor + 4 + j * 4), song.metadata[j])) return kInvalid;
        song.handle = p.next_song++;
        song.track = Load32(descriptor + 24);
        song.duration = Load32(descriptor + 28);
        song.format = Load32(descriptor + 32);
        list.songs.push_back(std::move(song));
      }
      if (p.playlists.contains(storage)) return kInvalid;
      if (handles)
        for (uint32_t i = 0; i < count; ++i) Store32(handles + i * 4, list.songs[i].handle);
      Store32(storage, storage);
      Store32(output, storage);
      p.playlists.emplace(storage, std::move(list));
      BroadcastNotification(kPlaylistChanged, 0);
      REXLOG_INFO("XMP: created playlist {:#x} with {} songs", storage, count);
      return 0;
    }
    case 0x70002: {  // PlayTitlePlaylist
      if (!valid(12)) return kInvalid;
      auto it = p.playlists.find(arg(4));
      if (it == p.playlists.end()) return kInvalid;
      uint32_t index = 0;
      if (arg(8)) {
        auto song = std::find_if(it->second.songs.begin(), it->second.songs.end(),
            [&](const Song& song) { return song.handle == arg(8); });
        if (song == it->second.songs.end()) return kInvalid;
        index = uint32_t(song - it->second.songs.begin());
      }
      return p.Start(arg(4), index) ? 0 : kFailed;
    }
    case 0x70003:  // Continue
      if (!valid(4)) return kInvalid;
      if (p.state == 2) p.State(1);
      else if (p.state == 0 && p.playlist) return p.Start(p.playlist, p.index) ? 0 : kFailed;
      return 0;
    case 0x70004:  // Stop
      if (!valid(8)) return kInvalid;
      p.decoder.Close(); p.State(0); p.frame.fill(0);
      return 0;
    case 0x70005:  // Pause
      if (!valid(4)) return kInvalid;
      if (p.state == 1) p.State(2);
      return 0;
    case 0x70006: case 0x70007:  // Next / Previous
      if (!valid(4)) return kInvalid;
      return p.Advance(message == 0x70007, false) ? 0 : kFailed;
    case 0x70008:  // SetPlaybackBehavior
      if (!valid(16) || arg(4) > 1 || arg(8) > 1 || arg(12) > 1) return kInvalid;
      p.mode = arg(4); p.repeat = arg(8); p.flags = arg(12);
      BroadcastNotification(kBehaviorChanged, 0);
      return 0;
    case 0x70009:  // GetStatus
      if (!valid(8) || !arg(4)) return kInvalid;
      Store32(arg(4), p.state);
      return 0;
    case 0x7000B:  // GetVolume
      if (!valid(8) || !arg(4)) return kInvalid;
      Store32(arg(4), std::bit_cast<uint32_t>(p.volume));
      return 0;
    case 0x7000C: {  // SetVolume
      if (!valid(8)) return kInvalid;
      float volume = std::bit_cast<float>(arg(4));
      if (!std::isfinite(volume) || volume < 0 || volume > 1) return kInvalid;
      p.volume = volume;
      return 0;
    }
    case 0x7000E: {  // GetCurrentSong
      if (!valid(12) || !arg(8)) return kInvalid;
      uint32_t output = arg(8);
      std::memset(G(output), 0, 0x3E0);
      auto it = p.playlists.find(p.playlist);
      if (it == p.playlists.end() || p.state == 0) return kFailed;
      const Song& song = it->second.songs[p.index];
      Store32(output, song.handle);
      for (uint32_t j = 0; j < 5; ++j) WriteString(output + 0x240 + j * 80, song.metadata[j]);
      Store32(output + 0x3D0, song.track);
      Store32(output + 0x3D4, song.duration);
      Store32(output + 0x3D8, song.format);
      return 0;
    }
    case 0x70013:  // DeleteTitlePlaylist
      if (!valid(8) || !p.playlists.contains(arg(4))) return kInvalid;
      if (p.playlist == arg(4)) { p.decoder.Close(); p.State(0); p.playlist = 0; p.frame.fill(0); }
      p.playlists.erase(arg(4));
      BroadcastNotification(kPlaylistChanged, 0);
      return 0;
    case 0x7001A:  // Only the game's controller exists in this offline runtime.
      if (!valid(12) || arg(4) != 0 || arg(8) != 0) return kInvalid;
      return 0;
    case 0x7001B:  // GetPlaybackController (also polled by GW1/GW2)
      if (!valid(12)) return kInvalid;
      if (arg(4)) Store32(arg(4), 0);
      if (arg(8)) Store32(arg(8), 0);
      return 0;
    case 0x70029:  // GetPlaybackBehavior
      if (!valid(16) || !arg(4) || !arg(8) || !arg(12)) return kInvalid;
      Store32(arg(4), p.mode); Store32(arg(8), p.repeat); Store32(arg(12), p.flags);
      return 0;
    case 0x7003D:  // CaptureOutput: 256 interleaved stereo big-endian floats.
      if (!valid(16)) return kInvalid;
      if (arg(4) && !p.capture_ring) {
        p.capture_ring = K().arena.SystemAlloc(kBytes * 16, 16);
        if (!p.capture_ring) return kFailed;
      }
      p.callback = arg(4); p.context = arg(8); p.title_render = arg(12) != 0;
      REXLOG_INFO("XMP: capture {:#x}, context {:#x}, title renders {}", p.callback, p.context, p.title_render);
      return 0;
    default:
      REXLOG_WARN("XMP: unsupported message {:#x}", message);
      return 0x80004001;  // E_NOTIMPL, never claim unwritten output succeeded.
  }
}

XmpCapture XmpPrepareFrame() {
  auto& p = P();
  uint32_t callback = 0, context = 0, buffer = 0;
  {
    std::lock_guard lock(p.mutex);
    p.frame.fill(0);
    p.mix_frame = false;
    if (p.shutdown || p.state != 1) return {};
    uint32_t frames = 0;
    // Each successful Start is primed with samples; bound transitions as a
    // further guard against pathological files/empty playlists on the audio thread.
    for (uint32_t attempts = 0; frames < kSamples && attempts < kSamples; ++attempts) {
      std::string error;
      uint32_t read = p.decoder.Read(p.frame.data() + frames * 2, kSamples - frames, error);
      frames += read;
      if (!error.empty()) {
        REXLOG_WARN("XMP: decode failed: {}", error);
        p.decoder.Close(); p.State(0); break;
      }
      if (frames == kSamples || !p.Advance(false, true)) break;
    }
    for (auto& sample : p.frame) sample *= p.volume;
    callback = p.callback; context = p.context;
    p.mix_frame = !callback || !p.title_render;
    if (callback) {
      buffer = p.capture_ring + (p.capture_index++ % 16) * kBytes;
      for (uint32_t i = 0; i < kSamples * 2; ++i)
        Store32(buffer + i * 4, std::bit_cast<uint32_t>(p.frame[i]));
    }
  }
  // Never hold the player lock across guest code: the callback may call XMP.
  return {callback, context, buffer};
}

void XmpMixFrame(float* stereo, uint32_t frames) {
  auto& p = P();
  std::lock_guard lock(p.mutex);
  if (!p.mix_frame) return;
  for (uint32_t i = 0; i < std::min(frames, kSamples) * 2; ++i)
    stereo[i] = std::clamp(stereo[i] + p.frame[i] * 0.70710678f, -1.0f, 1.0f);
  p.mix_frame = false;
}

void XmpShutdown() {
  auto& p = P();
  std::lock_guard lock(p.mutex);
  p.shutdown = true;
  p.decoder.Close();
  p.playlists.clear();
  p.callback = 0; p.state = 0;
  if (p.capture_ring) { K().arena.SystemFree(p.capture_ring); p.capture_ring = 0; }
}
}  // namespace xn
