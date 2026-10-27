// xenon-native: guest-visible structures and status codes the imports touch.
// Layouts follow the public Xbox 360 kernel ABI (same as any 360 emulator's).
#pragma once
#include <cstdint>

#include <rex/types.h>

namespace xn {

using be32 = rex::be<uint32_t>;
using be16 = rex::be<uint16_t>;
using be64 = rex::be<uint64_t>;

constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
constexpr uint32_t X_STATUS_USER_APC = 0x000000C0;
constexpr uint32_t X_STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t X_STATUS_PENDING = 0x00000103;
constexpr uint32_t X_STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t X_STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t X_STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t X_STATUS_INVALID_INFO_CLASS = 0xC0000003;
constexpr uint32_t X_STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
constexpr uint32_t X_STATUS_BUFFER_TOO_SMALL = 0xC0000023;
constexpr uint32_t X_STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0;

constexpr uint32_t X_ERROR_SUCCESS = 0;
constexpr uint32_t X_ERROR_INVALID_PARAMETER = 0x57;
constexpr uint32_t X_ERROR_INSUFFICIENT_BUFFER = 0x7A;
constexpr uint32_t X_ERROR_IO_PENDING = 0x3E5;
constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 0x48F;
constexpr uint32_t X_ERROR_NOT_FOUND = 0x490;
constexpr uint32_t X_ERROR_NO_SUCH_USER = 0x525;
constexpr uint32_t X_ERROR_CANCELLED = 0x4C7;
constexpr uint32_t X_E_INVALIDARG = 0x80070057;
constexpr uint32_t X_E_NO_SUCH_USER = 0x80070525;

constexpr uint32_t X_INVALID_HANDLE_VALUE = 0xFFFFFFFF;
constexpr uint32_t X_TLS_OUT_OF_INDEXES = 0xFFFFFFFF;

// Memory flags (NtAllocateVirtualMemory / MmAllocatePhysicalMemoryEx).
constexpr uint32_t X_MEM_COMMIT = 0x00001000;
constexpr uint32_t X_MEM_RESERVE = 0x00002000;
constexpr uint32_t X_MEM_DECOMMIT = 0x00004000;
constexpr uint32_t X_MEM_RELEASE = 0x00008000;
constexpr uint32_t X_MEM_RESET = 0x00080000;
constexpr uint32_t X_MEM_TOP_DOWN = 0x00100000;
constexpr uint32_t X_MEM_NOZERO = 0x00800000;
constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000;
constexpr uint32_t X_MEM_16MB_PAGES = 0x80000000;
constexpr uint32_t X_PAGE_READONLY = 0x02;
constexpr uint32_t X_PAGE_READWRITE = 0x04;

struct X_DISPATCH_HEADER {
  uint8_t type;       // 0 notification event, 1 sync event, 5 semaphore, 6 thread ...
  uint8_t absolute;   // (critical sections keep spin count / 256 here)
  uint8_t size;
  uint8_t inserted;
  rex::be<int32_t> signal_state;
  be32 wait_list_flink;  // xenon-native: signature when bound to a host object
  be32 wait_list_blink;  // xenon-native: host object handle when bound
};
static_assert(sizeof(X_DISPATCH_HEADER) == 0x10);
constexpr uint32_t kBoundHeaderSignature = 0x584E4F42;  // 'XNOB'

struct X_RTL_CRITICAL_SECTION {
  X_DISPATCH_HEADER header;     // 0x00
  rex::be<int32_t> lock_count;  // 0x10  -1 = free
  rex::be<int32_t> recursion_count;  // 0x14
  be32 owning_thread;           // 0x18  KTHREAD guest pointer
};
static_assert(sizeof(X_RTL_CRITICAL_SECTION) == 0x1C);

struct X_ANSI_STRING {
  be16 length;
  be16 maximum_length;
  be32 pointer;
};
struct X_OBJECT_ATTRIBUTES {
  be32 root_directory;
  be32 name_ptr;
  be32 attributes;
};
struct X_IO_STATUS_BLOCK {
  be32 status;
  be32 information;
};
struct X_VIDEO_MODE {
  be32 display_width;
  be32 display_height;
  be32 is_interlaced;
  be32 is_widescreen;
  be32 is_hi_def;
  rex::be<float> refresh_rate;
  be32 video_standard;
  be32 unknown_0x8a;
  be32 unknown_0x01;
  be32 reserved[3];
};
struct X_INPUT_GAMEPAD {
  be16 buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
  rex::be<int16_t> thumb_lx;
  rex::be<int16_t> thumb_ly;
  rex::be<int16_t> thumb_rx;
  rex::be<int16_t> thumb_ry;
};
struct X_INPUT_STATE {
  be32 packet_number;
  X_INPUT_GAMEPAD gamepad;
};
struct XAM_OVERLAPPED {
  be32 result;
  be32 length;
  be32 context;
  be32 event;
  be32 completion_routine;
  be32 completion_context;
  be32 extended_error;
};

// KPCR / KTHREAD: only the fields the title (and inlined kernel helpers) read.
constexpr uint32_t kKpcrSize = 0x2D8;
constexpr uint32_t kKpcrTlsPtr = 0x000;
constexpr uint32_t kKpcrStackBase = 0x070;
constexpr uint32_t kKpcrStackEnd = 0x074;
constexpr uint32_t kKpcrPrcb = 0x100;               // X_KPRCB embedded here
constexpr uint32_t kKpcrCurrentThread = 0x100;      // prcb.current_thread
constexpr uint32_t kKpcrCurrentCpu = 0x10C;         // prcb.current_cpu (byte)
constexpr uint32_t kKpcrDpcActive = 0x150;          // prcb.dpc_active
constexpr uint32_t kKpcrPrcbPtr = 0x2A8;
constexpr uint32_t kKthreadSize = 0x200;
constexpr uint32_t kKthreadStackBase = 0x05C;
constexpr uint32_t kKthreadStackLimit = 0x060;
constexpr uint32_t kKthreadTlsAddress = 0x068;
constexpr uint32_t kKthreadPriority = 0x070;
constexpr uint32_t kKthreadProcessType = 0x073;
constexpr uint32_t kKthreadProcess = 0x084;
constexpr uint32_t kKthreadThreadId = 0x14C;
constexpr uint32_t kKthreadLastError = 0x160;

}  // namespace xn
