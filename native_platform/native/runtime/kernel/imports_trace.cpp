// Optional console event tracing. There are no ETX consumers in this runtime.
#include <algorithm>
#include <atomic>
#include <cstring>

#include <rex/hook.h>
#include "kernel.h"

using namespace xn;

// The retail Forza and SpeechFacade callers pass guid/events/count/flags in
// r3-r6, descriptor size in r7, and the descriptor output in r8. Their 64-byte
// descriptor contains event-enable masks read directly by guest code (+8 in
// Forza's registration at 0x827BB704). Keep those masks disabled and report the
// unavailable service. Do not claim to have registered a working trace sink.
XN_IMPORT_RAW(__imp__EtxProducerRegister) {
  constexpr uint32_t descriptor_size = 64;
  const uint32_t size = ctx.r7.u32, output = ctx.r8.u32;
  const uint32_t bytes = std::min(size, descriptor_size);
  if (!output || uint64_t(output) + bytes > Arena::kGuestSize) {
    ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
    return;
  }
  std::memset(G(output), 0, bytes);
  if (size < descriptor_size) {
    ctx.r3.u64 = X_STATUS_BUFFER_TOO_SMALL;
    return;
  }
  if (size != descriptor_size) {
    ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
    return;
  }
  static std::atomic<bool> announced{false};
  if (!announced.exchange(true, std::memory_order_relaxed))
    REXKRNL_INFO("ETX tracing unavailable: producer masks disabled (no native consumers)");
  ctx.r3.u64 = X_STATUS_NOT_IMPLEMENTED;
}

// No registration allocates resources. Unregistering an inactive producer is
// idempotent and needs no access to its descriptor or event payloads.
XN_IMPORT_RAW(__imp__EtxProducerUnregister) { ctx.r3.u64 = X_STATUS_SUCCESS; }

// Event varargs are deliberately not decoded when no consumer can accept them.
XN_IMPORT_RAW(__imp__EtxProducerLog) { ctx.r3.u64 = X_STATUS_NOT_IMPLEMENTED; }

// XStudio is absent on this host. Forza's request 1 at 0x827B7294 branches
// on a signed failure; success would install its developer-camera path. The
// request 6 status query has a DWORD output too. Other request blocks include
// input handles and must remain intact when reporting the unavailable service.
XN_IMPORT_RAW(__imp__XamXStudioRequest) {
  const uint32_t request = ctx.r3.u32, parameters = ctx.r4.u32;
  if (parameters && (request == 1 || request == 6)) {
    if (uint64_t(parameters) + 4 > Arena::kGuestSize) {
      ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
      return;
    }
    Store32(parameters, 0);
  }
  static std::atomic<bool> announced{false};
  if (!announced.exchange(true, std::memory_order_relaxed))
    REXKRNL_INFO("XStudio unavailable: developer requests return a signed failure");
  ctx.r3.u64 = X_STATUS_NOT_IMPLEMENTED;
}
