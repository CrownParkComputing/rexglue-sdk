// Kernel objects the second title needs and the first did not: semaphores,
// mutants, waiting on several objects at once, counted suspend/resume, and the
// title-terminate notification list.
//
// Geometry Wars 2 links a Win32-shaped OS layer under its audio library
// (0x82140430-0x82141000) plus a thread pool (0x82150438) that creates six
// workers suspended and resumes them, so these are real objects with real
// state, not stubs. The multi-object wait is in runtime/kernel/objects.cpp
// (WaitMultiple); see docs/geometrywars2/SURFACE.md section 1.2.
#include <vector>

#include <rex/hook.h>

#include "kernel.h"

using namespace xn;

namespace {

// Guest KSEMAPHORE: dispatcher header then the limit.
constexpr uint32_t kSemaphoreLimitOffset = 0x10;
constexpr uint32_t kSemaphoreSize = 0x14;
// Guest KMUTANT: header, list entry, owner, abandoned. We only keep the header
// meaningful; the rest is zeroed so guest code that peeks sees a consistent
// "free" object.
constexpr uint32_t kMutantSize = 0x20;

constexpr uint32_t X_STATUS_MUTANT_NOT_OWNED = 0xC0000046;
constexpr uint32_t X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;

// Allocates a guest dispatcher object so KeWaitForSingleObject / the Ke* forms
// can reach the same host object by pointer, exactly as NtCreateEvent does.
uint32_t NewGuestDispatcher(uint8_t type, uint32_t size, int32_t signal_state) {
  uint32_t guest = K().arena.SystemAlloc(size, 16);
  K().arena.Fill(guest, size, 0);
  Store8(guest, type);
  Store8(guest + 2, uint8_t(size / 4));
  auto* h = reinterpret_cast<X_DISPATCH_HEADER*>(G(guest));
  h->signal_state = signal_state;
  return guest;
}

std::shared_ptr<KObject> SemaphoreFromGuest(uint32_t guest_ptr) {
  return K().objects.FromGuestHeader(guest_ptr);
}

}  // namespace

// ---- semaphores --------------------------------------------------------------

void KeInitializeSemaphore_entry(u32 semaphore_ptr, u32 count, u32 limit) {
  // Same reuse hazard as RtlInitializeCriticalSection: drop whatever host
  // object was bound to this address before.
  K().objects.UnbindGuestHeader(semaphore_ptr);
  auto* h = reinterpret_cast<X_DISPATCH_HEADER*>(G(semaphore_ptr));
  h->type = 5;
  h->absolute = 0;
  h->size = uint8_t(kSemaphoreSize / 4);
  h->inserted = 0;
  h->signal_state = int32_t(count);
  h->wait_list_flink = 0;
  h->wait_list_blink = 0;
  Store32(semaphore_ptr + kSemaphoreLimitOffset, limit);
  // Bind a host object now so a later wait by pointer finds this exact state
  // rather than re-deriving it after the guest has changed the count.
  auto sem = std::make_shared<Semaphore>(int32_t(count), int32_t(limit));
  K().objects.Add(sem, semaphore_ptr);
  REXKRNL_DEBUG("KeInitializeSemaphore {:#010x} count={} limit={}", semaphore_ptr, count, limit);
}
XN_IMPORT(__imp__KeInitializeSemaphore, KeInitializeSemaphore_entry)

u32 KeReleaseSemaphore_entry(u32 semaphore_ptr, u32 increment, u32 adjustment, u32 wait) {
  auto sem = std::dynamic_pointer_cast<Semaphore>(SemaphoreFromGuest(semaphore_ptr));
  if (!sem) return 0;
  int32_t prev = sem->Release(int32_t(adjustment ? adjustment : 1));
  return prev < 0 ? 0 : uint32_t(prev);
}
XN_IMPORT(__imp__KeReleaseSemaphore, KeReleaseSemaphore_entry)

u32 NtCreateSemaphore_entry(mapped_u32 handle_out, u32 obj_attributes, u32 count, u32 limit) {
  int32_t l = int32_t(limit);
  uint32_t guest = NewGuestDispatcher(5, kSemaphoreSize, int32_t(count));
  Store32(guest + kSemaphoreLimitOffset, limit);
  auto sem = std::make_shared<Semaphore>(int32_t(count), l);
  uint32_t handle = K().objects.Add(sem, guest);
  if (handle_out) *handle_out = handle;
  REXKRNL_DEBUG("NtCreateSemaphore count={} limit={} -> handle {:#x}", count, limit, handle);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtCreateSemaphore, NtCreateSemaphore_entry)

u32 NtReleaseSemaphore_entry(u32 handle, u32 release_count, mapped_u32 previous_count_ptr) {
  auto sem = K().objects.Lookup<Semaphore>(handle);
  if (!sem) return X_STATUS_INVALID_HANDLE;
  int32_t prev = sem->Release(int32_t(release_count ? release_count : 1));
  if (prev < 0) return X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
  if (previous_count_ptr) *previous_count_ptr = uint32_t(prev);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtReleaseSemaphore, NtReleaseSemaphore_entry)

// ---- mutants -----------------------------------------------------------------

u32 NtCreateMutant_entry(mapped_u32 handle_out, u32 obj_attributes, u32 initial_owner) {
  GuestThread* t = CurrentThread();
  uint32_t owner = initial_owner && t ? t->kthread() : 0;
  uint32_t guest = NewGuestDispatcher(2, kMutantSize, owner ? 0 : 1);
  auto m = std::make_shared<Mutant>(owner);
  uint32_t handle = K().objects.Add(m, guest);
  if (handle_out) *handle_out = handle;
  REXKRNL_DEBUG("NtCreateMutant owned={} -> handle {:#x}", initial_owner, handle);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtCreateMutant, NtCreateMutant_entry)

// NtReleaseMutant(handle, unknown). There is no previous-count out pointer on
// the 360: the guest's CRT unlock passes exactly two arguments, so an import
// that reads a third/fourth picks up stale registers and writes through a wild
// pointer. That corrupted Mercury Hg's CRT pool freelist (previous recursion
// count 1 stored into a free block), which then "allocated" address 1.
u32 NtReleaseMutant_entry(u32 handle, u32 unknown) {
  auto m = K().objects.Lookup<Mutant>(handle);
  if (!m) return X_STATUS_INVALID_HANDLE;
  int32_t prev = m->Release(false);
  if (prev < 0) return X_STATUS_MUTANT_NOT_OWNED;
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtReleaseMutant, NtReleaseMutant_entry)

// ---- waiting on several objects ------------------------------------------------

// wait_type 0 = WaitAll, 1 = WaitAny. On success WaitAny returns the index of
// the object that was taken; WaitAll returns 0.
u32 KeWaitForMultipleObjects_entry(u32 count, u32 objects_ptr, u32 wait_type, u32 wait_reason,
                                   u32 processor_mode, u32 alertable, mapped_u64 timeout_ptr,
                                   u32 wait_block_array) {
  std::vector<std::shared_ptr<KObject>> objects;
  objects.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t guest = Load32(objects_ptr + i * 4);
    auto obj = K().objects.FromGuestHeader(guest);
    if (!obj) {
      REXKRNL_WARN("KeWaitForMultipleObjects: object {} at {:#010x} is not a modelled dispatcher",
                   i, guest);
      return X_STATUS_INVALID_PARAMETER;
    }
    objects.push_back(std::move(obj));
  }
  uint64_t timeout = timeout_ptr ? uint64_t(*timeout_ptr) : 0;
  auto t = TimeoutFromGuest(timeout_ptr ? &timeout : nullptr);
  size_t index = WaitMultiple(objects, wait_type == 0, t);
  return index == kWaitTimeout ? X_STATUS_TIMEOUT : uint32_t(index);
}
XN_IMPORT(__imp__KeWaitForMultipleObjects, KeWaitForMultipleObjects_entry)

u32 NtWaitForMultipleObjectsEx_entry(u32 count, u32 handles, u32 wait_type,
                                     u32 wait_mode, u32 alertable, mapped_u64 timeout_ptr) {
  if (!count || count > 64 || !handles || wait_type > 1) return X_STATUS_INVALID_PARAMETER;
  std::vector<std::shared_ptr<KObject>> objects;
  objects.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    auto object = K().objects.Lookup(Load32(handles + i * 4));
    if (!object) return X_STATUS_INVALID_HANDLE;
    objects.push_back(std::move(object));
  }
  uint64_t timeout = timeout_ptr ? uint64_t(*timeout_ptr) : 0;
  const size_t result = WaitMultiple(objects, wait_type == 0,
                                    TimeoutFromGuest(timeout_ptr ? &timeout : nullptr));
  return result == kWaitTimeout ? X_STATUS_TIMEOUT : uint32_t(result);
}
XN_IMPORT(__imp__NtWaitForMultipleObjectsEx, NtWaitForMultipleObjectsEx_entry)

// Signal one object and wait on another, with no window between the two where
// the caller is neither signalling nor waiting - the guest's SignalObjectAndWait.
u32 NtSignalAndWaitForSingleObjectEx_entry(u32 signal_handle, u32 wait_handle, u32 wait_mode,
                                           u32 alertable, mapped_u64 timeout_ptr) {
  auto signal = K().objects.Lookup(signal_handle);
  auto wait = K().objects.Lookup(wait_handle);
  if (!signal || !wait) return X_STATUS_INVALID_HANDLE;
  // Signal first, then wait. The two are not one atomic step here; the guest
  // uses this for a worker handshake where the only effect of the gap is an
  // extra wakeup, never a lost one, because the object being signalled is what
  // the peer is waiting on.
  if (auto ev = std::dynamic_pointer_cast<Event>(signal)) {
    ev->Set();
  } else if (auto sem = std::dynamic_pointer_cast<Semaphore>(signal)) {
    sem->Release(1);
  } else if (auto m = std::dynamic_pointer_cast<Mutant>(signal)) {
    m->Release(false);
  } else {
    REXKRNL_WARN("NtSignalAndWaitForSingleObjectEx: cannot signal a {}", signal->name());
    return X_STATUS_INVALID_PARAMETER;
  }
  uint64_t timeout = timeout_ptr ? uint64_t(*timeout_ptr) : 0;
  auto t = TimeoutFromGuest(timeout_ptr ? &timeout : nullptr);
  return wait->Wait(t) == WaitResult::Signaled ? X_STATUS_SUCCESS : X_STATUS_TIMEOUT;
}
XN_IMPORT(__imp__NtSignalAndWaitForSingleObjectEx, NtSignalAndWaitForSingleObjectEx_entry)

// ---- suspend / resume -----------------------------------------------------------

namespace {
GuestThread* ThreadFromKthreadPtr(uint32_t kthread_ptr) {
  auto obj = K().objects.FromGuestHeader(kthread_ptr);
  return dynamic_cast<GuestThread*>(obj.get());
}
}  // namespace

u32 KeResumeThread_entry(u32 thread_ptr) {
  GuestThread* t = ThreadFromKthreadPtr(thread_ptr);
  if (!t) return 0;
  int32_t prev = t->ResumeCounted();
  REXKRNL_DEBUG("KeResumeThread {:#010x} (tid {:#x}) previous suspend count {}", thread_ptr,
                t->thread_id(), prev);
  return uint32_t(prev);
}
XN_IMPORT(__imp__KeResumeThread, KeResumeThread_entry)

u32 NtResumeThread_entry(u32 handle, mapped_u32 suspend_count_out) {
  auto t = K().objects.Lookup<GuestThread>(handle);
  if (!t) return X_STATUS_INVALID_HANDLE;
  REXKRNL_DEBUG("NtResumeThread h={:#x} -> tid {:#x}", handle, t->thread_id());
  int32_t prev = t->ResumeCounted();
  if (suspend_count_out) *suspend_count_out = uint32_t(prev);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtResumeThread, NtResumeThread_entry)

u32 NtSuspendThread_entry(u32 handle, mapped_u32 suspend_count_out) {
  auto t = K().objects.Lookup<GuestThread>(handle);
  REXKRNL_DEBUG("NtSuspendThread h={:#x} -> tid {:#x}", handle, t ? t->thread_id() : 0);
  if (!t) return X_STATUS_INVALID_HANDLE;
  int32_t prev = t->Suspend();
  if (suspend_count_out) *suspend_count_out = uint32_t(prev);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtSuspendThread, NtSuspendThread_entry)

// ---- timers --------------------------------------------------------------------

// Ion Assault's tick worker (guest sub_82501460) creates a 10 ms periodic timer
// and loops wait-on-timer -> tick. The old stubs returned success without ever
// writing the handle, so the worker read back handle 0, ExTerminateThread'd at
// boot, and the title spun on its loading screen forever.
u32 NtCreateTimer_entry(mapped_u32 handle_out, u32 obj_attributes, u32 timer_type) {
  // TIMER_TYPE: 0 = NotificationTimer (manual-reset), 1 = SynchronizationTimer
  // (auto-reset). The title's wrapper passes 1 for a plain timer.
  auto timer = std::make_shared<Timer>(timer_type == 0);
  uint32_t handle = K().objects.Add(timer);
  if (handle_out) *handle_out = handle;
  REXKRNL_DEBUG("NtCreateTimer type={} -> handle {:#x}", timer_type, handle);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtCreateTimer, NtCreateTimer_entry)

// Register layout taken from the title's wrapper (sub_8260DC90): r5 is a timer
// APC routine (always 0 so far - APC delivery is not implemented, waiters still
// see the signal), r9 is the period in ms, 0 for one-shot.
u32 NtSetTimerEx_entry(u32 handle, mapped_u64 due_time_ptr, u32 apc_routine, u32 apc_context,
                       u32 unk1, u32 unk2, u32 period_ms, u32 unk3) {
  auto timer = K().objects.Lookup<Timer>(handle);
  if (!timer) return X_STATUS_INVALID_HANDLE;
  if (apc_routine) {
    REXKRNL_WARN("NtSetTimerEx: APC routine {:#010x} not implemented; waiters still see the signal",
                 apc_routine);
  }
  uint64_t due = due_time_ptr ? uint64_t(*due_time_ptr) : 0;
  auto rel = TimeoutFromGuest(due_time_ptr ? &due : nullptr);
  timer->Set(rel.value_or(std::chrono::nanoseconds(0)), std::chrono::milliseconds(period_ms));
  REXKRNL_DEBUG("NtSetTimerEx handle={:#x} due={}ns period={}ms", handle,
                rel ? rel->count() : 0, period_ms);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtSetTimerEx, NtSetTimerEx_entry)

u32 NtCancelTimer_entry(u32 handle, u32 previous_state_ptr) {
  auto timer = K().objects.Lookup<Timer>(handle);
  if (!timer) return X_STATUS_INVALID_HANDLE;
  int32_t prev = timer->Cancel();
  if (previous_state_ptr) Store8(previous_state_ptr, uint8_t(prev ? 1 : 0));
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtCancelTimer, NtCancelTimer_entry)

// ---- title terminate notifications ---------------------------------------------------

// X_EX_TITLE_TERMINATE_REGISTRATION { be32 notification_routine; be32 priority;
//                                     be32 list_flink; be32 list_blink; }
// D3D registers one at device create and the audio thread pool registers
// another. We keep the list so a future clean shutdown can run them; nothing
// calls them today because the runtime does not unload a title.
void ExRegisterTitleTerminateNotification_entry(u32 registration_ptr, u32 create) {
  Kernel& k = K();
  uint32_t routine = Load32(registration_ptr + 0);
  uint32_t priority = Load32(registration_ptr + 4);
  if (create) {
    k.title_terminate_notifications.push_back(registration_ptr);
    REXKRNL_DEBUG("ExRegisterTitleTerminateNotification +{:#010x} routine={:#010x} priority={:#010x}",
                  registration_ptr, routine, priority);
  } else {
    auto& v = k.title_terminate_notifications;
    v.erase(std::remove(v.begin(), v.end(), registration_ptr), v.end());
    REXKRNL_DEBUG("ExRegisterTitleTerminateNotification -{:#010x}", registration_ptr);
  }
}
XN_IMPORT(__imp__ExRegisterTitleTerminateNotification, ExRegisterTitleTerminateNotification_entry)

// ---- misc thread state -----------------------------------------------------------------

// Enables or disables floating-point exceptions for the calling thread and
// returns the previous setting. The generated code keeps FPSCR in the context
// and never traps, so the console's answer for "they were off" is exact.
u32 KeEnableFpuExceptions_entry(u32 enabled) {
  static bool warned = false;
  if (enabled && !warned) {
    warned = true;
    REXKRNL_WARN("KeEnableFpuExceptions(1): FP exceptions stay masked (generated code never traps)");
  }
  return 0;
}
XN_IMPORT(__imp__KeEnableFpuExceptions, KeEnableFpuExceptions_entry)

// Retail consoles have no debugger attached, so this returns immediately.
XN_IMPORT_RAW(__imp__DbgBreakPoint) {
  (void)base;
  static bool warned = false;
  if (!warned) {
    warned = true;
    REXKRNL_WARN("DbgBreakPoint reached (no debugger attached, continuing) lr={:#010x}",
                 uint32_t(ctx.lr));
  }
}
