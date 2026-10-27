// The codegen contract's runtime half: the indirect-call resolver, the kernel
// interrupt lock, and the MMIO seam.
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/mmio_handler.h>
#include <rex/thread/mutex.h>

#include "kernel/kernel.h"

namespace rex::runtime {

PPCFunc* ResolveIndirectFunction(uint32_t guest_address) {
  PPCFunc* f = xn::LookupFunction(guest_address);
  if (f) return f;
  // Report on the guest thread so the fault names its address; the guest
  // called a pointer that is not a function start.
  PPCContext* ctx = xn::CurrentContext();
  if (ctx) {
    REXLOG_ERROR("indirect call failed: guest_address={:#010x} lr={:#010x} r3={:#010x} r4={:#010x} r5={:#010x} r6={:#010x} r7={:#010x}",
                 guest_address, uint32_t(ctx->lr), uint32_t(ctx->r3.u32), uint32_t(ctx->r4.u32),
                 uint32_t(ctx->r5.u32), uint32_t(ctx->r6.u32), uint32_t(ctx->r7.u32));
  }
  xn::Fatal(std::format("indirect call to unmapped guest address {:#010x} (lr={:#010x})",
                        guest_address, ctx ? uint32_t(ctx->lr) : 0u));
}

void ReportIndirectDispatchStats() {}

MMIOHandler* MMIOHandler::global_handler() {
  static MMIOHandler handler;
  return &handler;
}

void MMIOHandler::RegisterRange(uint32_t address, uint32_t mask, uint32_t size, void* context,
                                MMIORead read, MMIOWrite write) {
  ranges_.push_back({address, mask, size, context, read, write});
}

bool MMIOHandler::CheckLoad(uint32_t address, uint32_t* out_value) {
  for (auto& r : ranges_) {
    if ((address & r.mask) == r.address) {
      *out_value = r.read(r.context, address);
      return true;
    }
  }
  static bool warned = false;
  if (!warned) {
    warned = true;
    REXCPU_WARN("MMIO load from unclaimed address {:#010x} (returns 0)", address);
  }
  *out_value = 0;
  return false;
}

bool MMIOHandler::CheckStore(uint32_t address, uint32_t value) {
  for (auto& r : ranges_) {
    if ((address & r.mask) == r.address) {
      r.write(r.context, address, value);
      return true;
    }
  }
  static bool warned = false;
  if (!warned) {
    warned = true;
    REXCPU_WARN("MMIO store to unclaimed address {:#010x} = {:#x} (ignored)", address, value);
  }
  return false;
}

}  // namespace rex::runtime

namespace rex::thread {
std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex m;
  return m;
}
}  // namespace rex::thread
