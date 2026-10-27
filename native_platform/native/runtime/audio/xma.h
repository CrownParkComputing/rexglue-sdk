// xenon-native: the XMA hardware decoder, as the title sees it through the
// XMA* kernel imports (Option K in docs/AUDIO.md).
//
// The Xbox 360 XMA block is a table of 64-byte contexts in physical memory.
// The title (through xboxkrnl's XMA* imports) fills a context with the
// physical addresses of two input buffers of 2 KB XMA2 packets, an output ring
// of 256-byte blocks, loop points in bits, and valid bits; the block decodes
// frames into the ring and clears the valid bits as it consumes input. We keep
// the same table, the same bit layout and the same offset/valid contract; the
// WMA-Pro transform itself is libavcodec's xma2 decoder fed one XMA frame at a
// time (a synthetic one-frame packet), which lets loops and read offsets stay
// frame-exact the way the hardware behaves.
//
// Nothing here depends on the kernel: memory is reached through XmaMemory so
// the same engine runs offline against a host buffer (tests/xma_decode_test).
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace xn::audio {

// XMA2 stream geometry.
constexpr uint32_t kXmaPacketBytes = 2048;
constexpr uint32_t kXmaPacketBits = kXmaPacketBytes * 8;
constexpr uint32_t kXmaPacketHeaderBits = 32;
constexpr uint32_t kXmaFrameHeaderBits = 15;    // frame length prefix
constexpr uint32_t kXmaMaxFrameBits = 0x4000 - kXmaPacketHeaderBits;  // header offset limit
constexpr uint32_t kXmaSamplesPerFrame = 512;
constexpr uint32_t kXmaSamplesPerSubframe = 128;
constexpr uint32_t kXmaOutputBlockBytes = 256;  // one output block = 128 mono samples
constexpr uint32_t kXmaContextBytes = 64;
constexpr uint32_t kXmaContextCount = 320;      // ten 32-bit hardware context masks; MCLA exceeds 32
// The hardware output lags libavcodec's by this many samples (the encoder's
// overlap window); output is shifted so loop subframe indices line up.
constexpr uint32_t kXmaDecoderStartPadding = 192;

// The 64-byte hardware context. Guest memory holds it big-endian dword by
// dword; Load/Store swap. Bitfields are LSB-first within each dword after the
// swap, which is how every 360 emulator reads the block's registers.
struct XmaContextData {
  uint32_t input_buffer_0_packet_count : 12;
  uint32_t loop_count : 8;
  uint32_t input_buffer_0_valid : 1;
  uint32_t input_buffer_1_valid : 1;
  uint32_t output_buffer_block_count : 5;
  uint32_t output_buffer_write_offset : 5;

  uint32_t input_buffer_1_packet_count : 12;
  uint32_t loop_subframe_end : 2;
  uint32_t unk_dword_1_a : 3;
  uint32_t loop_subframe_skip : 3;
  uint32_t subframe_decode_count : 4;
  uint32_t output_buffer_padding : 3;
  uint32_t sample_rate : 2;   // 0 24000, 1 32000, 2 44100, 3 48000
  uint32_t is_stereo : 1;
  uint32_t unk_dword_1_c : 1;
  uint32_t output_buffer_valid : 1;

  uint32_t input_buffer_read_offset : 26;  // bits from the start of the current input buffer
  uint32_t error_status : 5;
  uint32_t error_set : 1;

  uint32_t loop_start : 26;
  uint32_t parser_error_status : 5;
  uint32_t parser_error_set : 1;

  uint32_t loop_end : 26;
  uint32_t packet_metadata : 5;
  uint32_t current_buffer : 1;

  uint32_t input_buffer_0_ptr;  // physical
  uint32_t input_buffer_1_ptr;  // physical
  uint32_t output_buffer_ptr;   // physical
  uint32_t work_buffer_ptr;

  uint32_t output_buffer_read_offset : 5;
  uint32_t : 25;
  uint32_t stop_when_done : 1;
  uint32_t interrupt_when_done : 1;

  uint32_t reserved[6];

  static XmaContextData Load(const void* guest);
  void Store(void* guest) const;
  bool InputValid(uint32_t which) const { return which ? input_buffer_1_valid : input_buffer_0_valid; }
  bool AnyInputValid() const { return input_buffer_0_valid || input_buffer_1_valid; }
  uint32_t InputPtr(uint32_t which) const { return which ? input_buffer_1_ptr : input_buffer_0_ptr; }
  uint32_t InputPackets(uint32_t which) const {
    return which ? input_buffer_1_packet_count : input_buffer_0_packet_count;
  }
};
static_assert(sizeof(XmaContextData) == kXmaContextBytes);

// XMA_CONTEXT_INIT as passed to XMAInitializeContext (56 bytes, big-endian).
struct XmaContextInit {
  uint32_t input_buffer_0_ptr;   // guest virtual
  uint32_t input_buffer_0_packet_count;
  uint32_t input_buffer_1_ptr;
  uint32_t input_buffer_1_packet_count;
  uint32_t input_buffer_read_offset;
  uint32_t output_buffer_ptr;
  uint32_t output_buffer_block_count;
  uint32_t work_buffer;
  uint32_t subframe_decode_count;
  uint32_t channel_count;        // 0 mono, >=1 stereo
  uint32_t sample_rate;          // enum, see XmaContextData::sample_rate
  uint32_t loop_start;
  uint32_t loop_end;
  uint8_t loop_count;
  uint8_t loop_subframe_end;
  uint8_t loop_subframe_skip;
  uint8_t pad;
  static XmaContextInit Load(const void* guest);
};
static_assert(sizeof(XmaContextInit) == 56);

// XMA_LOOP_DATA as XMASetLoopData receives it: 12 bytes, big-endian. Confirmed
// against the recompiled caller (geometrywars_recomp.6.cpp, 0x8207C2A4 and
// 0x8207C4xx): it builds the block at r1+80 with stw loop_start, stw loop_end,
// then three stb. It is NOT an XMA_CONTEXT_DATA - reading it as one picks up
// whatever is on the guest stack past the struct.
struct XmaLoopData {
  uint32_t loop_start = 0;   // bits
  uint32_t loop_end = 0;     // bits
  uint8_t loop_count = 0;    // 0 none, 255 infinite
  uint8_t loop_subframe_end = 0;
  uint8_t loop_subframe_skip = 0;
  static XmaLoopData Load(const void* guest);
};

inline int XmaSampleRate(uint32_t id) {
  static const int rates[4] = {24000, 32000, 44100, 48000};
  return rates[id & 3];
}

// XMA2 packet header (4 bytes, big-endian bit order).
struct XmaPacketHeader {
  uint32_t frame_count;        // 6 bits
  uint32_t first_frame_offset; // 15 bits, + 32 -> bit offset of the first frame in the packet
  uint32_t metadata;           // 3 bits, 1 = XMA2
  uint32_t skip_count;         // 8 bits, packets to skip before the next of this stream; 0xFF = none
};
XmaPacketHeader XmaParsePacketHeader(const uint8_t* packet);

// MSB-first bit access over a byte buffer.
uint32_t XmaPeekBits(const uint8_t* data, uint32_t bit_offset, uint32_t count);  // count <= 32
void XmaCopyBits(uint8_t* dst, uint32_t dst_bit, const uint8_t* src, uint32_t src_bit, uint32_t count);

// How the engine reaches guest memory.
struct XmaMemory {
  virtual ~XmaMemory() = default;
  virtual uint8_t* Physical(uint32_t phys) = 0;
};

// One libavcodec xma2 decoder fed single frames.
//
// libavcodec's xma2 wrapper drops the first frame it decodes (no overlap
// history) and then holds back 4096 samples (8 frames) in an internal FIFO,
// so output for fed frame k arrives after frame k+8 is fed. The context keeps
// per-frame loop metadata in step with that pipeline; Drain() releases the
// held frames when input runs out (the codec is reopened afterwards).
class XmaFrameDecoder {
 public:
  XmaFrameDecoder();
  ~XmaFrameDecoder();
  // (Re)opens the codec for a stream shape; returns true when it was reopened.
  bool Prepare(int sample_rate, bool stereo);
  void Reset();
  bool open() const { return ctx_ != nullptr; }
  // frame is the frame including its 15-bit length prefix; the trailing
  // "more frames" bit is rewritten so the codec sees a one-frame packet.
  // Whatever the codec releases (0, 512 or more samples per channel) is
  // appended to out as interleaved host-order int16. Returns false when the
  // codec rejected the frame.
  bool Decode(const uint8_t* frame, uint32_t frame_bits, std::vector<int16_t>& out);
  // Flushes the held-back samples into out and closes the codec.
  void Drain(std::vector<int16_t>& out);
  int sample_rate() const { return sample_rate_; }
  bool stereo() const { return stereo_; }
  uint32_t frames_fed() const { return frames_fed_; }

 private:
  void Receive(std::vector<int16_t>& out);
  const AVCodec* codec_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVFrame* av_frame_ = nullptr;
  AVPacket* av_packet_ = nullptr;
  int sample_rate_ = 0;
  bool stereo_ = false;
  uint32_t frames_fed_ = 0;
  std::array<uint8_t, kXmaPacketBytes> packet_{};
};

class XmaContext {
 public:
  void Setup(uint32_t id, XmaMemory* memory, uint8_t* guest_context);
  uint32_t id() const { return id_; }
  uint8_t* guest() const { return guest_; }

  // Counters for XN_AUDIO_STATS (all monotonic, relaxed).
  struct Stats {
    uint64_t passes = 0;         // Work() calls that ran
    uint64_t blocks_written = 0; // 256-byte blocks pushed into the ring
    uint64_t frames_decoded = 0; // frames taken from the codec
    uint64_t frames_fed = 0;     // frames handed to the codec
    uint64_t swaps = 0;          // input buffer switches
    uint64_t drains = 0;         // codec drains (input ran dry)
    uint64_t resets = 0;         // codec (re)opens
    uint64_t loops = 0;          // loop-end jumps taken
    uint64_t errors = 0;         // passes that set error_status
    uint64_t ring_full = 0;      // passes that filled the ring
    uint64_t loop_inexact = 0;   // loop points that did not fall on a frame boundary
  };
  Stats stats() const;
  // One line of context register state, for logs.
  std::string Describe() const;

  bool allocated() const { return allocated_.load(std::memory_order_acquire); }
  bool enabled() const { return enabled_.load(std::memory_order_acquire); }
  // True while a decode pass is inside Work() and may be writing the 64-byte
  // guest context. A title that drives the hardware through its registers
  // (rather than the XMA* imports) polls for exactly this before copying the
  // context out; see XmaRegisterFile.
  bool working() const { return working_.load(std::memory_order_acquire); }
  void set_allocated(bool v) { allocated_.store(v, std::memory_order_release); }

  void Enable();
  void Disable();   // waits for a running pass
  void Block();     // XMABlockWhileInUse: returns once no pass is running
  void Clear();     // XMA context clear register: drop valid bits and offsets
  void Release();
  // One decode pass: fills the output ring as far as input and space allow.
  // Returns true when anything was written or consumed.
  bool Work();

 private:
  struct FrameMeta {
    uint8_t output_limit;  // blocks to emit from this frame, 0 = all (loop end)
    uint8_t start_skip;    // blocks to drop at the start (loop start)
  };
  bool DecodeNextFrame(XmaContextData& d);
  bool TakeRawFrame(const XmaContextData& d);
  uint32_t NextPacketFrameOffset(const uint8_t* buffer, uint32_t packet_index, uint32_t packet_count);
  void SwapInputBuffer(XmaContextData& d);
  void ResetStream();

  uint32_t id_ = 0;
  XmaMemory* memory_ = nullptr;
  uint8_t* guest_ = nullptr;
  std::mutex lock_;
  std::atomic<bool> allocated_{false};
  std::atomic<bool> enabled_{false};
  std::atomic<bool> working_{false};

  XmaFrameDecoder decoder_;
  std::array<uint8_t, kXmaPacketBytes> frame_buf_{};
  // Codec output stream (interleaved int16) and the loop metadata of the
  // frames still inside the codec / not yet emitted, oldest first.
  std::vector<int16_t> pcm_;
  size_t pcm_head_ = 0;
  bool lead_dropped_ = false;  // kXmaDecoderStartPadding removed from the stream
  std::deque<FrameMeta> meta_;
  bool loop_start_skip_pending_ = false;
  // The frame being drained into the ring, in 256-byte blocks (big-endian int16).
  std::array<uint8_t, kXmaSamplesPerFrame * 2 * 2> raw_frame_{};
  uint8_t raw_blocks_total_ = 0;
  uint8_t raw_blocks_next_ = 0;   // next block index to write
  uint8_t raw_blocks_limit_ = 0;  // 0 = no loop limit
  mutable std::atomic<uint64_t> s_passes_{0}, s_blocks_{0}, s_decoded_{0}, s_fed_{0}, s_swaps_{0},
      s_drains_{0}, s_resets_{0}, s_loops_{0}, s_errors_{0}, s_full_{0}, s_loop_inexact_{0};
};

class XmaEngine {
 public:
  ~XmaEngine();
  // guest_table: kXmaContextCount * 64 bytes of guest memory (host pointer);
  // guest_table_address: what XMACreateContext hands the title for slot 0.
  void Setup(XmaMemory* memory, uint8_t* guest_table, uint32_t guest_table_address);
  void Shutdown();
  // Guest address of the new context, 0 when the table is full.
  uint32_t AllocateContext();
  void ReleaseContext(uint32_t guest_address);
  XmaContext* Lookup(uint32_t guest_address);
  // Kick: enable and wake the worker. The pass runs on the worker thread.
  void Enable(uint32_t guest_address);
  // Runs one decode pass on one context on the caller's thread. A title that
  // drives the register block kicks and then locks 11 us later (measured on
  // Geometry Wars 2), which no worker sweep can meet: the hardware decodes as
  // soon as the kick lands, so the kick path calls this to do the same.
  bool WorkContext(uint32_t guest_address);
  void Disable(uint32_t guest_address);
  // Index of a context currently inside a decode pass, or -1 when idle. This
  // is what the hardware's "current context index" register reports.
  int32_t WorkingContextIndex() const;
  // Runs pending work on the caller (used by the offline test; the runtime
  // uses the worker thread instead).
  bool WorkAll();
  uint32_t table_address() const { return table_address_; }
  // Allocated contexts, for logs.
  std::vector<XmaContext*> AllocatedContexts();
  uint64_t worker_sweeps() const { return sweeps_.load(std::memory_order_relaxed); }

 private:
  void WorkerMain();
  XmaMemory* memory_ = nullptr;
  uint8_t* table_ = nullptr;
  uint32_t table_address_ = 0;
  std::array<XmaContext, kXmaContextCount> contexts_;
  std::mutex alloc_lock_;
  std::thread worker_;
  std::atomic<bool> running_{false};
  std::mutex wake_lock_;
  std::condition_variable wake_;
  bool kicked_ = false;
  std::atomic<uint64_t> sweeps_{0};
};

}  // namespace xn::audio
