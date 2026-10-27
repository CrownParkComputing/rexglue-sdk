// xenon-native: offline XMA check. Decodes one wave-bank entry two ways -
// straight through libavcodec's xma2 decoder on whole 2 KB packets, and
// through the XmaEngine driven exactly the way the title drives the hardware
// context (input buffer valid bits, 10-block output ring, read offset polls) -
// and compares them. Writes .wav files for a listen check.
//
//   xma_decode_test <assets dir> <out dir> [bank.xwb] [entry]
//   xma_decode_test <assets dir> <out dir> <file.xma>
//
// The second form takes a RIFF/WAVE file whose wFormatTag is 0x0165
// (WAVE_FORMAT_XMA, "XMA1") - the shape Boom Boom Rocket ships all 101 of its
// assets in - and runs the same two paths over its data chunk. It exists to
// settle one question: our XmaEngine opens libavcodec's xma2 decoder, and
// these files are tagged XMA1, so does the XMA2 decoder accept their frames?
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include "audio/xma.h"
#include "audio/xwb.h"

using namespace xn::audio;

static bool WriteWav(const std::string& path, const std::vector<int16_t>& pcm, int channels, int rate) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint32_t data_bytes = uint32_t(pcm.size() * 2);
  auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f); w32(36 + data_bytes); std::fwrite("WAVEfmt ", 1, 8, f);
  w32(16); w16(1); w16(uint16_t(channels)); w32(uint32_t(rate)); w32(uint32_t(rate * channels * 2));
  w16(uint16_t(channels * 2)); w16(16); std::fwrite("data", 1, 4, f); w32(data_bytes);
  std::fwrite(pcm.data(), 2, pcm.size(), f);
  std::fclose(f);
  return true;
}

static void Stats(const char* label, const std::vector<int16_t>& pcm, int channels, int rate) {
  double sum2 = 0, sum = 0; int peak = 0; size_t zero_run = 0, max_zero_run = 0;
  for (int16_t s : pcm) {
    sum2 += double(s) * s; sum += s; peak = std::max(peak, std::abs(int(s)));
    if (s == 0) { if (++zero_run > max_zero_run) max_zero_run = zero_run; } else zero_run = 0;
  }
  const size_t n = std::max<size_t>(1, pcm.size());
  std::printf("  %s: %zu frames (%.2f s), peak %d (%.1f dBFS), rms %.1f dBFS, dc %.1f, longest zero run %zu samples\n",
              label, pcm.size() / channels, double(pcm.size()) / channels / rate, peak,
              20 * std::log10(std::max(1, peak) / 32768.0), 10 * std::log10(sum2 / n / (32768.0 * 32768.0)),
              sum / n, max_zero_run / channels);
}

// Path A: whole packets through the codec.
static std::vector<int16_t> DecodeWholePackets(const uint8_t* data, size_t size, int channels, int rate) {
  std::vector<int16_t> out;
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  ctx->sample_rate = rate;
  av_channel_layout_default(&ctx->ch_layout, channels);
  ctx->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
  ctx->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
  ctx->extradata_size = 34;
  ctx->extradata[0] = 1; ctx->extradata[2] = channels == 2 ? 3 : 4; ctx->extradata[15] = 8; ctx->extradata[32] = 4;
  if (avcodec_open2(ctx, codec, nullptr) < 0) { std::printf("  open failed\n"); return out; }
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  std::vector<uint8_t> buf(kXmaPacketBytes + AV_INPUT_BUFFER_PADDING_SIZE);
  for (size_t off = 0; off + kXmaPacketBytes <= size; off += kXmaPacketBytes) {
    std::memcpy(buf.data(), data + off, kXmaPacketBytes);
    pkt->data = buf.data(); pkt->size = int(kXmaPacketBytes);
    if (avcodec_send_packet(ctx, pkt) < 0) { std::printf("  send failed at packet %zu\n", off / kXmaPacketBytes); break; }
    while (avcodec_receive_frame(ctx, frame) == 0) {
      for (int i = 0; i < frame->nb_samples; i++)
        for (int c = 0; c < channels; c++)
          out.push_back(int16_t(std::clamp(reinterpret_cast<float*>(frame->data[c])[i], -1.0f, 1.0f) * 32767.0f));
      av_frame_unref(frame);
    }
  }
  avcodec_send_packet(ctx, nullptr);
  while (avcodec_receive_frame(ctx, frame) == 0) {
    for (int i = 0; i < frame->nb_samples; i++)
      for (int c = 0; c < channels; c++)
        out.push_back(int16_t(std::clamp(reinterpret_cast<float*>(frame->data[c])[i], -1.0f, 1.0f) * 32767.0f));
    av_frame_unref(frame);
  }
  av_frame_free(&frame); av_packet_free(&pkt); avcodec_free_context(&ctx);
  return out;
}

// Path B: the engine, driven like the title's XMA driver.
struct HostMemory : XmaMemory {
  std::vector<uint8_t> mem;
  uint8_t* Physical(uint32_t phys) override { return phys < mem.size() ? mem.data() + phys : nullptr; }
};

static std::vector<int16_t> DecodeThroughEngine(const uint8_t* data, size_t size, int channels, int rate,
                                                uint32_t loop_count, uint32_t loop_start, uint32_t loop_end,
                                                bool split_input, size_t stop_after = 0) {
  const uint32_t packets = uint32_t(size / kXmaPacketBytes);
  HostMemory mem;
  const uint32_t kTable = 0x1000, kRing = 0x8000, kRingBlocks = 10, kInput0 = 0x10000;
  // Optionally split the stream across the two input buffers, alternating
  // 8-packet halves the way a streaming title would.
  const uint32_t chunk = split_input ? 8 : packets;
  mem.mem.resize(kInput0 + packets * kXmaPacketBytes + 0x10000);
  std::memcpy(mem.mem.data() + kInput0, data, packets * kXmaPacketBytes);

  XmaEngine engine;
  engine.Setup(&mem, mem.mem.data() + kTable, kTable);
  const uint32_t ctx_addr = engine.AllocateContext();
  XmaContext* c = engine.Lookup(ctx_addr);
  uint8_t* g = c->guest();
  // XMAInitializeContext equivalent.
  XmaContextData d{};
  d.output_buffer_ptr = kRing;
  d.output_buffer_block_count = kRingBlocks;
  d.subframe_decode_count = 4;
  d.is_stereo = channels == 2;
  d.sample_rate = 3;
  d.input_buffer_read_offset = kXmaPacketHeaderBits;
  d.loop_count = loop_count; d.loop_start = loop_start; d.loop_end = loop_end;
  d.loop_subframe_end = 3; d.loop_subframe_skip = 0;
  uint32_t next_packet = 0;
  auto fill = [&](uint32_t which) {
    if (next_packet >= packets) return false;
    const uint32_t n = std::min(chunk, packets - next_packet);
    if (which == 0) { d.input_buffer_0_ptr = kInput0 + next_packet * kXmaPacketBytes; d.input_buffer_0_packet_count = n; d.input_buffer_0_valid = 1; }
    else { d.input_buffer_1_ptr = kInput0 + next_packet * kXmaPacketBytes; d.input_buffer_1_packet_count = n; d.input_buffer_1_valid = 1; }
    next_packet += n;
    return true;
  };
  fill(0);
  if (split_input) fill(1);
  d.output_buffer_valid = 1;
  d.Store(g);
  engine.Enable(ctx_addr);

  std::vector<int16_t> out;
  const uint32_t block_samples = kXmaOutputBlockBytes / 2;
  int idle = 0;
  for (int iter = 0; iter < 20000000; iter++) {
    // The title's consumer: disable (wait), read offsets, drain, set read
    // offset + valid, refill an input buffer that went invalid, enable.
    engine.Disable(ctx_addr);
    d = XmaContextData::Load(g);
    uint32_t r = d.output_buffer_read_offset, w = d.output_buffer_write_offset;
    uint32_t avail = (d.output_buffer_valid == 0 && r == w) ? kRingBlocks : (w + kRingBlocks - r) % kRingBlocks;
    for (uint32_t i = 0; i < avail; i++) {
      const uint16_t* be = reinterpret_cast<const uint16_t*>(mem.mem.data() + kRing + ((r + i) % kRingBlocks) * kXmaOutputBlockBytes);
      for (uint32_t s = 0; s < block_samples; s++) out.push_back(int16_t(__builtin_bswap16(be[s])));
    }
    d.output_buffer_read_offset = (r + avail) % kRingBlocks;
    d.output_buffer_valid = 1;
    bool refilled = false;
    if (!d.input_buffer_0_valid && next_packet < packets) refilled = fill(0);
    if (split_input && !d.input_buffer_1_valid && next_packet < packets) refilled |= fill(1);
    d.Store(g);
    if (stop_after && out.size() >= stop_after) break;
    if (!avail && !refilled && !d.AnyInputValid() && d.error_status == 0) break;  // stream exhausted
    if (!avail && !refilled) { if (++idle > 50) break; } else idle = 0;
    engine.Enable(ctx_addr);
    engine.WorkAll();
  }
  engine.Shutdown();
  return out;
}

// The exact bytes the guest passes to XMASetLoopData, captured from a real
// run (XN_AUDIO_TRACE): loop_start 32, loop_end 0x0086CB1F, count 255 (infinite),
// subframe end 3, skip 3. Pins the 12-byte XMA_LOOP_DATA layout: reading this
// as an XMA_CONTEXT_DATA is what made the music play once and stop.
static int CheckLoopDataLayout() {
  const uint8_t bytes[16] = {0x00, 0x00, 0x00, 0x20, 0x00, 0x86, 0xcb, 0x1f,
                             0xff, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00};
  const XmaLoopData l = XmaLoopData::Load(bytes);
  const bool ok = l.loop_start == 32 && l.loop_end == 0x0086CB1F && l.loop_count == 255 &&
                  l.loop_subframe_end == 3 && l.loop_subframe_skip == 3;
  std::printf("  XMA_LOOP_DATA: start %u end %u count %u end_sf %u skip_sf %u  %s\n", l.loop_start,
              l.loop_end, l.loop_count, l.loop_subframe_end, l.loop_subframe_skip,
              ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

// A RIFF/WAVE container whose wFormatTag is 0x0165 (WAVE_FORMAT_XMA). The
// format chunk is XMAWAVEFORMAT: a little-endian header followed by one
// XMASTREAMFORMAT per stream. Everything downstream only wants the packet
// bytes plus channels and rate, so this fills the same XwbEntry the bank
// reader does and the rest of the test is unchanged.
static bool RiffXmaLoad(const std::string& path, XwbBank& bank, std::string& error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { error = "cannot open " + path; return false; }
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  bank.file.resize(size_t(n < 0 ? 0 : n));
  const bool read = std::fread(bank.file.data(), 1, bank.file.size(), f) == bank.file.size();
  std::fclose(f);
  if (!read || bank.file.size() < 44) { error = "short file"; return false; }
  const uint8_t* d = bank.file.data();
  auto le32 = [&](size_t o) { uint32_t v; std::memcpy(&v, d + o, 4); return v; };
  auto le16 = [&](size_t o) { uint16_t v; std::memcpy(&v, d + o, 2); return v; };
  if (std::memcmp(d, "RIFF", 4) || std::memcmp(d + 8, "WAVE", 4)) { error = "not a RIFF/WAVE file"; return false; }
  size_t fmt = 0, fmt_size = 0, data = 0, data_size = 0, off = 12;
  while (off + 8 <= bank.file.size()) {
    const size_t size = le32(off + 4);
    if (!std::memcmp(d + off, "fmt ", 4)) { fmt = off + 8; fmt_size = size; }
    else if (!std::memcmp(d + off, "data", 4)) { data = off + 8; data_size = size; }
    off += 8 + size + (size & 1);
  }
  if (!fmt || fmt_size < 32 || !data) { error = "no fmt/data chunk"; return false; }
  if (data + data_size > bank.file.size()) data_size = bank.file.size() - data;
  const uint16_t tag = le16(fmt);
  if (tag != 0x0165) { error = "wFormatTag is not 0x0165 (WAVE_FORMAT_XMA)"; return false; }
  XwbEntry e;
  e.tag = 1;                      // XMA, so the test takes the decode path
  e.bits_per_sample = le16(fmt + 2);
  const uint16_t streams = le16(fmt + 8);
  const uint8_t loop_count = d[fmt + 10], version = d[fmt + 11];
  e.sample_rate = le32(fmt + 16);  // XMASTREAMFORMAT[0].SampleRate
  e.loop_a = le32(fmt + 20);       // LoopStart
  e.loop_b = le32(fmt + 24);       // LoopEnd
  e.channels = d[fmt + 29];        // XMASTREAMFORMAT[0].Channels
  e.play_offset = uint32_t(data);
  e.play_length = uint32_t(data_size - data_size % kXmaPacketBytes);
  e.duration = loop_count;
  bank.version = version;
  bank.alignment = streams;
  bank.wave_data_offset = 0;       // play_offset is already a file offset
  bank.wave_data_length = uint32_t(data_size);
  bank.entries.push_back(e);
  std::printf("RIFF/XMA: tag %#06x, version %u, %u stream(s), %u ch, %u Hz, "
              "loop_count %u, loop %u..%u, data %zu bytes = %u packets%s\n",
              tag, version, streams, e.channels, e.sample_rate, loop_count, e.loop_a, e.loop_b,
              data_size, e.play_length / kXmaPacketBytes,
              data_size % kXmaPacketBytes ? " (+ a partial tail, ignored)" : "");
  return true;
}

static bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s <assets dir> <out dir> [bank.xwb|file.xma] [entry]\n", argv[0]); return 2; }
  const std::string assets = argv[1], outdir = argv[2];
  const std::string bank_name = argc > 3 ? argv[3] : "GW2_Music.xwb";
  const int entry_index = argc > 4 ? std::atoi(argv[4]) : 1;
  const bool riff = EndsWith(bank_name, ".xma") || EndsWith(bank_name, ".XMA");
  // The loop-data layout check reads Geometry Wars' own XMA_LOOP_DATA blob;
  // it says nothing about a RIFF input, so it only runs for a bank.
  int failures = riff ? 0 : CheckLoopDataLayout();

  XwbBank bank;
  std::string err;
  if (riff) {
    if (!RiffXmaLoad(assets + "/" + bank_name, bank, err)) { std::printf("riff: %s\n", err.c_str()); return 1; }
  } else {
    if (!XwbLoad(assets + "/" + bank_name, bank, err)) { std::printf("xwb: %s\n", err.c_str()); return 1; }
    std::printf("%s: version %u, %zu entries, alignment %u\n", bank_name.c_str(), bank.version, bank.entries.size(), bank.alignment);
  }
  for (size_t i = 0; i < bank.entries.size(); i++) {
    const XwbEntry& e = bank.entries[i];
    std::printf("  [%zu] %s %uch %u Hz, %u bytes @ %u, duration %u, loop %08x/%08x\n", i,
                e.tag ? "XMA" : "PCM", e.channels, e.sample_rate, e.play_length, e.play_offset, e.duration, e.loop_a, e.loop_b);
  }
  if (entry_index < 0 || size_t(entry_index) >= bank.entries.size()) { std::printf("bad entry\n"); return 1; }
  const XwbEntry& e = bank.entries[entry_index];
  const std::string stem = outdir + "/" + bank_name.substr(0, bank_name.find('.')) + "_" + std::to_string(entry_index);

  if (e.tag == 0) {
    // PCM bank: byte-swap and write.
    std::vector<int16_t> pcm(e.play_length / 2);
    const uint16_t* be = reinterpret_cast<const uint16_t*>(bank.wave_data(e));
    for (size_t i = 0; i < pcm.size(); i++) pcm[i] = int16_t(__builtin_bswap16(be[i]));
    Stats("pcm", pcm, int(e.channels), int(e.sample_rate));
    WriteWav(stem + "_pcm.wav", pcm, int(e.channels), int(e.sample_rate));
    std::printf("  wrote %s_pcm.wav\n", stem.c_str());
    return 0;
  }

  const uint8_t* data = bank.wave_data(e);
  const XmaPacketHeader h0 = XmaParsePacketHeader(data);
  for (uint32_t p = 0; p < 6; p++) {
    const XmaPacketHeader h = XmaParsePacketHeader(data + p * kXmaPacketBytes);
    std::printf("  packet %u: %u frames, first frame at bit %u, metadata %u, skip %u\n", p, h.frame_count, h.first_frame_offset, h.metadata, h.skip_count);
  }

  std::vector<int16_t> a = DecodeWholePackets(data, e.play_length, int(e.channels), int(e.sample_rate));
  Stats("whole-packet libavcodec", a, int(e.channels), int(e.sample_rate));
  WriteWav(stem + "_ref.wav", a, int(e.channels), int(e.sample_rate));

  for (int split = 0; split < 2; split++) {
    std::vector<int16_t> b = DecodeThroughEngine(data, e.play_length, int(e.channels), int(e.sample_rate), 0, 0, 0, split != 0);
    Stats(split ? "engine (two input buffers)" : "engine (one input buffer)", b, int(e.channels), int(e.sample_rate));
    // The engine output is the codec output delayed by kXmaDecoderStartPadding
    // samples, minus the tail that never leaves the carry.
    const size_t shift = kXmaDecoderStartPadding * e.channels;
    size_t compared = 0, mismatched = 0; int maxdiff = 0;
    size_t first_bad = SIZE_MAX;
    for (size_t i = 0; i < b.size() && i + shift < a.size(); i++) {
      compared++;
      const int diff = std::abs(int(b[i]) - int(a[i + shift]));
      if (diff) { mismatched++; maxdiff = std::max(maxdiff, diff); first_bad = std::min(first_bad, i); }
    }
    if (mismatched) {
      std::printf("    first mismatch at sample %zu (frame %zu)\n", first_bad, first_bad / (kXmaSamplesPerFrame * e.channels));
      for (ptrdiff_t slip = -4096; slip <= 4096; slip += e.channels) {
        bool match = true;
        for (size_t k = 0; k < 4096 && match; k++) {
          const ptrdiff_t ai = ptrdiff_t(first_bad + k + shift) + slip;
          if (ai < 0 || size_t(ai) >= a.size() || first_bad + k >= b.size()) { match = false; break; }
          match = b[first_bad + k] == a[size_t(ai)];
        }
        if (match) { std::printf("    resyncs with slip %td samples (%td per channel)\n", slip, slip / ptrdiff_t(e.channels)); break; }
      }
    }
    std::printf("    vs reference: compared %zu samples, %zu mismatched, max diff %d, length delta %zd samples\n",
                compared, mismatched, maxdiff, ptrdiff_t(a.size()) - ptrdiff_t(b.size()) - ptrdiff_t(shift));
    const bool ok = compared > 0 && mismatched == 0 && b.size() + shift + kXmaSamplesPerFrame * e.channels * 2 >= a.size();
    std::printf("    %s\n", ok ? "PASS" : "FAIL");
    if (!ok) failures++;
    if (!split) { WriteWav(stem + ".wav", b, int(e.channels), int(e.sample_rate)); std::printf("    wrote %s.wav\n", stem.c_str()); }
  }

  // Infinite loop (loop_count 255) with the title's real loop points: the
  // stream must never run dry. Two loop periods' worth of output is enough to
  // show it wraps and keeps going. The loop points below are Geometry Wars'
  // own, so this only runs for that bank; a RIFF/XMA file carries LoopStart
  // and LoopEnd of its own and they are a separate question.
  if (!riff) {
    const uint32_t loop_start = 32, loop_end = 0x0086CB1F;  // from the trace above
    const size_t one_pass = a.size();
    std::vector<int16_t> inf = DecodeThroughEngine(data, e.play_length, int(e.channels),
                                                   int(e.sample_rate), 255, loop_start, loop_end,
                                                   false, size_t(one_pass * 2.5));
    Stats("engine (loop_count 255)", inf, int(e.channels), int(e.sample_rate));
    const bool ok = inf.size() >= size_t(one_pass * 2.4);
    std::printf("    produced %.2fx one pass without running dry: %s\n",
                double(inf.size()) / double(one_pass), ok ? "PASS" : "FAIL");
    if (!ok) failures++;
  }

  // Restart by re-validating the same buffer: what the title does when a
  // stream ends and it wants it again (it never calls XMASetInputBuffer0, only
  // XMASetInputBuffer0Valid). The decoder must resume at the top of the buffer
  // and produce the same audio, not continue from a stale offset.
  {
    const uint32_t packets = uint32_t(e.play_length / kXmaPacketBytes);
    HostMemory mem;
    const uint32_t kTable = 0x1000, kRing = 0x8000, kBlocks = 8, kIn = 0x10000;
    mem.mem.resize(kIn + packets * kXmaPacketBytes + 0x10000);
    std::memcpy(mem.mem.data() + kIn, data, packets * kXmaPacketBytes);
    XmaEngine engine;
    engine.Setup(&mem, mem.mem.data() + kTable, kTable);
    const uint32_t ctx = engine.AllocateContext();
    uint8_t* g = engine.Lookup(ctx)->guest();
    XmaContextData d{};
    d.output_buffer_ptr = kRing;
    d.output_buffer_block_count = kBlocks;
    d.subframe_decode_count = 2;
    d.is_stereo = e.channels == 2;
    d.sample_rate = 3;
    d.input_buffer_read_offset = kXmaPacketHeaderBits;
    d.input_buffer_0_ptr = kIn;
    d.input_buffer_0_packet_count = packets;
    d.input_buffer_0_valid = 1;
    d.output_buffer_valid = 1;
    d.Store(g);
    engine.Enable(ctx);
    std::vector<int16_t> pass[2];
    const uint32_t blk = kXmaOutputBlockBytes / 2;
    for (int p = 0; p < 2; p++) {
      int idle = 0;
      for (;;) {
        engine.Disable(ctx);
        d = XmaContextData::Load(g);
        const uint32_t r = d.output_buffer_read_offset, w = d.output_buffer_write_offset;
        const uint32_t avail = (!d.output_buffer_valid && r == w) ? kBlocks : (w + kBlocks - r) % kBlocks;
        for (uint32_t i = 0; i < avail; i++) {
          const uint16_t* be = reinterpret_cast<const uint16_t*>(mem.mem.data() + kRing + ((r + i) % kBlocks) * kXmaOutputBlockBytes);
          for (uint32_t k = 0; k < blk; k++) pass[p].push_back(int16_t(__builtin_bswap16(be[k])));
        }
        d.output_buffer_read_offset = (r + avail) % kBlocks;
        d.output_buffer_valid = 1;
        d.Store(g);
        if (!avail) {
          if (!d.input_buffer_0_valid) break;  // buffer consumed: end of this pass
          if (++idle > 200) break;
        } else {
          idle = 0;
        }
        engine.Enable(ctx);
        engine.WorkAll();
      }
      if (p == 0) {  // the title's restart: mark the same buffer valid again
        d = XmaContextData::Load(g);
        d.input_buffer_0_valid = 1;
        d.output_buffer_valid = 1;
        d.Store(g);
        engine.Enable(ctx);
      }
    }
    engine.Shutdown();
    const size_t n = std::min(pass[0].size(), pass[1].size());
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) if (pass[0][i] != pass[1][i]) bad++;
    std::printf("  restart by re-validate: pass1 %zu samples, pass2 %zu, %zu of %zu differ\n",
                pass[0].size(), pass[1].size(), bad, n);
    // Pass 2 starts one frame later (the codec's first frame after the restart
    // is the one it drops), so allow a frame of slack on length.
    const bool ok = pass[1].size() > pass[0].size() * 9 / 10 &&
                    bad * 100 < n;  // <1% differing, from the decoder warm-up
    std::printf("    %s\n", ok ? "PASS" : "FAIL");
    if (!ok) failures++;
  }

  // Loop: play packets [0, 4) three times (loop_count 2), then the rest.
  {
    const uint32_t loop_start = h0.first_frame_offset;
    uint32_t loop_end = 0;
    for (uint32_t p = 4; p < 8; p++) {
      const XmaPacketHeader h = XmaParsePacketHeader(data + p * kXmaPacketBytes);
      if (h.first_frame_offset <= kXmaMaxFrameBits) { loop_end = p * kXmaPacketBits + h.first_frame_offset; break; }
    }
    std::vector<int16_t> l = DecodeThroughEngine(data, e.play_length, int(e.channels), int(e.sample_rate), 2, loop_start, loop_end, false);
    Stats("engine (loop x3 over 4 packets)", l, int(e.channels), int(e.sample_rate));
    std::vector<int16_t> b0 = DecodeThroughEngine(data, e.play_length, int(e.channels), int(e.sample_rate), 0, 0, 0, false);
    // Frames in the loop body (the frame at loop_end is decoded too), by
    // walking the length prefixes the way the engine does.
    uint32_t body_frames = 0;
    {
      uint32_t p = 0, rel = h0.first_frame_offset;
      while (p * kXmaPacketBits + rel <= loop_end) {
        body_frames++;
        const uint8_t* pk = data + p * kXmaPacketBytes;
        const uint32_t avail = kXmaPacketBits - rel;
        uint32_t bits;
        if (avail >= kXmaFrameHeaderBits) bits = XmaPeekBits(pk, rel, kXmaFrameHeaderBits);
        else { uint8_t tmp[8] = {}; XmaCopyBits(tmp, 0, pk, rel, avail); XmaCopyBits(tmp, avail, pk + kXmaPacketBytes, kXmaPacketHeaderBits, kXmaFrameHeaderBits - avail); bits = XmaPeekBits(tmp, 0, kXmaFrameHeaderBits); }
        if (bits > avail) { p++; rel = XmaParsePacketHeader(data + p * kXmaPacketBytes).first_frame_offset; }
        else if (XmaPeekBits(pk, rel + bits - 1, 1)) rel += bits;
        else { p++; rel = XmaParsePacketHeader(data + p * kXmaPacketBytes).first_frame_offset; }
      }
    }
    const ptrdiff_t extra = ptrdiff_t(l.size()) - ptrdiff_t(b0.size());
    const ptrdiff_t expect = ptrdiff_t(2 * body_frames * kXmaSamplesPerFrame * e.channels);
    std::printf("    loop added %zd samples, expected %zd (%u frames x2)\n", extra, expect, body_frames);
    const bool ok = extra == expect;
    std::printf("    %s\n", ok ? "PASS" : "FAIL");
    if (!ok) failures++;
    WriteWav(stem + "_loop.wav", l, int(e.channels), int(e.sample_rate));
  }
  std::printf("%s\n", failures ? "FAILED" : "ALL PASS");
  return failures ? 1 : 0;
}
