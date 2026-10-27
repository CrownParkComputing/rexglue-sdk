// Console configuration, locale, network (offline), video mode query.
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include <cerrno>
#include <sys/random.h>

#include <rex/hook.h>

#include "kernel.h"
#include "gfx/frame_rate.h"

using namespace xn;

// No sockets can be started while offline; cleanup has no resources to release.
u32 NetDll_WSACleanup_entry(u32 caller) { return 0; }
XN_IMPORT(__imp__NetDll_WSACleanup, NetDll_WSACleanup_entry)

namespace {
constexpr uint32_t kLanguageEnglish = 1;
constexpr uint32_t kGameRegionPAL = 0x0002;
constexpr uint32_t kAVRegionPAL50 = 0x00400100;
}  // namespace

u32 XGetLanguage_entry() { return kLanguageEnglish; }
XN_IMPORT(__imp__XGetLanguage, XGetLanguage_entry)
u32 XGetGameRegion_entry() { return kGameRegionPAL; }
XN_IMPORT(__imp__XGetGameRegion, XGetGameRegion_entry)

// ExGetXConfigSetting(category, setting, buffer, buffer_size, required_size*)
// Settings and sizes mirror the reference runtime's xconfig model. Mercury Hg
// reads language (3, 9) and country (3, 14) at startup; failing those left it
// with no locale and it crashed indexing its string tables.
XN_IMPORT_RAW(__imp__ExGetXConfigSetting) {
  uint32_t category = ctx.r3.u32, setting = ctx.r4.u32, buffer = ctx.r5.u32, size = ctx.r6.u32,
           required = ctx.r7.u32;
  uint32_t value = 0;
  uint32_t setting_size = 4;
  bool known = false;
  if (category == 2 && setting == 2) { value = kAVRegionPAL50; known = true; }  // secured AV region
  if (category == 3) {
    switch (setting) {
      case 9: value = kLanguageEnglish; known = true; break;   // user language
      case 10: value = 0; known = true; break;                 // user video flags
      case 11: value = 0x00010001; known = true; break;        // user audio flags
      case 12: value = 0x40; known = true; break;              // user retail flags
      case 14: value = 103; setting_size = 1; known = true; break;  // user country (US)
      case 25: value = 0x03; setting_size = 1; known = true; break; // user PC flags: XBL allowed
      default: break;
    }
  }
  // Answering an unknown setting with 0 and SUCCESS is indistinguishable, to
  // the caller, from a console that really holds 0 there. Write the buffer
  // either way (so nothing stale is left behind) but say which settings we
  // actually know, and fail the ones we do not.
  if (known && buffer && size < setting_size) {
    if (required) Store16(required, uint16_t(setting_size));
    ctx.r3.u64 = X_STATUS_BUFFER_TOO_SMALL;
    return;
  }
  if (buffer && size >= setting_size) {
    if (setting_size == 1) Store8(buffer, uint8_t(value));
    else Store32(buffer, known ? value : 0);
  }
  if (required) Store16(required, uint16_t(known ? setting_size : 4));
  if (!known) {
    static std::mutex seen_mutex;
    static std::vector<uint32_t> seen;
    uint32_t key = (category << 16) | setting;
    std::lock_guard lock(seen_mutex);
    if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
      seen.push_back(key);
      REXKRNL_WARN("ExGetXConfigSetting({}, {}) is not a setting we model; buffer zeroed and the "
                   "call failed rather than answering 0 as if it were real",
                   category, setting);
    }
    ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
    return;
  }
  ctx.r3.u64 = X_STATUS_SUCCESS;
}

// A disconnected console still initializes its networking library. Failing
// startup makes MCLA enter its fatal-error callback before reaching its menu.
// Link/address queries below communicate offline status independently.
u32 NetDll_XNetStartup_entry(u32 caller, u32 params) {
  if (params && Load8(params) != 13) return 10022; // WSAEINVAL
  return 0;
}
XN_IMPORT(__imp__NetDll_XNetStartup, NetDll_XNetStartup_entry)

u32 NetDll_WSAStartup_entry(u32 caller, u32 version, u32 data) {
  if (!data) return 10014; // WSAEFAULT
  // X_WSADATA: preserve the caller-owned vendor pointer at +0x190.
  std::memset(G(data), 0, 0x190);
  if ((version & 255) < 1 || (version & 255) > 2) return 10092;
  Store16(data, uint16_t(version));
  Store16(data + 2, 0x0202);
  return 0;
}
XN_IMPORT(__imp__NetDll_WSAStartup, NetDll_WSAStartup_entry)

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) { return 0; }
XN_IMPORT(__imp__NetDll_XNetGetEthernetLinkStatus, NetDll_XNetGetEthernetLinkStatus_entry)

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, u32 address) {
  if (address) std::memset(G(address), 0, 36);
  return 1; // XNET_GET_XNADDR_NONE; 0 means still pending.
}
XN_IMPORT(__imp__NetDll_XNetGetTitleXnAddr, NetDll_XNetGetTitleXnAddr_entry)

// PGR4 networking is intentionally offline. Winsock operations fail with the
// ordinary disconnected/not-socket answers; event creation is modelled with
// the shared event table so WSAWaitForMultipleEvents still blocks correctly.
u32 NetDll_WSACreateEvent_entry(u32 caller) {
  uint32_t guest = K().arena.SystemAlloc(sizeof(X_DISPATCH_HEADER), 16);
  K().arena.Fill(guest, sizeof(X_DISPATCH_HEADER), 0);
  auto event = std::make_shared<Event>(true, false);
  const uint32_t handle = K().objects.Add(event, guest);
  REXKRNL_DEBUG("NetDll_WSACreateEvent -> {:#x}", handle);
  return handle;
}
XN_IMPORT(__imp__NetDll_WSACreateEvent, NetDll_WSACreateEvent_entry)

u32 NetDll_WSACloseEvent_entry(u32 caller, u32 handle) {
  K().objects.Remove(handle);
  return 1;
}
XN_IMPORT(__imp__NetDll_WSACloseEvent, NetDll_WSACloseEvent_entry)

u32 NetDll_WSASetEvent_entry(u32 caller, u32 handle) {
  auto event = K().objects.Lookup<Event>(handle);
  if (!event) return 0;
  event->Set();
  return 1;
}
XN_IMPORT(__imp__NetDll_WSASetEvent, NetDll_WSASetEvent_entry)

u32 NetDll_WSAResetEvent_entry(u32 caller, u32 handle) {
  auto event = K().objects.Lookup<Event>(handle);
  if (!event) return 0;
  event->Reset();
  return 1;
}
XN_IMPORT(__imp__NetDll_WSAResetEvent, NetDll_WSAResetEvent_entry)

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 caller, u32 count, u32 handles,
                                          u32 wait_all, u32 timeout, u32 alertable) {
  if (!count || count > 64 || !handles) return 0xFFFFFFFFu;
  std::vector<std::shared_ptr<KObject>> events;
  events.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    auto event = K().objects.Lookup(Load32(handles + i * 4));
    if (!event) return 0xFFFFFFFFu;
    events.push_back(std::move(event));
  }
  const auto duration = timeout == 0xFFFFFFFFu
      ? std::optional<std::chrono::nanoseconds>{}
      : std::optional(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::milliseconds(timeout)));
  const size_t result = WaitMultiple(events, wait_all != 0, duration);
  return result == kWaitTimeout ? 0x102u /*WSA_WAIT_TIMEOUT*/ : uint32_t(result);
}
XN_IMPORT(__imp__NetDll_WSAWaitForMultipleEvents, NetDll_WSAWaitForMultipleEvents_entry)

u32 NetDll_WSAEventSelect_entry(u32 caller, u32 socket, u32 event, u32 network_events) {
  (void)caller; (void)socket; (void)event; (void)network_events;
  return 0;  // successful registration; no network events occur offline
}
XN_IMPORT(__imp__NetDll_WSAEventSelect, NetDll_WSAEventSelect_entry)

u32 NetDll_WSASetLastError_entry(u32 caller, u32 error) {
  if (GuestThread* thread = CurrentThread()) Store32(thread->kthread() + kKthreadLastError, error);
  return 0;
}
XN_IMPORT(__imp__NetDll_WSASetLastError, NetDll_WSASetLastError_entry)

u32 NetDll_WSAGetLastError_entry(u32 caller) {
  if (GuestThread* thread = CurrentThread()) return Load32(thread->kthread() + kKthreadLastError);
  return 0;
}
XN_IMPORT(__imp__NetDll_WSAGetLastError, NetDll_WSAGetLastError_entry)

u32 NetDll_WSACancelOverlappedIO_entry(u32 caller, u32 socket) {
  (void)caller; (void)socket;
  return 0;
}
XN_IMPORT(__imp__NetDll_WSACancelOverlappedIO, NetDll_WSACancelOverlappedIO_entry)

u32 NetDll_WSASend_entry(u32 caller, u32 socket, u32 buffers, u32 buffer_count,
                         u32 bytes_sent, u32 flags, u32 overlapped, u32 completion) {
  (void)caller; (void)socket; (void)buffers; (void)buffer_count; (void)flags;
  (void)overlapped; (void)completion;
  if (bytes_sent) Store32(bytes_sent, 0);
  return 10057; // WSAENOTCONN
}
XN_IMPORT(__imp__NetDll_WSASend, NetDll_WSASend_entry)

u32 NetDll_WSARecv_entry(u32 caller, u32 socket, u32 buffers, u32 buffer_count,
                         u32 bytes_received, u32 flags, u32 overlapped, u32 completion) {
  (void)caller; (void)socket; (void)buffers; (void)buffer_count; (void)flags;
  (void)overlapped; (void)completion;
  if (bytes_received) Store32(bytes_received, 0);
  return 10057; // WSAENOTCONN
}
XN_IMPORT(__imp__NetDll_WSARecv, NetDll_WSARecv_entry)

u32 NetDll_shutdown_entry(u32 caller, u32 socket, u32 how) {
  (void)caller; (void)socket; (void)how;
  return 10057; // WSAENOTCONN
}
XN_IMPORT(__imp__NetDll_shutdown, NetDll_shutdown_entry)

u32 NetDll_getpeername_entry(u32 caller, u32 socket, u32 address, u32 address_length) {
  (void)caller; (void)socket; (void)address;
  if (address_length) Store32(address_length, 0);
  return 10057;
}
XN_IMPORT(__imp__NetDll_getpeername, NetDll_getpeername_entry)

u32 NetDll_getsockname_entry(u32 caller, u32 socket, u32 address, u32 address_length) {
  (void)caller; (void)socket; (void)address;
  if (address_length) Store32(address_length, 0);
  return 10057;
}
XN_IMPORT(__imp__NetDll_getsockname, NetDll_getsockname_entry)

u32 NetDll_getsockopt_entry(u32 caller, u32 socket, u32 level, u32 option,
                            u32 value, u32 value_length) {
  (void)caller; (void)socket; (void)level; (void)option;
  if (value && value_length) std::memset(G(value), 0, Load32(value_length));
  return 10057;
}
XN_IMPORT(__imp__NetDll_getsockopt, NetDll_getsockopt_entry)

u32 NetDll_XNetServerToInAddr_entry(u32 caller, u32 server_id, u32 title_id, u32 address) {
  (void)caller; (void)server_id; (void)title_id;
  if (address) std::memset(G(address), 0, 16);
  return 1; // XNET_GET_XNADDR_NONE / no server mapping while offline
}
XN_IMPORT(__imp__NetDll_XNetServerToInAddr, NetDll_XNetServerToInAddr_entry)

u32 NetDll_XNetInAddrToString_entry(u32 caller, u32 address, u32 buffer, u32 length) {
  (void)caller; (void)address;
  if (buffer && length) Store8(buffer, 0);
  return 0;
}
XN_IMPORT(__imp__NetDll_XNetInAddrToString, NetDll_XNetInAddrToString_entry)

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, u32 address, mapped_u64 machine_id) {
  (void)caller; (void)address;
  if (machine_id) *machine_id = 0;
  return 1;
}
XN_IMPORT(__imp__NetDll_XNetXnAddrToMachineId, NetDll_XNetXnAddrToMachineId_entry)

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, u32 address) {
  (void)caller;
  if (address) std::memset(G(address), 0, 36);
  return 1;
}
XN_IMPORT(__imp__NetDll_XNetGetDebugXnAddr, NetDll_XNetGetDebugXnAddr_entry)

u32 NetDll___WSAFDIsSet_entry(u32 caller, u32 socket, u32 fd_set) {
  (void)caller; (void)socket; (void)fd_set;
  return 0;
}
XN_IMPORT(__imp__NetDll___WSAFDIsSet, NetDll___WSAFDIsSet_entry)

// Video mode as seen by XAM (same answer as VdQueryVideoMode).
void FillVideoMode(X_VIDEO_MODE* mode);
void XGetVideoMode_entry(ppc_ptr_t<X_VIDEO_MODE> mode) { FillVideoMode(mode); }
XN_IMPORT(__imp__XGetVideoMode, XGetVideoMode_entry)

// ---- second title -------------------------------------------------------------

// The A/V pack the console reports. HDMI, matching the 1280x720 mode
// VdQueryVideoMode answers; a title that asks usually only wants to know
// whether it is on a TV that can do widescreen/HD.
u32 XGetAVPack_entry() { return 0x010; }  // AV_PACK_HDMI
XN_IMPORT(__imp__XGetAVPack, XGetAVPack_entry)

// XEX module services are implemented in modules.cpp.

// --- Ion Assault Bring-up Stubs ---
XN_IMPORT_RAW(__imp__NetDll_XNetRegisterKey) { REXKRNL_WARN("NetDll_XNetRegisterKey stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_WSARecvFrom) { REXKRNL_WARN("NetDll_WSARecvFrom stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__ExFreePool) {
  REXKRNL_DEBUG("ExFreePool({:#010x})", ctx.r3.u32);
  K().arena.SystemFree(ctx.r3.u32);
  ctx.r3.u64 = 0;
}
XN_IMPORT_RAW(__imp__XUsbcamCreate) { REXKRNL_WARN("XUsbcamCreate stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NtQueueApcThread) { REXKRNL_WARN("NtQueueApcThread stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__MmUnlockAndUnmapSegmentArray) { REXKRNL_WARN("MmUnlockAndUnmapSegmentArray stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XUsbcamDestroy) { REXKRNL_WARN("XUsbcamDestroy stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XamUserAreUsersFriends) { REXKRNL_WARN("XamUserAreUsersFriends stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XUsbcamReadFrame) { REXKRNL_WARN("XUsbcamReadFrame stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_ioctlsocket) { REXKRNL_WARN("NetDll_ioctlsocket stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__ExInitializeReadWriteLock) { REXKRNL_WARN("ExInitializeReadWriteLock stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__ExReleaseReadWriteLock) { REXKRNL_WARN("ExReleaseReadWriteLock stubbed"); ctx.r3.u64 = 0; }
u32 NetDll_XNetRandom_entry(u32 caller, u32 buffer, u32 length) {
  if (!buffer && length) return 10014; // WSAEFAULT
  size_t done = 0;
  while (done < length) {
    auto count = getrandom(G(buffer) + done, length - done, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return 10022;
    done += size_t(count);
  }
  return 0;
}
XN_IMPORT(__imp__NetDll_XNetRandom, NetDll_XNetRandom_entry)
XN_IMPORT_RAW(__imp__ExAcquireReadWriteLockShared) { REXKRNL_WARN("ExAcquireReadWriteLockShared stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetQosLookup) { REXKRNL_WARN("NetDll_XNetQosLookup stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_socket) { REXKRNL_WARN("NetDll_socket stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XUsbcamSetConfig) { REXKRNL_WARN("XUsbcamSetConfig stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XUsbcamGetState) { REXKRNL_WARN("XUsbcamGetState stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_WSASendTo) { REXKRNL_WARN("NetDll_WSASendTo stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__ExAllocatePoolTypeWithTag) {
  const uint32_t size = ctx.r3.u32;
  const uint32_t tag = ctx.r4.u32;
  const uint32_t type = ctx.r5.u32;
  uint32_t address = K().arena.SystemAlloc(size, 16);
  REXKRNL_DEBUG("ExAllocatePoolTypeWithTag(size={:#x}, tag={:#x}, type={:#x}) -> {:#010x}",
                size, tag, type, address);
  ctx.r3.u64 = address;
}
XN_IMPORT_RAW(__imp__XamVoiceSubmitPacket) { REXKRNL_WARN("XamVoiceSubmitPacket stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__KeInsertQueueApc) { REXKRNL_WARN("KeInsertQueueApc stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_bind) { REXKRNL_WARN("NetDll_bind stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XamWriteGamerTile) { REXKRNL_WARN("XamWriteGamerTile stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_WSAGetOverlappedResult) { REXKRNL_WARN("NetDll_WSAGetOverlappedResult stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__ExAllocatePoolWithTag) {
  const uint32_t size = ctx.r3.u32;
  const uint32_t tag = ctx.r4.u32;
  uint32_t address = K().arena.SystemAlloc(size, 16);
  REXKRNL_DEBUG("ExAllocatePoolWithTag(size={:#x}, tag={:#x}) -> {:#010x}", size, tag, address);
  ctx.r3.u64 = address;
}
XN_IMPORT_RAW(__imp__KeInitializeApc) { REXKRNL_WARN("KeInitializeApc stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_recv) { REXKRNL_WARN("NetDll_recv stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetGetOpt) { REXKRNL_WARN("NetDll_XNetGetOpt stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_listen) { REXKRNL_WARN("NetDll_listen stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_send) { REXKRNL_WARN("NetDll_send stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetQosListen) { REXKRNL_WARN("NetDll_XNetQosListen stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetReplaceKey) { REXKRNL_WARN("NetDll_XNetReplaceKey stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XamVoiceHeadsetPresent) { REXKRNL_WARN("XamVoiceHeadsetPresent stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__FscSetCacheElementCount) { REXKRNL_WARN("FscSetCacheElementCount stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_accept) { REXKRNL_WARN("NetDll_accept stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_setsockopt) { REXKRNL_WARN("NetDll_setsockopt stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XamVoiceClose) { REXKRNL_WARN("XamVoiceClose stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XUsbcamSetCaptureMode) { REXKRNL_WARN("XUsbcamSetCaptureMode stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetQosRelease) { REXKRNL_WARN("NetDll_XNetQosRelease stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetUnregisterInAddr) { REXKRNL_WARN("NetDll_XNetUnregisterInAddr stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__MmMapIoSpace) { REXKRNL_WARN("MmMapIoSpace stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetConnect) { REXKRNL_WARN("NetDll_XNetConnect stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_recvfrom) { REXKRNL_WARN("NetDll_recvfrom stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__MmLockAndMapSegmentArray) { REXKRNL_WARN("MmLockAndMapSegmentArray stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetGetConnectStatus) { REXKRNL_WARN("NetDll_XNetGetConnectStatus stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XamParseGamerTileKey) { REXKRNL_WARN("XamParseGamerTileKey stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XamReadTileToTexture) { REXKRNL_WARN("XamReadTileToTexture stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetXnAddrToInAddr) { REXKRNL_WARN("NetDll_XNetXnAddrToInAddr stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetCleanup) { REXKRNL_WARN("NetDll_XNetCleanup stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetInAddrToXnAddr) { REXKRNL_WARN("NetDll_XNetInAddrToXnAddr stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_select) { REXKRNL_WARN("NetDll_select stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XUsbcamSetView) { REXKRNL_WARN("XUsbcamSetView stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_closesocket) { REXKRNL_WARN("NetDll_closesocket stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__ExAcquireReadWriteLockExclusive) { REXKRNL_WARN("ExAcquireReadWriteLockExclusive stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetDnsLookup) { REXKRNL_WARN("NetDll_XNetDnsLookup stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__KeSetCurrentProcessType) { REXKRNL_WARN("KeSetCurrentProcessType stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XamShowMessageBoxUI) { REXKRNL_WARN("XamShowMessageBoxUI stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetDnsRelease) { REXKRNL_WARN("NetDll_XNetDnsRelease stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__InterlockedPopEntrySList) { REXKRNL_WARN("InterlockedPopEntrySList stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__KeInsertQueueDpc) { REXKRNL_WARN("KeInsertQueueDpc stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_connect) { REXKRNL_WARN("NetDll_connect stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__KeInitializeDpc) { REXKRNL_WARN("KeInitializeDpc stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__XNetLogonGetTitleID) { REXKRNL_WARN("XNetLogonGetTitleID stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_inet_addr) { REXKRNL_WARN("NetDll_inet_addr stubbed"); ctx.r3.u64 = 0; }
// OutRun Online Arcade asks for the remaining Live setup calls before its
// frontend decides the network is unavailable. Answering 0 keeps the status
// codes in the family the other NetDll stubs use: the title sees a failed
// network and falls back to its offline paths.
XN_IMPORT_RAW(__imp__NetDll_sendto) { REXKRNL_WARN("NetDll_sendto stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetCreateKey) { REXKRNL_WARN("NetDll_XNetCreateKey stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetGetBroadcastVersionStatus) { REXKRNL_WARN("NetDll_XNetGetBroadcastVersionStatus stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__NetDll_XNetUnregisterKey) { REXKRNL_WARN("NetDll_XNetUnregisterKey stubbed"); ctx.r3.u64 = 0; }
XN_IMPORT_RAW(__imp__VdSetDisplayModeOverride) { REXKRNL_WARN("VdSetDisplayModeOverride stubbed"); ctx.r3.u64 = 0; }
