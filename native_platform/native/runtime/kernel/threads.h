// xenon-native: guest threads. Each is a host std::thread with its own
// PPCContext, guest stack, KPCR and TLS block. Host-side workers (GPU command
// processor, vsync, audio pump) use the same setup so they can call into guest
// code (interrupt callbacks) with a valid r1/r13.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore.h>
#include <string>
#include <thread>

#include <rex/ppc/context.h>

#include "objects.h"

namespace xn {

class GuestThread : public KObject {
 public:
  struct Params {
    std::string name;
    uint32_t stack_size = 0;       // 0 = image default
    uint32_t start_address = 0;    // guest function (or xapi startup's target)
    uint32_t start_context = 0;
    uint32_t xapi_startup = 0;     // if set: entry = xapi_startup(start_address, start_context)
    uint32_t creation_flags = 0;   // bit0 suspended, 0x80 return kthread ptr, byte3 processor
    std::function<void(GuestThread&)> host_body;  // for host workers: replaces guest entry
  };
  explicit GuestThread(Params params);
  ~GuestThread() override;

  // Allocates guest resources and starts the host thread.
  bool Start();
  // For host workers that want to run on the *calling* host thread instead.
  void BindCurrentHostThread();
  void Resume();
  void Join();

  uint32_t thread_id() const { return thread_id_; }
  uint32_t kthread() const { return kthread_; }
  uint32_t kpcr() const { return kpcr_; }
  PPCContext* context() { return context_; }
  const Params& params() const { return params_; }
  bool exited() const { return exited_.load(); }
  int exit_code() const { return exit_code_; }
  const char* name() const override { return "thread"; }
  WaitResult Wait(std::optional<std::chrono::nanoseconds> timeout) override;
  bool PollLocked() override { return exited_.load(); }
  // Suspend count, for NtSuspendThread / KeResumeThread. Returns the previous
  // count; a thread created suspended starts at 1.
  int32_t Suspend();
  int32_t ResumeCounted();

  // TLS slots (KeTls*).
  uint32_t GetTls(uint32_t slot) const;
  void SetTls(uint32_t slot, uint32_t value);
  void SetActiveCpu(uint8_t cpu);
  void SetLastError(uint32_t error);

  // Marks the thread finished (called by the thread itself or ExTerminateThread).
  void MarkExited(int code);

  // Runs inside the RT signal handler on this thread when preemptively
  // suspended (free function SuspendSignalAction forwards here).
  void SuspendSignalHandler();

 private:
  void AllocateGuestState();
  void RunGuestEntry();
  // Wakes a preempt-parked or gate-parked thread. Lock-free on the target's
  // suspend state (see ResumeCounted for why it must not take gate_mutex_).
  void WakeUnlocked();

  Params params_;
  uint32_t thread_id_ = 0;
  uint32_t stack_base_ = 0, stack_limit_ = 0, stack_alloc_ = 0;
  uint32_t kpcr_ = 0, kthread_ = 0, tls_block_ = 0, tls_slots_ = 0;
  uint32_t tls_slot_count_ = 0;
  PPCContext* context_ = nullptr;
  std::thread host_;
  std::atomic<bool> exited_{false};
  int exit_code_ = 0;
  std::mutex exit_mutex_;
  std::condition_variable exit_cv_;
  std::mutex gate_mutex_;
  std::condition_variable gate_cv_;
  std::atomic<bool> gate_open_{true};
  std::atomic<int32_t> suspend_count_{0};
  std::atomic<bool> started_{false};
  std::atomic<uint32_t> suspend_warn_count_{0};  // NtSuspendThread refusal rate limit

  // --- Preemptive suspension (NtSuspendThread on a RUNNING thread) ---
  // The console kernel stops a running thread wherever it is; titles that use
  // suspend/resume as a synchronisation primitive (OutRun Online Arcade does,
  // ~10^7 calls/s against its streaming workers) corrupt their own state if
  // suspension silently does nothing. Each suspend of a running thread queues
  // one RT signal whose handler blocks in suspend_park_ until a matching
  // resume posts - fire-and-forget on the suspend side, exactly like the
  // reference kernel, so cross-suspending thread pairs cannot deadlock us.
  void PreemptiveSuspend();
  sem_t suspend_park_{};            // handler parks here until resume
  std::atomic<bool> suspend_park_init_{false};  // sem_init succeeded
  // Signals queued whose resume has not posted yet. The suspend side bumps
  // this under gate_mutex_; the resume side drains it WITHOUT any lock (the
  // parked handler needs no lock to wake, so wake never deadlocks against a
  // thread that was preempted mid-suspend holding someone's gate_mutex_).
  std::atomic<int32_t> preempt_pending_{0};
};

// Current guest thread on this host thread (null on plain host threads).
GuestThread* CurrentThread();
PPCContext* CurrentContext();
uint8_t* GuestBase();

// Calls a guest function on the current guest thread with integer args in r3..
// The caller's registers are preserved around the call.
uint64_t CallGuest(uint32_t address, std::initializer_list<uint64_t> args);

// Creates a guest thread from ExCreateThread parameters; returns the object.
std::shared_ptr<GuestThread> CreateGuestThread(GuestThread::Params params);
// Creates a host worker with guest-callable state; body runs on the new host thread.
std::shared_ptr<GuestThread> CreateHostWorker(const std::string& name,
                                              std::function<void(GuestThread&)> body);

}  // namespace xn
