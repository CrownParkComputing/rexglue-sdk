/**
 * @file        include/rex/audio/music_player.h
 * @brief       Native title-playlist (XMP) music: decode the title's own WMA
 *              soundtrack files and mix them into the device output.
 *
 * On a console, XMP - the dashboard's media player - plays a title's
 * soundtrack on its behalf: EA's titles (Burnout Revenge, the Need for Speed
 * line) ship their music as plain WMA files and hand XMP a playlist of them
 * rather than streaming through XAudio. Without a player behind XMP those
 * titles run silent apart from effects, and worse, a "system playback is
 * enabled" answer makes them mute their own music on purpose.
 *
 * This is that player: an ASF container reader, the WMA decoder FFmpeg
 * already provides for XMA, a resampler to the device rate, and one decode
 * thread ahead of a small ring. The kernel's XMP app owns the playlist logic
 * and the notifications; this owns the audio.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rex::audio {

struct MusicTrack {
  // Shown in logs only.
  std::string label;
  // Produces the whole file. Called on the player thread when the track
  // starts, so it may block on I/O; return false if the file cannot be read.
  std::function<bool(std::vector<uint8_t>& out)> load;
};

// Replaces the playlist and cues track start_index without playing it yet.
void MusicSetPlaylist(std::vector<MusicTrack> tracks, size_t start_index, bool repeat_playlist);
// Starts the cued track, or resumes after MusicPause.
void MusicPlay();
void MusicPause();
// Stops and forgets the position; the playlist stays cued at its first track.
void MusicStop();
void MusicNext();
void MusicPrevious();
// Linear gain applied at mix time, 0..1.
void MusicSetVolume(float volume);
// Invoked on the player thread when the last track of a non-repeating
// playlist has drained. The kernel turns it into the title's "stopped" state.
void MusicSetFinishedCallback(std::function<void()> on_playlist_finished);

bool MusicIsPlaying();
size_t MusicCurrentIndex();
uint32_t MusicPositionMs();

// Adds the music to channels 0 and 1 (front left/right) of a channel-sequential,
// big-endian float frame of `channels` x `channel_samples` at the device rate.
// Returns false when nothing was mixed, so the caller can keep the guest's
// frame untouched.
bool MusicMixInto(float* frame_be, size_t channel_samples, size_t channels);

}  // namespace rex::audio
