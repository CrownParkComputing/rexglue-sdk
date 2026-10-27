// xenon-native: XMA hardware decoder (see xma.h).
//
// Frame walking follows the XMA2 packet format (2 KB packets: 32-bit header,
// then 15-bit-length-prefixed frames whose last bit says whether another
// frame follows; frames may straddle into the next packet's payload). The
// output delay / loop subframe accounting matches what the ReXGlue/Xenia
// context implementation observed against hardware; the code is ours.
#include "audio/xma.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}

namespace xn::audio {

// ---------------------------------------------------------------------------
// Guest structures

static inline uint32_t Bswap(uint32_t v) { return __builtin_bswap32(v); }

XmaContextData XmaContextData::Load(const void* guest) {
  XmaContextData d;
  const uint32_t* src = static_cast<const uint32_t*>(guest);
  uint32_t* dst = reinterpret_cast<uint32_t*>(&d);
  for (size_t i = 0; i < kXmaContextBytes / 4; i++) dst[i] = Bswap(src[i]);
  return d;
}

void XmaContextData::Store(void* guest) const {
  const uint32_t* src = reinterpret_cast<const uint32_t*>(this);
  uint32_t* dst = static_cast<uint32_t*>(guest);
  for (size_t i = 0; i < kXmaContextBytes / 4; i++) dst[i] = Bswap(src[i]);
}

XmaLoopData XmaLoopData::Load(const void* guest) {
  const uint8_t* b = static_cast<const uint8_t*>(guest);
  XmaLoopData l;
  l.loop_start = Bswap(*reinterpret_cast<const uint32_t*>(b));
  l.loop_end = Bswap(*reinterpret_cast<const uint32_t*>(b + 4));
  l.loop_count = b[8];
  l.loop_subframe_end = b[9];
  l.loop_subframe_skip = b[10];
  return l;
}

XmaContextInit XmaContextInit::Load(const void* guest) {
  XmaContextInit i;
  const uint32_t* src = static_cast<const uint32_t*>(guest);
  uint32_t* dst = reinterpret_cast<uint32_t*>(&i);
  for (size_t k = 0; k < 13; k++) dst[k] = Bswap(src[k]);
  const uint8_t* b = static_cast<const uint8_t*>(guest) + 52;
  i.loop_count = b[0];
  i.loop_subframe_end = b[1];
  i.loop_subframe_skip = b[2];
  i.pad = b[3];
  return i;
}

// ---------------------------------------------------------------------------
// Bits and packets

XmaPacketHeader XmaParsePacketHeader(const uint8_t* p) {
  XmaPacketHeader h;
  h.frame_count = p[0] >> 2;
  h.first_frame_offset = (((p[0] & 3u) << 13) | (uint32_t(p[1]) << 5) | (p[2] >> 3)) + kXmaPacketHeaderBits;
  h.metadata = p[2] & 7;
  h.skip_count = p[3];
  return h;
}

uint32_t XmaPeekBits(const uint8_t* data, uint32_t bit_offset, uint32_t count) {
  uint64_t v = 0;
  const uint8_t* p = data + (bit_offset >> 3);
  for (int i = 0; i < 5; i++) v = (v << 8) | p[i];   // 40 bits cover any 32-bit window
  const uint32_t shift = 40 - (bit_offset & 7) - count;
  return uint32_t((v >> shift) & ((count == 32) ? 0xFFFFFFFFull : ((1ull << count) - 1)));
}

void XmaCopyBits(uint8_t* dst, uint32_t dst_bit, const uint8_t* src, uint32_t src_bit, uint32_t count) {
  // Byte-aligned fast path, bitwise otherwise; frames are at most 2 KB so the
  // slow path costs nothing measurable.
  if (((dst_bit | src_bit) & 7) == 0) {
    std::memcpy(dst + (dst_bit >> 3), src + (src_bit >> 3), count >> 3);
    dst_bit += count & ~7u;
    src_bit += count & ~7u;
    count &= 7;
  }
  while (count--) {
    const uint32_t bit = (src[src_bit >> 3] >> (7 - (src_bit & 7))) & 1;
    uint8_t& b = dst[dst_bit >> 3];
    const uint8_t mask = uint8_t(1u << (7 - (dst_bit & 7)));
    b = bit ? (b | mask) : (b & ~mask);
    dst_bit++;
    src_bit++;
  }
}

static inline void SetBit(uint8_t* data, uint32_t bit, bool value) {
  const uint8_t mask = uint8_t(1u << (7 - (bit & 7)));
  if (value) data[bit >> 3] |= mask; else data[bit >> 3] &= uint8_t(~mask);
}

// ---------------------------------------------------------------------------
// libavcodec wrapper

namespace {
void AvLog(void*, int level, const char* fmt, va_list va) {
  if (level > AV_LOG_WARNING) return;
  char buf[512];
  vsnprintf(buf, sizeof(buf), fmt, va);
  // "Could not update timestamps for skipped samples" is the codec noticing
  // our packets carry no pts; harmless.
  if (std::strstr(buf, "timestamps for skipped")) return;
  std::fprintf(stderr, "[xma/ffmpeg] %s", buf);
}
}  // namespace

XmaFrameDecoder::XmaFrameDecoder() {
  static bool log_installed = false;
  if (!log_installed) {
    log_installed = true;
    av_log_set_callback(AvLog);
  }
  codec_ = avcodec_find_decoder(AV_CODEC_ID_XMA2);
  av_frame_ = av_frame_alloc();
  av_packet_ = av_packet_alloc();
}

XmaFrameDecoder::~XmaFrameDecoder() {
  if (ctx_) avcodec_free_context(&ctx_);
  if (av_frame_) av_frame_free(&av_frame_);
  if (av_packet_) av_packet_free(&av_packet_);
}

void XmaFrameDecoder::Reset() {
  if (ctx_) avcodec_free_context(&ctx_);
  sample_rate_ = 0;
  stereo_ = false;
  frames_fed_ = 0;
}

bool XmaFrameDecoder::Prepare(int sample_rate, bool stereo) {
  if (ctx_ && sample_rate_ == sample_rate && stereo_ == stereo) return false;
  Reset();
  if (!codec_) return false;
  ctx_ = avcodec_alloc_context3(codec_);
  ctx_->sample_rate = sample_rate;
  av_channel_layout_default(&ctx_->ch_layout, stereo ? 2 : 1);
  // The codec would drop the 64-sample encoder delay itself; the hardware does
  // not, and kXmaDecoderStartPadding accounts for the whole delay.
  ctx_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
  // XMA2WAVEFORMATEX extra data (34 bytes, little-endian): one stream carries
  // both channels; the rest is informational for the decoder.
  ctx_->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
  ctx_->extradata_size = 34;
  uint8_t* x = ctx_->extradata;
  x[0] = 1; x[1] = 0;                              // NumStreams
  x[2] = stereo ? 3 : 4;                           // ChannelMask
  x[14] = 0x00; x[15] = 0x08;                      // BytesPerBlock 2048
  x[32] = 4;                                       // EncoderVersion
  if (avcodec_open2(ctx_, codec_, nullptr) < 0) {
    std::fprintf(stderr, "[xma] avcodec_open2(xma2) failed\n");
    avcodec_free_context(&ctx_);
    return false;
  }
  sample_rate_ = sample_rate;
  stereo_ = stereo;
  return true;
}

void XmaFrameDecoder::Receive(std::vector<int16_t>& out) {
  const uint32_t channels = stereo_ ? 2 : 1;
  for (;;) {
    int ret = avcodec_receive_frame(ctx_, av_frame_);
    if (ret < 0) break;  // EAGAIN / EOF / error: nothing (more) to take
    if (av_frame_->format != AV_SAMPLE_FMT_FLTP) {
      std::fprintf(stderr, "[xma] unexpected sample format %d\n", av_frame_->format);
      av_frame_unref(av_frame_);
      continue;
    }
    const size_t base = out.size();
    out.resize(base + size_t(av_frame_->nb_samples) * channels);
    for (uint32_t c = 0; c < channels; c++) {
      const float* in = reinterpret_cast<const float*>(av_frame_->data[c]);
      for (int i = 0; i < av_frame_->nb_samples; i++)
        out[base + size_t(i) * channels + c] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
    }
    av_frame_unref(av_frame_);
  }
}

bool XmaFrameDecoder::Decode(const uint8_t* frame, uint32_t frame_bits, std::vector<int16_t>& out) {
  if (!ctx_) return false;
  // One-frame XMA2 packet: frame_count 1, first frame right after the header,
  // metadata 1 (XMA2), skip 0. The frame's trailing "more frames" bit becomes 0.
  packet_.fill(0);
  packet_[0] = 1 << 2;
  packet_[2] = 1;
  XmaCopyBits(packet_.data(), kXmaPacketHeaderBits, frame, 0, frame_bits);
  SetBit(packet_.data(), kXmaPacketHeaderBits + frame_bits - 1, false);

  av_packet_unref(av_packet_);
  av_packet_->data = packet_.data();
  av_packet_->size = int(packet_.size());
  int ret = avcodec_send_packet(ctx_, av_packet_);
  if (ret < 0) {
    char e[64];
    av_strerror(ret, e, sizeof(e));
    std::fprintf(stderr, "[xma] send_packet: %s\n", e);
    return false;
  }
  frames_fed_++;
  Receive(out);
  return true;
}

void XmaFrameDecoder::Drain(std::vector<int16_t>& out) {
  if (!ctx_) return;
  if (avcodec_send_packet(ctx_, nullptr) == 0) Receive(out);
  Reset();
}

// ---------------------------------------------------------------------------
// Context

void XmaContext::Setup(uint32_t id, XmaMemory* memory, uint8_t* guest_context) {
  id_ = id;
  memory_ = memory;
  guest_ = guest_context;
}

void XmaContext::Enable() {
  enabled_.store(true, std::memory_order_release);
}

void XmaContext::Disable() {
  enabled_.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> g(lock_);  // wait for a pass in flight
}

void XmaContext::Block() {
  std::lock_guard<std::mutex> g(lock_);
}

void XmaContext::ResetStream() {
  decoder_.Reset();
  pcm_.clear();
  pcm_head_ = 0;
  lead_dropped_ = false;
  meta_.clear();
  loop_start_skip_pending_ = false;
  raw_blocks_total_ = raw_blocks_next_ = raw_blocks_limit_ = 0;
}

void XmaContext::Clear() {
  std::lock_guard<std::mutex> g(lock_);
  XmaContextData d = XmaContextData::Load(guest_);
  d.input_buffer_0_valid = 0;
  d.input_buffer_1_valid = 0;
  d.output_buffer_valid = 0;
  d.input_buffer_read_offset = kXmaPacketHeaderBits;
  d.output_buffer_read_offset = 0;
  d.output_buffer_write_offset = 0;
  d.Store(guest_);
  ResetStream();
}

void XmaContext::Release() {
  std::lock_guard<std::mutex> g(lock_);
  enabled_.store(false);
  set_allocated(false);
  ResetStream();
  std::memset(guest_, 0, kXmaContextBytes);
}

void XmaContext::SwapInputBuffer(XmaContextData& d) {
  s_swaps_.fetch_add(1, std::memory_order_relaxed);
  if (d.current_buffer == 0) d.input_buffer_0_valid = 0; else d.input_buffer_1_valid = 0;
  d.current_buffer ^= 1;
  d.input_buffer_read_offset = kXmaPacketHeaderBits;
}

// Bit offset of the first frame in the first packet at or after packet_index
// that has one; kXmaPacketHeaderBits when the buffer has no more frames.
uint32_t XmaContext::NextPacketFrameOffset(const uint8_t* buffer, uint32_t packet_index,
                                           uint32_t packet_count) {
  for (; packet_index < packet_count; packet_index++) {
    const XmaPacketHeader h = XmaParsePacketHeader(buffer + packet_index * kXmaPacketBytes);
    if (h.first_frame_offset <= kXmaMaxFrameBits) return packet_index * kXmaPacketBits + h.first_frame_offset;
  }
  return kXmaPacketHeaderBits;
}

// Feeds the frame at the read offset to the codec and advances the offset.
// Returns true when the context state moved (a frame fed, a packet skipped or
// a buffer swapped), false when nothing can be done until the title acts.
bool XmaContext::DecodeNextFrame(XmaContextData& d) {
  if (!d.AnyInputValid()) return false;
  if (!d.InputValid(d.current_buffer)) {
    SwapInputBuffer(d);
    if (!d.InputValid(d.current_buffer)) return false;
  }
  const uint32_t packet_count = d.InputPackets(d.current_buffer);
  uint8_t* buffer = memory_->Physical(d.InputPtr(d.current_buffer));
  if (!buffer || packet_count == 0) {
    SwapInputBuffer(d);
    return true;
  }
  if (d.input_buffer_read_offset < kXmaPacketHeaderBits) d.input_buffer_read_offset = kXmaPacketHeaderBits;
  uint32_t ro = d.input_buffer_read_offset;
  const uint32_t packet_index = ro / kXmaPacketBits;
  if (packet_index >= packet_count) {  // ran off the buffer: it is consumed
    SwapInputBuffer(d);
    return true;
  }
  const uint8_t* packet = buffer + packet_index * kXmaPacketBytes;
  const XmaPacketHeader hdr = XmaParsePacketHeader(packet);
  static const bool debug_parse = std::getenv("XN_XMA_DEBUG") != nullptr;
  static std::atomic<int> debug_headers{0};
  if (debug_parse && debug_headers.fetch_add(1) < 40) {
    std::fprintf(stderr,
                 "[xma] ctx%u pkt %u/%u ro %u hdr: frames %u first_off %u meta %u skip %#x "
                 "bytes %02x%02x%02x%02x\n",
                 id_, packet_index, packet_count, ro, hdr.frame_count, hdr.first_frame_offset,
                 hdr.metadata, hdr.skip_count, packet[0], packet[1], packet[2], packet[3]);
  }
  uint32_t rel = ro % kXmaPacketBits;
  if (rel < hdr.first_frame_offset) {
    rel = hdr.first_frame_offset;
    ro = packet_index * kXmaPacketBits + rel;
  }

  // Skip-only packets and packets whose frames are exhausted.
  auto advance_to_next_packet = [&]() {
    const uint32_t next = NextPacketFrameOffset(buffer, packet_index + hdr.skip_count + 1, packet_count);
    if (next == kXmaPacketHeaderBits) {
      SwapInputBuffer(d);
    } else {
      d.input_buffer_read_offset = next;
    }
  };
  if (hdr.skip_count == 0xFF) {
    advance_to_next_packet();
    return true;
  }

  // The packet this stream continues in: the next one here, else the first
  // packet of the other input buffer.
  const uint32_t avail = kXmaPacketBits - rel;  // bits left in this packet
  auto next_packet_ptr = [&]() -> const uint8_t* {
    const uint32_t next_index = packet_index + hdr.skip_count + 1;
    if (next_index < packet_count) return buffer + next_index * kXmaPacketBytes;
    const uint32_t other = d.current_buffer ^ 1;
    if (d.InputValid(other) && d.InputPtr(other)) return memory_->Physical(d.InputPtr(other));
    return nullptr;
  };

  // Frame length prefix; it may itself be split across the packet boundary
  // (the payload is one continuous bitstream around the 32-bit headers).
  uint32_t frame_bits;
  const uint8_t* next_packet = nullptr;
  if (avail >= kXmaFrameHeaderBits) {
    frame_bits = XmaPeekBits(packet, rel, kXmaFrameHeaderBits);
  } else {
    next_packet = next_packet_ptr();
    if (!next_packet) {
      d.error_status = 4;
      return false;
    }
    uint8_t tmp[8] = {};
    XmaCopyBits(tmp, 0, packet, rel, avail);
    XmaCopyBits(tmp, avail, next_packet, kXmaPacketHeaderBits, kXmaFrameHeaderBits - avail);
    frame_bits = XmaPeekBits(tmp, 0, kXmaFrameHeaderBits);
  }
  if (frame_bits == 0 || frame_bits == 0x7FFF) {
    advance_to_next_packet();
    return true;
  }

  // Loop bookkeeping: the frame at loop_end ends the loop body. The hardware
  // compares the read offset exactly; we accept "reached or passed" as well,
  // so a loop point that is not on one of our frame boundaries costs a frame
  // instead of running off the end of the buffer and stopping the music.
  bool is_loop_end_frame = false;
  if (d.loop_count > 0) {
    const uint32_t loop_end = std::max(kXmaPacketHeaderBits, uint32_t(d.loop_end));
    is_loop_end_frame = (ro >= loop_end);
    if (is_loop_end_frame && ro != loop_end)
      s_loop_inexact_.fetch_add(1, std::memory_order_relaxed);
  }

  // Gather the frame, continuing into the next packet's payload if needed.
  const bool straddles = frame_bits > avail;
  if (straddles) {
    if (!next_packet) next_packet = next_packet_ptr();
    if (!next_packet) {
      d.error_status = 4;  // waiting for the rest of the frame
      return false;
    }
    XmaCopyBits(frame_buf_.data(), 0, packet, rel, avail);
    XmaCopyBits(frame_buf_.data(), avail, next_packet, kXmaPacketHeaderBits, frame_bits - avail);
  } else {
    XmaCopyBits(frame_buf_.data(), 0, packet, rel, frame_bits);
  }
  const bool more_frames = !straddles && XmaPeekBits(packet, rel + frame_bits - 1, 1) == 1;

  // Feed the codec. Loop metadata rides along with the frame through the
  // codec's pipeline; the first frame after (re)open never comes out.
  const bool stereo = d.is_stereo != 0;
  if (decoder_.Prepare(XmaSampleRate(d.sample_rate), stereo)) {
    s_resets_.fetch_add(1, std::memory_order_relaxed);
    pcm_.clear();
    pcm_head_ = 0;
    lead_dropped_ = false;
    meta_.clear();
  }
  const bool first = decoder_.frames_fed() == 0;
  static const bool debug = getenv("XN_XMA_DEBUG") != nullptr;
  if (debug)
    std::fprintf(stderr, "[xma] ctx %u fed %u: packet %u bit %u, %u bits, straddles %d, more %d\n", id_,
                 decoder_.frames_fed(), packet_index, rel, frame_bits, straddles, more_frames);
  s_fed_.fetch_add(1, std::memory_order_relaxed);
  if (decoder_.Decode(frame_buf_.data(), frame_bits, pcm_) && !first) {
    FrameMeta m;
    m.output_limit = is_loop_end_frame ? uint8_t((d.loop_subframe_end + 1) << d.is_stereo) : 0;
    m.start_skip = loop_start_skip_pending_ ? uint8_t(d.loop_subframe_skip << d.is_stereo) : 0;
    meta_.push_back(m);
  }
  loop_start_skip_pending_ = false;

  if (is_loop_end_frame) {
    d.input_buffer_read_offset = std::max(kXmaPacketHeaderBits, uint32_t(d.loop_start));
    loop_start_skip_pending_ = true;
    s_loops_.fetch_add(1, std::memory_order_relaxed);
    if (d.loop_count < 255) d.loop_count--;
    return true;
  }
  if (more_frames) {
    d.input_buffer_read_offset = ro + frame_bits;
    return true;
  }
  // Last frame of the packet: move to the next packet of this stream.
  uint32_t next = NextPacketFrameOffset(buffer, packet_index + hdr.skip_count + 1, packet_count);
  if (next == kXmaPacketHeaderBits) {
    SwapInputBuffer(d);
    if (d.AnyInputValid() && d.InputPtr(d.current_buffer)) {
      const uint8_t* nb = memory_->Physical(d.InputPtr(d.current_buffer));
      const XmaPacketHeader nh = XmaParsePacketHeader(nb);
      if (nh.first_frame_offset > kXmaMaxFrameBits) {
        SwapInputBuffer(d);
        return true;
      }
      next = nh.first_frame_offset;
    }
  }
  d.input_buffer_read_offset = next;
  return true;
}

// Moves the next 512-sample frame of codec output into raw_frame_ (big-endian
// 256-byte blocks) with its loop limits. False when not enough output yet.
bool XmaContext::TakeRawFrame(const XmaContextData& d) {
  const uint32_t channels = d.is_stereo ? 2 : 1;
  if (!lead_dropped_) {
    const size_t lead = kXmaDecoderStartPadding * channels;
    if (pcm_.size() - pcm_head_ < lead) return false;
    pcm_head_ += lead;
    lead_dropped_ = true;
  }
  const size_t frame_samples = kXmaSamplesPerFrame * channels;
  if (pcm_.size() - pcm_head_ < frame_samples) {
    if (pcm_head_ > 0) {  // compact
      pcm_.erase(pcm_.begin(), pcm_.begin() + ptrdiff_t(pcm_head_));
      pcm_head_ = 0;
    }
    return false;
  }
  uint16_t* be = reinterpret_cast<uint16_t*>(raw_frame_.data());
  for (size_t i = 0; i < frame_samples; i++) be[i] = __builtin_bswap16(uint16_t(pcm_[pcm_head_ + i]));
  pcm_head_ += frame_samples;
  FrameMeta m{0, 0};
  if (!meta_.empty()) {
    m = meta_.front();
    meta_.pop_front();
  }
  s_decoded_.fetch_add(1, std::memory_order_relaxed);
  raw_blocks_total_ = uint8_t(4u << d.is_stereo);
  raw_blocks_next_ = std::min(m.start_skip, raw_blocks_total_);
  raw_blocks_limit_ = m.output_limit ? std::min(m.output_limit, raw_blocks_total_) : 0;
  return true;
}

namespace {
// Sets a flag for the lifetime of a scope.
struct FlagGuard {
  std::atomic<bool>& flag;
  explicit FlagGuard(std::atomic<bool>& f) : flag(f) { flag.store(true, std::memory_order_release); }
  ~FlagGuard() { flag.store(false, std::memory_order_release); }
};
}  // namespace

XmaContext::Stats XmaContext::stats() const {
  Stats s;
  s.passes = s_passes_.load(std::memory_order_relaxed);
  s.blocks_written = s_blocks_.load(std::memory_order_relaxed);
  s.frames_decoded = s_decoded_.load(std::memory_order_relaxed);
  s.frames_fed = s_fed_.load(std::memory_order_relaxed);
  s.swaps = s_swaps_.load(std::memory_order_relaxed);
  s.drains = s_drains_.load(std::memory_order_relaxed);
  s.resets = s_resets_.load(std::memory_order_relaxed);
  s.loops = s_loops_.load(std::memory_order_relaxed);
  s.errors = s_errors_.load(std::memory_order_relaxed);
  s.ring_full = s_full_.load(std::memory_order_relaxed);
  s.loop_inexact = s_loop_inexact_.load(std::memory_order_relaxed);
  return s;
}

std::string XmaContext::Describe() const {
  const XmaContextData d = XmaContextData::Load(guest_);
  const Stats s = stats();
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "ctx%u %s%s in0=%08x/%u%s in1=%08x/%u%s cur=%u ro=%u(pkt %u) out=%08x r=%u w=%u/%u %s "
                "err=%u loop=%u(%u..%u) | passes=%llu blocks=%llu fed=%llu dec=%llu swap=%llu drain=%llu "
                "reset=%llu loops=%llu(%llu inexact) full=%llu pcm=%zu meta=%zu",
                id_, allocated() ? "A" : "-", enabled() ? "E" : "-",
                uint32_t(d.input_buffer_0_ptr), uint32_t(d.input_buffer_0_packet_count),
                d.input_buffer_0_valid ? "V" : "-", uint32_t(d.input_buffer_1_ptr),
                uint32_t(d.input_buffer_1_packet_count), d.input_buffer_1_valid ? "V" : "-",
                uint32_t(d.current_buffer), uint32_t(d.input_buffer_read_offset),
                uint32_t(d.input_buffer_read_offset) / kXmaPacketBits, uint32_t(d.output_buffer_ptr),
                uint32_t(d.output_buffer_read_offset), uint32_t(d.output_buffer_write_offset),
                uint32_t(d.output_buffer_block_count), d.output_buffer_valid ? "valid" : "INVALID",
                uint32_t(d.error_status), uint32_t(d.loop_count), uint32_t(d.loop_start),
                uint32_t(d.loop_end), (unsigned long long)s.passes, (unsigned long long)s.blocks_written,
                (unsigned long long)s.frames_fed, (unsigned long long)s.frames_decoded,
                (unsigned long long)s.swaps, (unsigned long long)s.drains, (unsigned long long)s.resets,
                (unsigned long long)s.loops, (unsigned long long)s.loop_inexact,
                (unsigned long long)s.ring_full, pcm_.size() - pcm_head_,
                meta_.size());
  return buf;
}

bool XmaContext::Work() {
  if (!allocated() || !enabled()) return false;
  std::lock_guard<std::mutex> g(lock_);
  // Disable() flips the flag and then waits for the lock; a pass that got the
  // lock afterwards must not run while the title edits the context.
  if (!allocated() || !enabled()) return false;
  s_passes_.fetch_add(1, std::memory_order_relaxed);
  // Visible to XmaRegisterFile while this pass may be writing guest_.
  FlagGuard working(working_);
  XmaContextData d = XmaContextData::Load(guest_);
  const XmaContextData initial = d;
  if (!d.output_buffer_valid || !d.output_buffer_block_count || !d.output_buffer_ptr) return false;

  const uint32_t cap = d.output_buffer_block_count;
  uint8_t* ring = memory_->Physical(d.output_buffer_ptr);
  if (!ring) return false;
  const uint32_t r = d.output_buffer_read_offset % cap;
  uint32_t w = d.output_buffer_write_offset % cap;
  // valid && read == write means empty: the whole ring is writable. The pass
  // ends with valid cleared when write catches read (ring full).
  int32_t space = int32_t((r + cap - w) % cap);
  if (space == 0) space = int32_t(cap);
  const uint32_t sdc = std::max(1u, uint32_t(d.subframe_decode_count));
  const uint32_t pad = d.output_buffer_padding;
  bool progressed = false;
  bool drained = false;

  while (space >= int32_t(sdc + pad)) {
    const uint32_t end = raw_blocks_limit_ ? raw_blocks_limit_ : raw_blocks_total_;
    if (raw_blocks_next_ >= end) {
      // Need a new frame.
      if (raw_blocks_total_) {
        space -= int32_t(pad);  // a (possibly loop-trimmed) frame finished
        raw_blocks_total_ = raw_blocks_next_ = raw_blocks_limit_ = 0;
        continue;
      }
      if (TakeRawFrame(d)) continue;
      if (!d.AnyInputValid()) {
        // Input ran out: release what the codec still holds, once.
        if (!drained && decoder_.open() && decoder_.frames_fed() > 0) {
          s_drains_.fetch_add(1, std::memory_order_relaxed);
          decoder_.Drain(pcm_);
          drained = true;
          continue;
        }
        break;
      }
      if (!DecodeNextFrame(d)) break;
      if (d.error_status == 4) break;
      continue;
    }
    uint32_t n = std::min<uint32_t>(end - raw_blocks_next_, sdc);
    n = std::min<uint32_t>(n, uint32_t(space));
    for (uint32_t i = 0; i < n; i++) {
      std::memcpy(ring + w * kXmaOutputBlockBytes,
                  raw_frame_.data() + (raw_blocks_next_ + i) * kXmaOutputBlockBytes, kXmaOutputBlockBytes);
      w = (w + 1) % cap;
    }
    raw_blocks_next_ = uint8_t(raw_blocks_next_ + n);
    space -= int32_t(n);
    s_blocks_.fetch_add(n, std::memory_order_relaxed);
    progressed = true;
    if (w == r) break;  // full
  }

  // Merge back only what the decoder owns; the title may have touched the
  // other fields (valid bits, read offset) while we were decoding.
  XmaContextData fresh = XmaContextData::Load(guest_);
  fresh.loop_count = d.loop_count;
  fresh.output_buffer_write_offset = w;
  if (initial.input_buffer_0_valid && !d.input_buffer_0_valid) fresh.input_buffer_0_valid = 0;
  if (initial.input_buffer_1_valid && !d.input_buffer_1_valid) fresh.input_buffer_1_valid = 0;
  if (progressed && w == r) {
    fresh.output_buffer_valid = 0;  // full
    s_full_.fetch_add(1, std::memory_order_relaxed);
  }
  if (d.error_status) s_errors_.fetch_add(1, std::memory_order_relaxed);
  fresh.input_buffer_read_offset = d.input_buffer_read_offset;
  fresh.error_status = d.error_status;
  fresh.current_buffer = d.current_buffer;
  fresh.Store(guest_);
  return progressed;
}

// ---------------------------------------------------------------------------
// Engine

XmaEngine::~XmaEngine() { Shutdown(); }

void XmaEngine::Setup(XmaMemory* memory, uint8_t* guest_table, uint32_t guest_table_address) {
  memory_ = memory;
  table_ = guest_table;
  table_address_ = guest_table_address;
  std::memset(table_, 0, kXmaContextCount * kXmaContextBytes);
  for (uint32_t i = 0; i < kXmaContextCount; i++) contexts_[i].Setup(i, memory, table_ + i * kXmaContextBytes);
  running_ = true;
  worker_ = std::thread([this] { WorkerMain(); });
}

void XmaEngine::Shutdown() {
  if (!running_.exchange(false)) return;
  {
    std::lock_guard<std::mutex> g(wake_lock_);
    kicked_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

uint32_t XmaEngine::AllocateContext() {
  std::lock_guard<std::mutex> g(alloc_lock_);
  for (uint32_t i = 0; i < kXmaContextCount; i++) {
    if (!contexts_[i].allocated()) {
      std::memset(contexts_[i].guest(), 0, kXmaContextBytes);
      contexts_[i].set_allocated(true);
      return table_address_ + i * kXmaContextBytes;
    }
  }
  return 0;
}

XmaContext* XmaEngine::Lookup(uint32_t guest_address) {
  if (guest_address < table_address_) return nullptr;
  const uint32_t off = guest_address - table_address_;
  if (off % kXmaContextBytes || off / kXmaContextBytes >= kXmaContextCount) return nullptr;
  return &contexts_[off / kXmaContextBytes];
}

void XmaEngine::ReleaseContext(uint32_t guest_address) {
  if (XmaContext* c = Lookup(guest_address)) c->Release();
}

void XmaEngine::Enable(uint32_t guest_address) {
  XmaContext* c = Lookup(guest_address);
  if (!c) return;
  c->Enable();
  {
    std::lock_guard<std::mutex> g(wake_lock_);
    kicked_ = true;
  }
  wake_.notify_one();
}

void XmaEngine::Disable(uint32_t guest_address) {
  if (XmaContext* c = Lookup(guest_address)) c->Disable();
}

bool XmaEngine::WorkContext(uint32_t guest_address) {
  XmaContext* c = Lookup(guest_address);
  return c ? c->Work() : false;
}

int32_t XmaEngine::WorkingContextIndex() const {
  for (uint32_t i = 0; i < kXmaContextCount; i++) {
    if (contexts_[i].working()) return int32_t(i);
  }
  return -1;
}

std::vector<XmaContext*> XmaEngine::AllocatedContexts() {
  std::vector<XmaContext*> out;
  for (auto& c : contexts_) if (c.allocated()) out.push_back(&c);
  return out;
}

bool XmaEngine::WorkAll() {
  bool any = false;
  for (auto& c : contexts_) any |= c.Work();
  return any;
}

void XmaEngine::WorkerMain() {
  // A kick wakes us; while any context is enabled we sweep every 2 ms so a
  // ring the title drained gets refilled without another kick (the hardware
  // block is autonomous once enabled).
  while (running_) {
    sweeps_.fetch_add(1, std::memory_order_relaxed);
    bool any_enabled = false;
    for (auto& c : contexts_) {
      if (c.enabled()) {
        any_enabled = true;
        c.Work();
      }
    }
    std::unique_lock<std::mutex> lk(wake_lock_);
    if (any_enabled) {
      wake_.wait_for(lk, std::chrono::milliseconds(2), [&] { return kicked_ || !running_; });
    } else {
      wake_.wait(lk, [&] { return kicked_ || !running_; });
    }
    kicked_ = false;
  }
}

}  // namespace xn::audio
