#include "audio/music_decoder.h"
#include "audio/sdl_output.h"

#include <algorithm>
#include <cstring>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace xn::audio {
namespace {
std::string AvError(int code) {
  char text[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(code, text, sizeof(text));
  return text;
}
}

struct MusicDecoder::Impl {
  AVFormatContext* format = nullptr;
  AVCodecContext* codec = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  SwrContext* resampler = nullptr;
  int stream = -1;
  bool flushing = false, eof = false;
  std::vector<float> pcm;
  size_t cursor = 0;
  ~Impl() {
    swr_free(&resampler);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
  }

  bool Fill(std::string& error) {
    pcm.clear();
    cursor = 0;
    while (!eof) {
      int result = avcodec_receive_frame(codec, frame);
      if (result == AVERROR(EAGAIN)) {
        if (flushing) { error = "decoder requested input after drain"; return false; }
        do {
          av_packet_unref(packet);
          result = av_read_frame(format, packet);
        } while (result >= 0 && packet->stream_index != stream);
        if (result < 0 && result != AVERROR_EOF) { error = AvError(result); return false; }
        flushing = result == AVERROR_EOF;
        result = avcodec_send_packet(codec, flushing ? nullptr : packet);
        av_packet_unref(packet);
        if (result < 0) { error = AvError(result); return false; }
        continue;
      }
      if (result < 0 && result != AVERROR_EOF) { error = AvError(result); return false; }
      const bool drain = result == AVERROR_EOF;
      if (!resampler) {
        if (drain) { eof = true; return false; }
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        result = swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLT,
            kOutputSampleRate, &frame->ch_layout, AVSampleFormat(frame->format),
            frame->sample_rate, 0, nullptr);
        if (result >= 0) result = swr_init(resampler);
        if (result < 0) { error = AvError(result); return false; }
      }
      const int capacity = std::max(256, swr_get_out_samples(resampler, drain ? 0 : frame->nb_samples));
      pcm.resize(size_t(capacity) * 2);
      uint8_t* output = reinterpret_cast<uint8_t*>(pcm.data());
      result = swr_convert(resampler, &output, capacity,
          drain ? nullptr : const_cast<const uint8_t**>(frame->extended_data),
          drain ? 0 : frame->nb_samples);
      av_frame_unref(frame);
      if (result < 0) { pcm.clear(); error = AvError(result); return false; }
      pcm.resize(size_t(result) * 2);
      if (drain && !result) eof = true;
      if (result) return true;
    }
    return false;
  }
};

MusicDecoder::MusicDecoder() = default;
MusicDecoder::~MusicDecoder() = default;
void MusicDecoder::Close() { impl_.reset(); }

bool MusicDecoder::Open(const std::string& path, std::string& error) {
  Close();
  error.clear();
  auto impl = std::make_unique<Impl>();
  int result = avformat_open_input(&impl->format, path.c_str(), nullptr, nullptr);
  if (result >= 0) result = avformat_find_stream_info(impl->format, nullptr);
  const AVCodec* decoder = nullptr;
  if (result >= 0) result = av_find_best_stream(impl->format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
  if (result < 0) { error = AvError(result); return false; }
  impl->stream = result;
  impl->codec = avcodec_alloc_context3(decoder);
  impl->packet = av_packet_alloc();
  impl->frame = av_frame_alloc();
  if (!impl->codec || !impl->packet || !impl->frame) { error = "out of memory"; return false; }
  result = avcodec_parameters_to_context(impl->codec, impl->format->streams[impl->stream]->codecpar);
  if (result >= 0) result = avcodec_open2(impl->codec, decoder, nullptr);
  if (result < 0) { error = AvError(result); return false; }
  // Prime the decoder here so a corrupt/empty file cannot report successful play.
  if (!impl->Fill(error)) {
    if (error.empty()) error = "audio file contains no samples";
    return false;
  }
  impl_ = std::move(impl);
  return true;
}

uint32_t MusicDecoder::Read(float* stereo, uint32_t frames, std::string& error) {
  error.clear();
  if (!impl_) return 0;
  uint32_t copied = 0;
  while (copied < frames) {
    if (impl_->cursor == impl_->pcm.size() && !impl_->Fill(error)) break;
    const size_t count = std::min(size_t(frames - copied) * 2, impl_->pcm.size() - impl_->cursor);
    std::memcpy(stereo + copied * 2, impl_->pcm.data() + impl_->cursor, count * sizeof(float));
    impl_->cursor += count;
    copied += uint32_t(count / 2);
  }
  return copied;
}
}  // namespace xn::audio
