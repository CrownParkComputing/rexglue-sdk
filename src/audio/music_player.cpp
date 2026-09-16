/**
 * @file        audio/music_player.cpp
 * @brief       Native title-playlist (XMP) music - see music_player.h.
 */
#include <rex/audio/music_player.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include <rex/logging.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"
}

namespace rex::audio {

namespace {

// ---------------------------------------------------------------- ASF reader

// Just enough of the ASF container to feed a WMA decoder: the header objects
// that describe the audio stream, then the fixed-size data packets, out of
// which the payloads for that one stream are pulled in order. WMA "frames"
// (superframes of block_align bytes) always arrive whole and in sequence
// through those payloads, so the reader hands back a byte stream and the
// decoder cuts it at block_align - the same thing a full demuxer ends up doing
// for this codec, without the object reassembly.
//
// Reference: "Advanced Systems Format (ASF) Specification", revision 01.20.05.

uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t Rd64(const uint8_t* p) { return uint64_t(Rd32(p)) | (uint64_t(Rd32(p + 4)) << 32); }

constexpr uint8_t kGuidHeader[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                     0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kGuidData[16] = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                   0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kGuidFileProperties[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                                             0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kGuidStreamProperties[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                               0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kGuidAudioMedia[16] = {0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11,
                                         0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};

bool GuidIs(const uint8_t* p, const uint8_t (&guid)[16]) { return std::memcmp(p, guid, 16) == 0; }

class AsfAudioReader {
 public:
  bool Open(const std::vector<uint8_t>& file, const std::string& label) {
    data_ = file.data();
    size_ = file.size();
    if (size_ < 30 || !GuidIs(data_, kGuidHeader)) {
      REXAPU_ERROR("music: {} is not an ASF file", label);
      return false;
    }
    const uint64_t header_size = Rd64(data_ + 16);
    const uint32_t object_count = Rd32(data_ + 24);
    if (header_size < 30 || header_size > size_) {
      return false;
    }
    size_t pos = 30;
    for (uint32_t i = 0; i < object_count && pos + 24 <= header_size; ++i) {
      const uint8_t* guid = data_ + pos;
      const uint64_t object_size = Rd64(data_ + pos + 16);
      if (object_size < 24 || pos + object_size > header_size) {
        break;
      }
      const uint8_t* body = data_ + pos + 24;
      const size_t body_size = size_t(object_size - 24);
      if (GuidIs(guid, kGuidFileProperties) && body_size >= 80) {
        packet_count_ = Rd64(body + 32);
        play_duration_100ns_ = Rd64(body + 40);
        preroll_ms_ = Rd64(body + 56);
        const uint32_t min_packet = Rd32(body + 68);
        const uint32_t max_packet = Rd32(body + 72);
        packet_size_ = max_packet;
        if (min_packet != max_packet) {
          REXAPU_WARN("music: {} has variable packet sizes ({}..{}); reading the maximum", label,
                      min_packet, max_packet);
        }
      } else if (GuidIs(guid, kGuidStreamProperties) && body_size >= 54 && !have_audio_ &&
                 GuidIs(body, kGuidAudioMedia)) {
        const uint32_t type_specific_length = Rd32(body + 40);
        stream_number_ = uint8_t(Rd16(body + 48) & 0x7F);
        if (type_specific_length < 18 || 54 + type_specific_length > body_size) {
          REXAPU_ERROR("music: {} has a malformed audio stream header", label);
          return false;
        }
        const uint8_t* wfx = body + 54;  // WAVEFORMATEX
        codec_tag_ = Rd16(wfx + 0);
        channels_ = Rd16(wfx + 2);
        sample_rate_ = Rd32(wfx + 4);
        avg_bytes_per_sec_ = Rd32(wfx + 8);
        block_align_ = Rd16(wfx + 12);
        bits_per_sample_ = Rd16(wfx + 14);
        const uint16_t cb_size = Rd16(wfx + 16);
        const size_t extra = std::min<size_t>(cb_size, type_specific_length - 18);
        extradata_.assign(wfx + 18, wfx + 18 + extra);
        have_audio_ = true;
      }
      pos += size_t(object_size);
    }
    if (!have_audio_ || !packet_size_ || !block_align_) {
      REXAPU_ERROR("music: {} has no usable audio stream", label);
      return false;
    }
    // The Data Object directly follows the Header Object.
    if (header_size + 50 > size_ || !GuidIs(data_ + header_size, kGuidData)) {
      REXAPU_ERROR("music: {} has no data object where one was expected", label);
      return false;
    }
    const uint64_t data_object_size = Rd64(data_ + header_size + 16);
    const uint64_t total_packets = Rd64(data_ + header_size + 40);
    packet_pos_ = size_t(header_size + 50);
    packets_left_ = total_packets ? total_packets : (data_object_size - 50) / packet_size_;
    return true;
  }

  // Appends the next packet's audio payload bytes. False at the end of the data.
  bool NextPacket(std::vector<uint8_t>& audio) {
    if (!packets_left_ || packet_pos_ + packet_size_ > size_) {
      return false;
    }
    const uint8_t* pkt = data_ + packet_pos_;
    packet_pos_ += packet_size_;
    --packets_left_;
    size_t pos = 0;
    auto avail = [&](size_t n) { return pos + n <= packet_size_; };
    auto read_var = [&](uint32_t type) -> uint32_t {
      switch (type) {
        case 1:
          if (!avail(1)) return 0;
          return pkt[pos++];
        case 2: {
          if (!avail(2)) return 0;
          uint32_t v = Rd16(pkt + pos);
          pos += 2;
          return v;
        }
        case 3: {
          if (!avail(4)) return 0;
          uint32_t v = Rd32(pkt + pos);
          pos += 4;
          return v;
        }
        default:
          return 0;
      }
    };
    if (!avail(2)) return true;
    // Optional error-correction block first.
    if (pkt[0] & 0x80) {
      const size_t ec_length = pkt[0] & 0x0F;
      pos = 1 + ec_length;
      if (!avail(2)) return true;
    }
    const uint8_t length_flags = pkt[pos++];
    const uint8_t property_flags = pkt[pos++];
    const bool multiple = length_flags & 0x01;
    const uint32_t sequence_type = (length_flags >> 1) & 3;
    const uint32_t padding_type = (length_flags >> 3) & 3;
    const uint32_t packet_length_type = (length_flags >> 5) & 3;
    const uint32_t replicated_type = property_flags & 3;
    const uint32_t offset_type = (property_flags >> 2) & 3;
    const uint32_t object_number_type = (property_flags >> 4) & 3;
    uint32_t packet_length = read_var(packet_length_type);
    (void)read_var(sequence_type);
    const uint32_t padding = read_var(padding_type);
    pos += 6;  // send time (u32 ms) + duration (u16 ms)
    if (!packet_length) {
      packet_length = packet_size_;
    }
    packet_length = std::min<uint32_t>(packet_length, packet_size_);
    uint32_t payload_count = 1;
    uint32_t payload_length_type = 0;
    if (multiple) {
      if (!avail(1)) return true;
      const uint8_t payload_flags = pkt[pos++];
      payload_count = payload_flags & 0x3F;
      payload_length_type = (payload_flags >> 6) & 3;
    }
    for (uint32_t i = 0; i < payload_count; ++i) {
      if (!avail(1)) break;
      const uint8_t stream = pkt[pos++] & 0x7F;
      (void)read_var(object_number_type);
      (void)read_var(offset_type);
      const uint32_t replicated = read_var(replicated_type);
      const bool wanted = stream == stream_number_;
      if (replicated == 1) {
        // Compressed payload: a run of whole media objects, each with a
        // one-byte size, after a one-byte presentation time delta.
        pos += 1;
        uint32_t length;
        if (multiple) {
          length = read_var(payload_length_type);
        } else {
          length = packet_length > padding + pos ? packet_length - padding - uint32_t(pos) : 0;
        }
        const size_t end = std::min<size_t>(pos + length, packet_size_);
        while (pos < end) {
          const size_t sub = pkt[pos++];
          const size_t take = std::min(sub, end - pos);
          if (wanted && take) audio.insert(audio.end(), pkt + pos, pkt + pos + take);
          pos += take;
        }
      } else {
        pos += replicated;
        uint32_t length;
        if (multiple) {
          length = read_var(payload_length_type);
        } else {
          length = packet_length > padding + pos ? packet_length - padding - uint32_t(pos) : 0;
        }
        const size_t take = std::min<size_t>(length, packet_size_ > pos ? packet_size_ - pos : 0);
        if (wanted && take) audio.insert(audio.end(), pkt + pos, pkt + pos + take);
        pos += take;
      }
    }
    return true;
  }

  uint16_t codec_tag() const { return codec_tag_; }
  uint16_t channels() const { return channels_; }
  uint32_t sample_rate() const { return sample_rate_; }
  uint32_t avg_bytes_per_sec() const { return avg_bytes_per_sec_; }
  uint16_t block_align() const { return block_align_; }
  const std::vector<uint8_t>& extradata() const { return extradata_; }
  uint64_t play_duration_100ns() const { return play_duration_100ns_; }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t packet_pos_ = 0;
  uint64_t packets_left_ = 0;
  uint32_t packet_size_ = 0;
  uint64_t packet_count_ = 0;
  uint64_t play_duration_100ns_ = 0;
  uint64_t preroll_ms_ = 0;
  bool have_audio_ = false;
  uint8_t stream_number_ = 0;
  uint16_t codec_tag_ = 0;
  uint16_t channels_ = 0;
  uint32_t sample_rate_ = 0;
  uint32_t avg_bytes_per_sec_ = 0;
  uint16_t block_align_ = 0;
  uint16_t bits_per_sample_ = 0;
  std::vector<uint8_t> extradata_;
};

// ------------------------------------------------------------- WMA decoder

class WmaDecoder {
 public:
  ~WmaDecoder() { Close(); }

  bool Open(const AsfAudioReader& asf, const std::string& label) {
    Close();
    AVCodecID id;
    switch (asf.codec_tag()) {
      case 0x0160:
        id = AV_CODEC_ID_WMAV1;
        break;
      case 0x0161:
        id = AV_CODEC_ID_WMAV2;
        break;
      case 0x0162:
        id = AV_CODEC_ID_WMAPRO;
        break;
      default:
        REXAPU_ERROR("music: {} uses codec tag {:#06x}, which this player does not decode", label,
                     asf.codec_tag());
        return false;
    }
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) {
      REXAPU_ERROR("music: FFmpeg has no decoder for codec tag {:#06x}", asf.codec_tag());
      return false;
    }
    context_ = avcodec_alloc_context3(codec);
    context_->sample_rate = int(asf.sample_rate());
    context_->channels = asf.channels();
    context_->channel_layout = asf.channels() == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    context_->block_align = asf.block_align();
    context_->bit_rate = int64_t(asf.avg_bytes_per_sec()) * 8;
    if (!asf.extradata().empty()) {
      context_->extradata = static_cast<uint8_t*>(
          av_mallocz(asf.extradata().size() + AV_INPUT_BUFFER_PADDING_SIZE));
      std::memcpy(context_->extradata, asf.extradata().data(), asf.extradata().size());
      context_->extradata_size = int(asf.extradata().size());
    }
    if (avcodec_open2(context_, codec, nullptr) < 0) {
      REXAPU_ERROR("music: could not open the decoder for {}", label);
      Close();
      return false;
    }
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    block_align_ = asf.block_align();
    return true;
  }

  void Close() {
    if (frame_) av_frame_free(&frame_);
    if (packet_) av_packet_free(&packet_);
    if (context_) avcodec_free_context(&context_);
  }

  // Feeds one block_align-sized chunk (or nullptr to flush) and collects the
  // decoded planar float samples through `sink(planes, channels, samples)`.
  template <typename Sink>
  bool Decode(const uint8_t* chunk, Sink&& sink) {
    int ret;
    if (chunk) {
      if (av_new_packet(packet_, block_align_) < 0) return false;
      std::memcpy(packet_->data, chunk, block_align_);
      ret = avcodec_send_packet(context_, packet_);
      av_packet_unref(packet_);
    } else {
      ret = avcodec_send_packet(context_, nullptr);
    }
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
      return false;
    }
    while ((ret = avcodec_receive_frame(context_, frame_)) == 0) {
      if (frame_->format == AV_SAMPLE_FMT_FLTP) {
        sink(reinterpret_cast<const float* const*>(frame_->extended_data), context_->channels,
             size_t(frame_->nb_samples));
      }
      av_frame_unref(frame_);
    }
    return true;
  }

 private:
  AVCodecContext* context_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  int block_align_ = 0;
};

// -------------------------------------------------------------- resampler

// Cubic Hermite interpolation, one instance per channel. Good enough for
// music at 44.1 -> 48 kHz; a proper windowed-sinc would be the next step if
// anyone hears the difference.
class Resampler {
 public:
  void Reset(double ratio_in_per_out) {
    step_ = ratio_in_per_out;
    history_.assign(3, 0.0f);
    pos_ = 1.0;
  }
  void Push(const float* in, size_t n) { history_.insert(history_.end(), in, in + n); }
  // Emits as many output samples as the buffered input allows.
  void Pull(std::vector<float>& out) {
    while (pos_ + 2.0 < double(history_.size())) {
      const size_t i = size_t(pos_);
      const float t = float(pos_ - double(i));
      const float x0 = history_[i - 1], x1 = history_[i], x2 = history_[i + 1], x3 = history_[i + 2];
      const float c1 = 0.5f * (x2 - x0);
      const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
      const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
      out.push_back(((c3 * t + c2) * t + c1) * t + x1);
      pos_ += step_;
    }
    // Drop what can no longer be referenced, keeping one sample of history.
    const size_t keep_from = size_t(pos_) > 1 ? size_t(pos_) - 1 : 0;
    if (keep_from > 0) {
      history_.erase(history_.begin(), history_.begin() + keep_from);
      pos_ -= double(keep_from);
    }
  }

 private:
  double step_ = 1.0;
  double pos_ = 1.0;
  std::vector<float> history_;
};

// ----------------------------------------------------------------- player

constexpr uint32_t kDeviceRate = 48000;
constexpr size_t kRingFrames = kDeviceRate * 2;  // two seconds of stereo

enum class State { kStopped, kPlaying, kPaused };

class Player {
 public:
  static Player& Get() {
    static Player instance;
    return instance;
  }

  void SetPlaylist(std::vector<MusicTrack> tracks, size_t start, bool repeat) {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_ = std::move(tracks);
    index_ = tracks_.empty() ? 0 : std::min(start, tracks_.size() - 1);
    repeat_ = repeat;
    state_ = State::kStopped;
    ++generation_;
    ClearRingLocked();
    cv_.notify_all();
  }
  void Play() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::kPaused) {
      state_ = State::kPlaying;
    } else if (state_ == State::kStopped && !tracks_.empty()) {
      state_ = State::kPlaying;
      ++generation_;
      ClearRingLocked();
      EnsureThreadLocked();
    }
    cv_.notify_all();
  }
  void Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::kPlaying) state_ = State::kPaused;
  }
  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::kStopped;
    ++generation_;
    ClearRingLocked();
    cv_.notify_all();
  }
  void Skip(int delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tracks_.empty()) return;
    const size_t n = tracks_.size();
    index_ = size_t((int(index_) + delta + int(n)) % int(n));
    ++generation_;
    ClearRingLocked();
    if (state_ == State::kPaused) state_ = State::kPlaying;
    cv_.notify_all();
  }
  void SetVolume(float v) { volume_.store(std::clamp(v, 0.0f, 1.0f)); }
  void SetFinishedCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_finished_ = std::move(cb);
  }
  bool IsPlaying() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kPlaying;
  }
  size_t CurrentIndex() {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_;
  }
  uint32_t PositionMs() {
    return uint32_t(consumed_frames_.load() * 1000ull / kDeviceRate);
  }

  bool MixInto(float* frame_be, size_t channel_samples, size_t channels) {
    if (channels < 2) return false;
    std::vector<float> pulled;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_ != State::kPlaying || ring_count_ == 0) {
        return false;
      }
      const size_t frames = std::min(channel_samples, ring_count_);
      pulled.resize(frames * 2);
      for (size_t i = 0; i < frames * 2; ++i) {
        pulled[i] = ring_[ring_read_];
        ring_read_ = (ring_read_ + 1) % (kRingFrames * 2);
      }
      ring_count_ -= frames;
      consumed_frames_ += frames;
      cv_.notify_all();
    }
    const float gain = volume_.load();
    const size_t frames = pulled.size() / 2;
    auto mix = [&](float* channel, size_t i, float sample) {
      uint32_t bits;
      std::memcpy(&bits, &channel[i], 4);
      bits = __builtin_bswap32(bits);
      float value;
      std::memcpy(&value, &bits, 4);
      value = std::clamp(value + sample * gain, -1.0f, 1.0f);
      std::memcpy(&bits, &value, 4);
      bits = __builtin_bswap32(bits);
      std::memcpy(&channel[i], &bits, 4);
    };
    for (size_t i = 0; i < frames; ++i) {
      mix(frame_be, i, pulled[i * 2]);
      mix(frame_be + channel_samples, i, pulled[i * 2 + 1]);
    }
    return true;
  }

 private:
  Player() = default;

  void ClearRingLocked() {
    ring_read_ = ring_write_ = ring_count_ = 0;
    consumed_frames_ = 0;
  }
  void EnsureThreadLocked() {
    if (!thread_.joinable()) {
      thread_ = std::thread([this] { Run(); });
      thread_.detach();
    }
  }

  // Blocks until `frames` stereo frames fit, or the generation changes.
  bool PushFrames(const std::vector<float>& left, const std::vector<float>& right, uint64_t gen) {
    size_t offset = 0;
    const size_t total = std::min(left.size(), right.size());
    while (offset < total) {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return generation_ != gen || ring_count_ < kRingFrames; });
      if (generation_ != gen) return false;
      const size_t room = kRingFrames - ring_count_;
      const size_t n = std::min(room, total - offset);
      for (size_t i = 0; i < n; ++i) {
        ring_[ring_write_] = left[offset + i];
        ring_[(ring_write_ + 1) % (kRingFrames * 2)] = right[offset + i];
        ring_write_ = (ring_write_ + 2) % (kRingFrames * 2);
      }
      ring_count_ += n;
      offset += n;
    }
    return true;
  }

  void Run() {
    ring_.assign(kRingFrames * 2, 0.0f);
    for (;;) {
      MusicTrack track;
      uint64_t gen;
      size_t index;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return state_ == State::kPlaying && !tracks_.empty(); });
        gen = generation_;
        index = index_;
        track = tracks_[index_];
      }
      PlayTrack(track, gen, index);
    }
  }

  // Decodes one track into the ring; returns when it ends or is interrupted.
  void PlayTrack(const MusicTrack& track, uint64_t gen, size_t index) {
    std::vector<uint8_t> file;
    if (!track.load || !track.load(file)) {
      REXAPU_ERROR("music: could not read {}", track.label);
      Advance(gen, index, /*failed=*/true);
      return;
    }
    AsfAudioReader asf;
    WmaDecoder decoder;
    if (!asf.Open(file, track.label) || !decoder.Open(asf, track.label)) {
      Advance(gen, index, true);
      return;
    }
    REXAPU_INFO("music: playing {} ({} Hz, {} ch, {} kbit/s, {:.0f} s)", track.label,
                asf.sample_rate(), asf.channels(), asf.avg_bytes_per_sec() * 8 / 1000,
                double(asf.play_duration_100ns()) / 10'000'000.0);
    Resampler left, right;
    const double ratio = double(asf.sample_rate()) / double(kDeviceRate);
    left.Reset(ratio);
    right.Reset(ratio);
    std::vector<float> out_left, out_right;
    auto sink = [&](const float* const* planes, int channels, size_t samples) {
      left.Push(planes[0], samples);
      right.Push(channels > 1 ? planes[1] : planes[0], samples);
    };
    std::vector<uint8_t> stream;
    size_t stream_pos = 0;
    const size_t block = asf.block_align();
    bool more = true;
    while (more) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_ != gen) return;
      }
      more = asf.NextPacket(stream);
      while (stream.size() - stream_pos >= block) {
        if (!decoder.Decode(stream.data() + stream_pos, sink)) {
          REXAPU_WARN("music: decode error in {}", track.label);
        }
        stream_pos += block;
      }
      if (stream_pos > (1u << 20)) {
        stream.erase(stream.begin(), stream.begin() + stream_pos);
        stream_pos = 0;
      }
      out_left.clear();
      out_right.clear();
      left.Pull(out_left);
      right.Pull(out_right);
      if (!PushFrames(out_left, out_right, gen)) return;
    }
    decoder.Decode(nullptr, sink);
    out_left.clear();
    out_right.clear();
    left.Pull(out_left);
    right.Pull(out_right);
    if (!PushFrames(out_left, out_right, gen)) return;
    // Let the ring drain before the next track (or the stop) so the tail is heard.
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return generation_ != gen || ring_count_ == 0 || state_ == State::kStopped; });
      if (generation_ != gen) return;
    }
    Advance(gen, index, false);
  }

  void Advance(uint64_t gen, size_t index, bool failed) {
    std::function<void()> finished;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation_ != gen) return;
      if (failed && tracks_.size() <= 1) {
        state_ = State::kStopped;
        finished = on_finished_;
      } else if (index + 1 < tracks_.size()) {
        index_ = index + 1;
      } else if (repeat_) {
        index_ = 0;
      } else {
        state_ = State::kStopped;
        finished = on_finished_;
      }
      ++generation_;
      if (state_ == State::kStopped) ClearRingLocked();
    }
    if (finished) finished();
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread thread_;
  std::vector<MusicTrack> tracks_;
  size_t index_ = 0;
  bool repeat_ = false;
  State state_ = State::kStopped;
  uint64_t generation_ = 0;
  std::function<void()> on_finished_;
  std::atomic<float> volume_{1.0f};
  std::atomic<uint64_t> consumed_frames_{0};
  std::vector<float> ring_;
  size_t ring_read_ = 0, ring_write_ = 0, ring_count_ = 0;
};

}  // namespace

void MusicSetPlaylist(std::vector<MusicTrack> tracks, size_t start_index, bool repeat_playlist) {
  Player::Get().SetPlaylist(std::move(tracks), start_index, repeat_playlist);
}
void MusicPlay() { Player::Get().Play(); }
void MusicPause() { Player::Get().Pause(); }
void MusicStop() { Player::Get().Stop(); }
void MusicNext() { Player::Get().Skip(+1); }
void MusicPrevious() { Player::Get().Skip(-1); }
void MusicSetVolume(float volume) { Player::Get().SetVolume(volume); }
void MusicSetFinishedCallback(std::function<void()> cb) {
  Player::Get().SetFinishedCallback(std::move(cb));
}
bool MusicIsPlaying() { return Player::Get().IsPlaying(); }
size_t MusicCurrentIndex() { return Player::Get().CurrentIndex(); }
uint32_t MusicPositionMs() { return Player::Get().PositionMs(); }
bool MusicMixInto(float* frame_be, size_t channel_samples, size_t channels) {
  return Player::Get().MixInto(frame_be, channel_samples, channels);
}

}  // namespace rex::audio
