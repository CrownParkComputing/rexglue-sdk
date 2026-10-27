// Threads, events, waits, critical sections, spinlocks, IRQL, TLS.
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <rex/chrono/clock.h>
#include <rex/hook.h>

#include "kernel.h"

using namespace xn;

u32 InterlockedFlushSList_entry(u32 list) {
  // Xbox headers are eight bytes: big-endian next, depth, sequence. Detach
  // the whole header atomically, matching concurrent guest 64-bit CAS pushes.
  // Clearing only next would race producers and leave stale depth/sequence.
  auto header = std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(G(list)));
  const uint64_t detached = header.exchange(0, std::memory_order_acq_rel);
  return uint32_t(__builtin_bswap64(detached) >> 32);
}
XN_IMPORT(__imp__InterlockedFlushSList, InterlockedFlushSList_entry)

u32 KeSetDisableBoostThread_entry(u32 thread, u32 disabled) {
  if (!thread) return 0;
  // X_KTHREAD.boost_disabled is a byte at +0xBB; return the previous policy.
  return std::atomic_ref<uint8_t>(*G(thread + 0xBB)).exchange(uint8_t(disabled));
}
XN_IMPORT(__imp__KeSetDisableBoostThread, KeSetDisableBoostThread_entry)

u32 NtYieldExecution_entry() {
  static const bool trace = std::getenv("XN_TRACE_YIELDS") != nullptr;
  if (trace) {
    static thread_local uint64_t calls = 0;
    if ((++calls % 1000000) == 0) {
      const auto* context = CurrentContext();
      auto frame = context->r1.u32;
      REXKRNL_INFO("native yield tid={:#x} calls={} lr={:#x}", CurrentThread()->thread_id(), calls, uint32_t(context->lr));
      for (unsigned i = 0; frame && i < 5; ++i) {
        const auto parent = Load32(frame);
        if (parent <= frame || parent - frame > 0x10000) break;
        REXKRNL_INFO("native yield frame={} sp={:#x} caller_lr={:#x} words=[{:#x} {:#x} {:#x} {:#x} {:#x} {:#x}]",
            i, frame, Load32(parent - 8), Load32(frame + 0x50), Load32(frame + 0x54),
            Load32(frame + 0xE4), Load32(frame + 0xE8), Load32(frame + 0xEC), Load32(frame + 0xF0));
        if (Load32(parent - 8) == 0x8233EE94) {
          const auto queue = Load32(frame + 228);
          REXKRNL_INFO("Forza queue {:#x}: state={} produced={} pending={} limit={} read={} available={} callback={:#x}",
              queue, Load32(queue + 172), Load32(queue + 124), Load32(queue + 128), Load32(queue + 132),
              Load32(queue + 60), Load32(queue + 144), Load32(queue + 164));
        }
        frame = parent;
      }
      DumpWaits("yield diagnostic");
    }
  }
  std::this_thread::yield();
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtYieldExecution, NtYieldExecution_entry)

u32 KeQueryBasePriorityThread_entry(u32 thread) {
  return thread ? Load8(thread + kKthreadPriority) : 0;
}
XN_IMPORT(__imp__KeQueryBasePriorityThread, KeQueryBasePriorityThread_entry)

namespace {

thread_local uint8_t t_irql = 0;

std::shared_ptr<KObject> ObjectFromHandle(uint32_t handle) { return K().objects.Lookup(handle); }

uint32_t WaitOn(const std::shared_ptr<KObject>& object, const uint64_t* timeout_ptr) {
  if (!object) return X_STATUS_INVALID_HANDLE;
  auto timeout = TimeoutFromGuest(timeout_ptr);
  const bool trace_poll = timeout_ptr && *timeout_ptr == 0 && std::getenv("XN_TRACE_WAITS");
  if (trace_poll) {
    bool available = false;
    std::string state;
    {
      std::lock_guard lock(DispatcherMutex());
      available = object->PollLocked();
      state = object->DescribeLocked();
    }
    PPCContext* caller = CurrentContext();
    REXKRNL_INFO("zero-timeout wait handle={:#x} object={} guest={:#010x} state=[{}] "
                 "available={} caller_lr={:#010x} timeout_100ns=0",
                 object->handle(), object->name(), object->guest_object(), state, available,
                 caller ? uint32_t(caller->lr) : 0);
    if (caller && object->handle() == 0xBC) {
      uint32_t frame_sp = caller->r1.u32;
      for (uint32_t frame = 0; frame < 6 && frame_sp; ++frame) {
        uint32_t previous_sp = Load32(frame_sp);
        if (!previous_sp || previous_sp <= frame_sp || previous_sp - frame_sp > 0x10000) break;
        const uint32_t saved_lr = Load32(previous_sp - 8);
        REXKRNL_INFO("PGR4 wait stack frame={} sp={:#010x} caller_lr={:#010x}",
                     frame, frame_sp, saved_lr);
        frame_sp = previous_sp;
      }
    }
  }
  const uint32_t status = object->Wait(timeout) == WaitResult::Signaled ? X_STATUS_SUCCESS : X_STATUS_TIMEOUT;
  if (trace_poll) {
    REXKRNL_INFO("zero-timeout wait result handle={:#x} status={:#010x}", object->handle(), status);
  }
  return status;
}

}  // namespace

// ---- threads ---------------------------------------------------------------

u32 ExCreateThread_entry(mapped_u32 handle_ptr, u32 stack_size, mapped_u32 thread_id_ptr,
                         u32 xapi_thread_startup, u32 start_address, u32 start_context,
                         u32 creation_flags) {
  GuestThread::Params p;
  p.name = std::format("guest-{:08x}", start_address);
  p.stack_size = stack_size;
  p.start_address = start_address;
  p.start_context = start_context;
  p.xapi_startup = xapi_thread_startup;
  p.creation_flags = creation_flags;
  auto thread = CreateGuestThread(std::move(p));
  if (handle_ptr) *handle_ptr = (creation_flags & 0x80) ? thread->kthread() : thread->handle();
  if (thread_id_ptr) *thread_id_ptr = thread->thread_id();
  PPCContext* caller = CurrentContext();
  REXKRNL_INFO("ExCreateThread start={:#010x} ctx={:#010x} xapi={:#010x} flags={:#x} "
               "caller_lr={:#010x} -> tid {:#x}",
               start_address, start_context, xapi_thread_startup, creation_flags,
               caller ? uint32_t(caller->lr) : 0, thread->thread_id());
  // ExCreateThread is called through the generated xapilib wrapper
  // sub_8267E018. Its prologue saves LR at incoming r1-8, then allocates a
  // 0x60-byte frame, so the title-side return address is at current r1+0x58.
  // Do not read r1+4: that is not an LR save slot and can look like a code
  // address by accident.
  if (start_address == 0x822F6BA0 && caller) {
    uint32_t sp = caller->r1.u32;
    uint32_t wrapper_caller = Load32(sp + 0x58);
    REXKRNL_INFO("PGR4 error-worker created by guest caller_lr={:#010x} "
                 "(ExCreateThread wrapper frame sp={:#010x})",
                 wrapper_caller, sp);
  }
  if (creation_flags & 1) {
    REXKRNL_DEBUG("thread {:#x} created suspended; waiting for KeResumeThread/NtResumeThread",
                  thread->thread_id());
  }
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__ExCreateThread, ExCreateThread_entry)

XN_IMPORT_RAW(__imp__ExTerminateThread) {
  GuestThread* t = CurrentThread();
  int code = int(ctx.r3.u32);
  REXKRNL_INFO("ExTerminateThread({}) on tid {:#x}", code, t ? t->thread_id() : 0);
  if (t) t->MarkExited(code);
  // We cannot unwind the generated code's host frames; park this host thread.
  while (true) std::this_thread::sleep_for(std::chrono::hours(1));
}

static GuestThread* ThreadFromKthread(uint32_t kthread_ptr) {
  auto obj = K().objects.FromGuestHeader(kthread_ptr);
  return dynamic_cast<GuestThread*>(obj.get());
}

u32 KeSetAffinityThread_entry(u32 thread_ptr, u32 affinity, mapped_u32 previous_ptr) {
  GuestThread* t = ThreadFromKthread(thread_ptr);
  if (t && affinity) t->SetActiveCpu(uint8_t(__builtin_ctz(affinity)));
  if (previous_ptr) *previous_ptr = 1;
  return 1;
}
XN_IMPORT(__imp__KeSetAffinityThread, KeSetAffinityThread_entry)

u32 KeSetBasePriorityThread_entry(u32 thread_ptr, u32 increment) {
  if (thread_ptr) Store8(thread_ptr + kKthreadPriority, uint8_t(increment));
  return 0;
}
XN_IMPORT(__imp__KeSetBasePriorityThread, KeSetBasePriorityThread_entry)

u32 KeGetCurrentProcessType_entry() { return 1; }  // X_PROCTYPE_USER (title process)
XN_IMPORT(__imp__KeGetCurrentProcessType, KeGetCurrentProcessType_entry)

u32 KeDelayExecutionThread_entry(u32 processor_mode, u32 alertable, mapped_u64 interval_ptr) {
  int64_t interval = interval_ptr ? int64_t(uint64_t(*interval_ptr)) : 0;
  if (interval < 0) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(-interval * 100));
  } else if (interval == 0) {
    std::this_thread::yield();
  } else {
    auto t = TimeoutFromGuest(reinterpret_cast<const uint64_t*>(&interval));
    if (t && t->count() > 0) std::this_thread::sleep_for(*t);
  }
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__KeDelayExecutionThread, KeDelayExecutionThread_entry)

// ---- TLS ---------------------------------------------------------------------

u32 KeTlsAlloc_entry() {
  Kernel& k = K();
  for (uint32_t i = 0; i < k.tls_bitmap.size() * 32; i++) {
    uint32_t& word = k.tls_bitmap[i / 32];
    if (!(word & (1u << (i % 32)))) {
      word |= 1u << (i % 32);
      if (GuestThread* t = CurrentThread()) t->SetTls(i, 0);
      return i;
    }
  }
  return X_TLS_OUT_OF_INDEXES;
}
XN_IMPORT(__imp__KeTlsAlloc, KeTlsAlloc_entry)

u32 KeTlsFree_entry(u32 index) {
  Kernel& k = K();
  if (index / 32 < k.tls_bitmap.size()) k.tls_bitmap[index / 32] &= ~(1u << (index % 32));
  return 1;
}
XN_IMPORT(__imp__KeTlsFree, KeTlsFree_entry)

u32 KeTlsGetValue_entry(u32 index) {
  GuestThread* t = CurrentThread();
  return t ? t->GetTls(index) : 0;
}
XN_IMPORT(__imp__KeTlsGetValue, KeTlsGetValue_entry)

u32 KeTlsSetValue_entry(u32 index, u32 value) {
  GuestThread* t = CurrentThread();
  if (t) t->SetTls(index, value);
  return 1;
}
XN_IMPORT(__imp__KeTlsSetValue, KeTlsSetValue_entry)

// ---- events ------------------------------------------------------------------

u32 NtCreateEvent_entry(mapped_u32 handle_ptr, u32 obj_attributes, u32 event_type, u32 initial_state) {
  auto ev = std::make_shared<Event>(event_type == 0, initial_state != 0);
  // Give the event a guest-side dispatcher header too so KeWaitForSingleObject
  // on the object pointer (via ObReferenceObjectByHandle) works.
  uint32_t guest = K().arena.SystemAlloc(0x10, 16);
  Store8(guest, uint8_t(event_type));
  Store8(guest + 2, 4);
  Store32(guest + 4, initial_state != 0);
  uint32_t handle = K().objects.Add(ev, guest);
  if (handle_ptr) *handle_ptr = handle;
  if (std::getenv("XN_TRACE_WAITS")) {
    PPCContext* caller = CurrentContext();
    const uint32_t wrapper_caller = caller ? Load32(caller->r1.u32 + 0x88) : 0;
    REXKRNL_INFO("NtCreateEvent type={} initial={} guest={:#010x} -> handle={:#x} "
                 "caller_lr={:#010x} wrapper_saved_caller={:#010x}",
                 event_type, initial_state, guest, handle, caller ? uint32_t(caller->lr) : 0,
                 wrapper_caller);
    if (caller && guest == 0x7E005D00) {
      uint32_t frame_sp = caller->r1.u32;
      for (uint32_t frame = 0; frame < 8 && frame_sp; ++frame) {
        uint32_t previous_sp = Load32(frame_sp);
        if (!previous_sp || previous_sp <= frame_sp || previous_sp - frame_sp > 0x10000) break;
        const uint32_t saved_lr = Load32(previous_sp - 8);
        REXKRNL_INFO("PGR4 event-create stack frame={} sp={:#010x} caller_lr={:#010x}",
                     frame, frame_sp, saved_lr);
        frame_sp = previous_sp;
      }
    }
  }
  REXKRNL_DEBUG("NtCreateEvent type={} initial={} -> handle {:#x}", event_type, initial_state, handle);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtCreateEvent, NtCreateEvent_entry)

u32 NtSetEvent_entry(u32 handle, mapped_u32 previous_state_ptr) {
  auto ev = K().objects.Lookup<Event>(handle);
  if (!ev) return X_STATUS_INVALID_HANDLE;
  int32_t prev = ev->Set();
  if (previous_state_ptr) *previous_state_ptr = uint32_t(prev);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtSetEvent, NtSetEvent_entry)

u32 NtClearEvent_entry(u32 handle) {
  auto ev = K().objects.Lookup<Event>(handle);
  if (!ev) return X_STATUS_INVALID_HANDLE;
  ev->Reset();
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtClearEvent, NtClearEvent_entry)

u32 KeSetEvent_entry(u32 event_ptr, u32 increment, u32 wait) {
  auto obj = K().objects.FromGuestHeader(event_ptr);
  auto ev = std::dynamic_pointer_cast<Event>(obj);
  return ev ? uint32_t(ev->Set()) : 0;
}
XN_IMPORT(__imp__KeSetEvent, KeSetEvent_entry)

u32 KeResetEvent_entry(u32 event_ptr) {
  auto ev = std::dynamic_pointer_cast<Event>(K().objects.FromGuestHeader(event_ptr));
  return ev ? uint32_t(ev->Reset()) : 0;
}
XN_IMPORT(__imp__KeResetEvent, KeResetEvent_entry)

// ---- waits -------------------------------------------------------------------

u32 NtWaitForSingleObjectEx_entry(u32 handle, u32 wait_mode, u32 alertable, mapped_u64 timeout_ptr) {
  uint64_t timeout = timeout_ptr ? uint64_t(*timeout_ptr) : 0;
  return WaitOn(ObjectFromHandle(handle), timeout_ptr ? &timeout : nullptr);
}
XN_IMPORT(__imp__NtWaitForSingleObjectEx, NtWaitForSingleObjectEx_entry)

u32 KeWaitForSingleObject_entry(u32 object_ptr, u32 wait_reason, u32 processor_mode, u32 alertable,
                                mapped_u64 timeout_ptr) {
  uint64_t timeout = timeout_ptr ? uint64_t(*timeout_ptr) : 0;
  auto obj = K().objects.FromGuestHeader(object_ptr);
  if (!obj) return X_STATUS_SUCCESS;
  return WaitOn(obj, timeout_ptr ? &timeout : nullptr);
}
XN_IMPORT(__imp__KeWaitForSingleObject, KeWaitForSingleObject_entry)

// ---- critical sections (guest-visible state, host wait) ----------------------

static uint32_t CurrentKthread() {
  GuestThread* t = CurrentThread();
  return t ? t->kthread() : 0xFFFFFFFF;
}

static void CsInit(uint32_t guest_address, X_RTL_CRITICAL_SECTION* cs, uint32_t spin_count) {
  // Initialising in place means this is a NEW object, whatever used to live
  // here. Critical sections are routinely locals on a thread stack, and stacks
  // are reused, so without this the next Enter that blocks would find the dead
  // thread's Event - already signalled, or owned - and let two threads into
  // the section at once. That is what produced an intermittent boot failure in
  // Geometry Wars 2: two threads inside the xapilib heap, one writing the next
  // heap base while the other read it as an allocation size, giving a 1 GB
  // NtAllocateVirtualMemory and a fatal RtlRaiseException in half of all runs.
  K().objects.UnbindGuestHeader(guest_address);
  uint32_t spin = std::min<uint32_t>((spin_count + 255) >> 8, 255);
  cs->header.type = 1;
  cs->header.absolute = uint8_t(spin);
  cs->header.signal_state = 0;
  // The wait list starts empty; this also clears our binding stamp, so a
  // lookup that races the unbind above still sees the header is not ours.
  cs->header.wait_list_flink = 0;
  cs->header.wait_list_blink = 0;
  cs->lock_count = -1;
  cs->recursion_count = 0;
  cs->owning_thread = 0;
}

void RtlInitializeCriticalSection_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs) {
  CsInit(cs.guest_address(), cs, 0);
}
XN_IMPORT(__imp__RtlInitializeCriticalSection, RtlInitializeCriticalSection_entry)

u32 RtlInitializeCriticalSectionAndSpinCount_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs, u32 spin) {
  CsInit(cs.guest_address(), cs, spin);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__RtlInitializeCriticalSectionAndSpinCount, RtlInitializeCriticalSectionAndSpinCount_entry)

static bool CasLockCount(X_RTL_CRITICAL_SECTION* cs, int32_t expect, int32_t desired) {
  auto* p = reinterpret_cast<std::atomic<uint32_t>*>(&cs->lock_count);
  uint32_t e = __builtin_bswap32(uint32_t(expect));
  return p->compare_exchange_strong(e, __builtin_bswap32(uint32_t(desired)));
}
static int32_t AddLockCount(X_RTL_CRITICAL_SECTION* cs, int32_t delta) {
  // Guest field is big-endian; do the RMW under a CAS loop.
  auto* p = reinterpret_cast<std::atomic<uint32_t>*>(&cs->lock_count);
  uint32_t old = p->load();
  while (true) {
    int32_t v = int32_t(__builtin_bswap32(old));
    if (p->compare_exchange_weak(old, __builtin_bswap32(uint32_t(v + delta)))) return v + delta;
  }
}

void RtlEnterCriticalSection_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs) {
  uint32_t me = CurrentKthread();
  if (cs->owning_thread == me) {
    AddLockCount(cs, 1);
    cs->recursion_count = int32_t(cs->recursion_count) + 1;
    return;
  }
  uint32_t spin = uint32_t(cs->header.absolute) * 256;
  while (spin--) {
    if (CasLockCount(cs, -1, 0)) {
      cs->owning_thread = me;
      cs->recursion_count = 1;
      return;
    }
  }
  if (AddLockCount(cs, 1) != 0) {
    auto ev = std::dynamic_pointer_cast<Event>(K().objects.FromGuestHeader(cs.guest_address()));
    ev->Wait(std::nullopt);
  }
  cs->owning_thread = me;
  cs->recursion_count = 1;
}
XN_IMPORT(__imp__RtlEnterCriticalSection, RtlEnterCriticalSection_entry)

void RtlLeaveCriticalSection_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs) {
  int32_t rec = int32_t(cs->recursion_count);
  if (rec <= 0) {
    static bool warned = false;
    if (!warned) { warned = true; REXKRNL_WARN("RtlLeaveCriticalSection over-release at {:#010x}", cs.guest_address()); }
    cs->owning_thread = 0;
    cs->recursion_count = 0;
    if (int32_t(cs->lock_count) >= 0 && AddLockCount(cs, -1) != -1) {
      if (auto ev = std::dynamic_pointer_cast<Event>(K().objects.FromGuestHeader(cs.guest_address()))) ev->Set();
    }
    return;
  }
  if (--rec != 0) {
    cs->recursion_count = rec;
    AddLockCount(cs, -1);
    return;
  }
  cs->recursion_count = 0;
  cs->owning_thread = 0;
  if (AddLockCount(cs, -1) != -1) {
    // Someone is waiting: wake exactly one (synchronization event).
    if (auto ev = std::dynamic_pointer_cast<Event>(K().objects.FromGuestHeader(cs.guest_address()))) ev->Set();
  }
}
XN_IMPORT(__imp__RtlLeaveCriticalSection, RtlLeaveCriticalSection_entry)

u32 RtlTryEnterCriticalSection_entry(ppc_ptr_t<X_RTL_CRITICAL_SECTION> cs) {
  uint32_t me = CurrentKthread();
  if (CasLockCount(cs, -1, 0)) {
    cs->owning_thread = me;
    cs->recursion_count = 1;
    return 1;
  }
  if (cs->owning_thread == me) {
    AddLockCount(cs, 1);
    cs->recursion_count = int32_t(cs->recursion_count) + 1;
    return 1;
  }
  return 0;
}
XN_IMPORT(__imp__RtlTryEnterCriticalSection, RtlTryEnterCriticalSection_entry)

// ---- spinlocks / IRQL -----------------------------------------------------------
// The guest lock dword holds the owner's KPCR address (big-endian), like the
// real kernel; a plain CAS spin is enough because every holder is a host thread.

static void SpinAcquire(uint32_t lock_ptr) {
  auto* p = reinterpret_cast<std::atomic<uint32_t>*>(G(lock_ptr));
  uint32_t owner = __builtin_bswap32(uint32_t(CurrentContext()->r13.u32));
  uint32_t expect = 0;
  int spins = 0;
  while (!p->compare_exchange_weak(expect, owner, std::memory_order_acquire)) {
    expect = 0;
    if (++spins > 64) { std::this_thread::yield(); spins = 0; }
  }
}
static void SpinRelease(uint32_t lock_ptr) {
  reinterpret_cast<std::atomic<uint32_t>*>(G(lock_ptr))->store(0, std::memory_order_release);
}

u32 KeRaiseIrqlToDpcLevel_entry() { uint8_t old = t_irql; t_irql = 2; return old; }
XN_IMPORT(__imp__KeRaiseIrqlToDpcLevel, KeRaiseIrqlToDpcLevel_entry)
void KfLowerIrql_entry(u32 old) { t_irql = uint8_t(old); }
XN_IMPORT(__imp__KfLowerIrql, KfLowerIrql_entry)
u32 KfRaiseIrql_entry(u32 irql) { uint8_t old = t_irql; t_irql = uint8_t(irql); return old; }
XN_IMPORT(__imp__KfRaiseIrql, KfRaiseIrql_entry)

void KeAcquireSpinLockAtRaisedIrql_entry(u32 lock_ptr) { SpinAcquire(lock_ptr); }
XN_IMPORT(__imp__KeAcquireSpinLockAtRaisedIrql, KeAcquireSpinLockAtRaisedIrql_entry)
void KeReleaseSpinLockFromRaisedIrql_entry(u32 lock_ptr) { SpinRelease(lock_ptr); }
XN_IMPORT(__imp__KeReleaseSpinLockFromRaisedIrql, KeReleaseSpinLockFromRaisedIrql_entry)
u32 KeTryToAcquireSpinLockAtRaisedIrql_entry(u32 lock_ptr) {
  auto* p = reinterpret_cast<std::atomic<uint32_t>*>(G(lock_ptr));
  uint32_t expect = 0;
  return p->compare_exchange_strong(expect, __builtin_bswap32(uint32_t(CurrentContext()->r13.u32))) ? 1 : 0;
}
XN_IMPORT(__imp__KeTryToAcquireSpinLockAtRaisedIrql, KeTryToAcquireSpinLockAtRaisedIrql_entry)
u32 KfAcquireSpinLock_entry(u32 lock_ptr) {
  uint8_t old = t_irql;
  t_irql = 2;
  SpinAcquire(lock_ptr);
  return old;
}
XN_IMPORT(__imp__KfAcquireSpinLock, KfAcquireSpinLock_entry)
void KfReleaseSpinLock_entry(u32 lock_ptr, u32 old_irql) {
  SpinRelease(lock_ptr);
  t_irql = uint8_t(old_irql);
}
XN_IMPORT(__imp__KfReleaseSpinLock, KfReleaseSpinLock_entry)

void KeEnterCriticalRegion_entry() {}
XN_IMPORT(__imp__KeEnterCriticalRegion, KeEnterCriticalRegion_entry)
void KeLeaveCriticalRegion_entry() {}
XN_IMPORT(__imp__KeLeaveCriticalRegion, KeLeaveCriticalRegion_entry)

// ---- object references -------------------------------------------------------------

namespace {
// Forza creates unnamed custom dispatcher bodies through ObCreateObject,
// initializes their contents in guest code, and explicitly destroys those
// contents before its last ObDereferenceObject. Keep allocation/reference
// metadata off-body so its inline dispatcher header stays guest-owned.
struct PoolObject {
  uint32_t type, size, references;
  uint32_t dispatcher_handle = 0;
};
std::mutex pool_objects_mutex;
std::unordered_map<uint32_t, PoolObject> pool_objects;
// Each exposed handle owns one body reference. The internal dispatcher
// binding is removed separately when the final body reference goes away.
std::unordered_map<uint32_t, uint32_t> pool_handles;
}

u32 ObCreateObject_entry(u32 object_type, u32 attributes, u32 size, mapped_u32 output) {
  if (!output) return X_STATUS_INVALID_PARAMETER;
  *output = 0;
  if (!object_type || !size) return X_STATUS_INVALID_PARAMETER;
  if (attributes) return X_STATUS_NOT_IMPLEMENTED; // named objects need a namespace
  if (size > Arena::kSystemHeapSize - 16) return X_STATUS_NO_MEMORY;
  const uint32_t address = K().arena.SystemAlloc(size, 16);
  {
    std::lock_guard lock(pool_objects_mutex);
    pool_objects.emplace(address, PoolObject{object_type, size, 1});
  }
  *output = address;
  REXKRNL_DEBUG("ObCreateObject type={:#x} size={} -> {:#x}", object_type, size, address);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__ObCreateObject, ObCreateObject_entry)

void ObReferenceObject_entry(u32 address) {
  std::lock_guard lock(pool_objects_mutex);
  if (auto it = pool_objects.find(address); it != pool_objects.end()) ++it->second.references;
}
XN_IMPORT(__imp__ObReferenceObject, ObReferenceObject_entry)

u32 ObReferenceObjectByHandle_entry(u32 handle, u32 object_type_ptr, mapped_u32 out_object_ptr) {
  if (!out_object_ptr) return X_STATUS_INVALID_PARAMETER;
  *out_object_ptr = 0;
  std::lock_guard lock(pool_objects_mutex);
  auto obj = K().objects.Lookup(handle);
  if (!obj) return X_STATUS_INVALID_HANDLE;
  const uint32_t body = obj->guest_object();
  if (auto it = pool_objects.find(body); it != pool_objects.end()) {
    if (object_type_ptr && object_type_ptr != it->second.type) return 0xC0000024; // type mismatch
    ++it->second.references;
  }
  *out_object_ptr = body;
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__ObReferenceObjectByHandle, ObReferenceObjectByHandle_entry)

u32 ObOpenObjectByPointer_entry(u32 body, mapped_u32 output) {
  if (!output) return X_STATUS_INVALID_PARAMETER;
  *output = 0;
  if (!body || uint64_t(body) + sizeof(X_DISPATCH_HEADER) > 0x100000000ull)
    return X_STATUS_INVALID_PARAMETER;
  std::lock_guard lock(pool_objects_mutex);
  auto pool = pool_objects.find(body);
  if (pool != pool_objects.end() && pool->second.size < sizeof(X_DISPATCH_HEADER))
    return X_STATUS_INVALID_PARAMETER;
  auto object = K().objects.FromGuestHeader(body);
  if (!object) return X_STATUS_UNSUCCESSFUL;
  const uint32_t handle = K().objects.Add(object);
  if (pool != pool_objects.end()) {
    pool->second.dispatcher_handle = object->handle();
    ++pool->second.references;
    pool_handles.emplace(handle, body);
  }
  *output = handle;
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__ObOpenObjectByPointer, ObOpenObjectByPointer_entry)

u32 ObDereferenceObject_entry(u32 native_ptr) {
  bool release = false;
  uint32_t dispatcher_handle = 0;
  {
    std::lock_guard lock(pool_objects_mutex);
    auto it = pool_objects.find(native_ptr);
    if (it != pool_objects.end() && !--it->second.references) {
      dispatcher_handle = it->second.dispatcher_handle;
      pool_objects.erase(it);
      release = true;
    }
  }
  if (release) {
    K().objects.UnbindGuestHeader(native_ptr);
    if (dispatcher_handle) K().objects.Remove(dispatcher_handle);
    K().arena.SystemFree(native_ptr);
  }
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__ObDereferenceObject, ObDereferenceObject_entry)

u32 NtClose_entry(u32 handle) {
  uint32_t body = 0;
  {
    std::lock_guard lock(pool_objects_mutex);
    if (!K().objects.Lookup(handle)) return X_STATUS_INVALID_HANDLE;
    if (auto it = pool_handles.find(handle); it != pool_handles.end()) {
      body = it->second;
      pool_handles.erase(it);
    }
    K().objects.Remove(handle);
  }
  if (body) ObDereferenceObject_entry(body);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtClose, NtClose_entry)

u32 NtDuplicateObject_entry(u32 handle, mapped_u32 output, u32 options) {
  if (!output) return X_STATUS_INVALID_PARAMETER;
  *output = 0;
  {
    std::lock_guard lock(pool_objects_mutex);
    auto object = K().objects.Lookup(handle);
    if (!object) return X_STATUS_INVALID_HANDLE;
    const uint32_t duplicate = K().objects.Add(object);
    if (auto it = pool_handles.find(handle); it != pool_handles.end()) {
      const uint32_t body = it->second;
      ++pool_objects.at(body).references;
      pool_handles.emplace(duplicate, body);
    }
    *output = duplicate;
  }
  if (options & 1) NtClose_entry(handle); // DUPLICATE_CLOSE_SOURCE
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtDuplicateObject, NtDuplicateObject_entry)

u32 KiApcNormalRoutineNop_entry(u32, u32) { return 0; }
XN_IMPORT(__imp__KiApcNormalRoutineNop, KiApcNormalRoutineNop_entry)
