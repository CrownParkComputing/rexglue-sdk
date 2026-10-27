#include "threads.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <time.h>

#include <rex/logging.h>
#include <rex/ppc.h>

#include "kernel.h"

namespace xn {

namespace {
thread_local GuestThread* t_current = nullptr;
thread_local PPCContext* t_fiber_context = nullptr;
std::atomic<uint32_t> g_next_thread_id{0x00000100};

// --- Preemptive suspension ---------------------------------------------------
// The console kernel stops a running thread wherever it is; titles that use
// suspend/resume as a synchronisation primitive (OutRun Online Arcade does,
// ~10^7 NtSuspendThread calls a second against its streaming workers) corrupt
// their own state when suspension silently does nothing, because the suspended
// thread keeps racing the suspender through shared guest memory.
//
// Freezing a host thread mid-C++ is only safe without a full register freeze
// because of what guest state actually lives where: guest registers are in the
// thread's PPCContext (never in host registers across generated calls) and all
// other guest-visible state is in guest memory. Both stay exactly as they were
// while this thread parks inside the signal handler - no host frame is unwound
// and nothing is copied - so from the outside the thread is simply frozen, and
// when the handler returns it continues with every host register intact.
const int kSuspendSignal = SIGRTMIN + 1;  // glibc's lowest RT signal is its own; +1 is unused

void SuspendSignalAction(int, siginfo_t*, void*) {
  GuestThread* self = t_current;
  if (!self) return;
  self->SuspendSignalHandler();
}

void InstallSuspendSignalHandler() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction sa {};
    sa.sa_sigaction = SuspendSignalAction;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;  // do not break blocking syscalls
    sigemptyset(&sa.sa_mask);
    if (sigaction(kSuspendSignal, &sa, nullptr) != 0) {
      REXKRNL_ERROR("sigaction({}) failed: {}", kSuspendSignal, errno);
    }
  });
}

PPCContext* NewContext() {
  void* p = nullptr;
  if (posix_memalign(&p, 64, sizeof(PPCContext)) != 0) Fatal("context alloc failed");
  return new (p) PPCContext{};
}
}  // namespace

GuestThread* CurrentThread() { return t_current; }
PPCContext* CurrentContext() { return t_fiber_context ? t_fiber_context : (t_current ? t_current->context() : nullptr); }
void SetFiberContext(PPCContext* context) { t_fiber_context = context; }
uint8_t* GuestBase() { return K().arena.base(); }

GuestThread::GuestThread(Params params) : KObject(Type::Thread), params_(std::move(params)) {
  thread_id_ = g_next_thread_id.fetch_add(4);
  context_ = NewContext();
  // ExCreateThread flags bit 0 = create suspended. Geometry Wars 2's audio
  // library creates all six of its workers this way and resumes them with
  // KeResumeThread.
  gate_open_ = (params_.creation_flags & 1) == 0;
  suspend_count_ = gate_open_ ? 0 : 1;
}

GuestThread::~GuestThread() {
  if (host_.joinable()) host_.detach();
  if (suspend_park_init_) sem_destroy(&suspend_park_);
  context_->~PPCContext();
  std::free(context_);
}

void GuestThread::AllocateGuestState() {
  Kernel& k = K();
  uint32_t stack_size = params_.stack_size ? params_.stack_size : k.image.default_stack_size;
  stack_size = std::max<uint32_t>(0x4000, (stack_size + 0xFFF) & ~0xFFFu);
  // Two guard pages below the stack (never touched by us; the arena is RW so
  // they only reserve address space).
  const uint32_t guard = 0x10000;
  stack_alloc_ = k.arena.AllocVirtual(stack_size + guard, 0x10000, true);
  if (!stack_alloc_) Fatal("guest stack allocation failed");
  stack_limit_ = stack_alloc_ + guard;
  stack_base_ = stack_limit_ + stack_size;
  k.arena.Fill(stack_limit_, stack_size, 0);

  // TLS: [extended data][slots]. r13[0] points at the block start.
  tls_slot_count_ = k.image.tls.slot_count ? k.image.tls.slot_count : 64;
  uint32_t ext = k.image.tls.data_size;
  uint32_t total = ext + tls_slot_count_ * 4;
  tls_block_ = k.arena.SystemAlloc(total, 16);
  tls_slots_ = tls_block_ + ext;
  if (ext && k.image.tls.raw_data_address) {
    std::memcpy(G(tls_block_), G(k.image.tls.raw_data_address),
                std::min(ext, k.image.tls.raw_data_size));
  }

  kpcr_ = k.arena.SystemAlloc(kKpcrSize, 64);
  kthread_ = k.arena.SystemAlloc(kKthreadSize, 64);
  Store32(kpcr_ + kKpcrTlsPtr, tls_block_);
  Store32(kpcr_ + kKpcrStackBase, stack_base_);
  Store32(kpcr_ + kKpcrStackEnd, stack_limit_);
  Store32(kpcr_ + kKpcrCurrentThread, kthread_);
  Store32(kpcr_ + kKpcrPrcbPtr, kpcr_ + kKpcrPrcb);
  Store8(kthread_ + 0, 6);  // dispatcher header type: thread
  Store32(kthread_ + kKthreadStackBase, stack_base_);
  Store32(kthread_ + kKthreadStackLimit, stack_limit_);
  Store32(kthread_ + kKthreadTlsAddress, tls_slots_);
  Store8(kthread_ + kKthreadProcessType, 1);  // user/title process
  Store32(kthread_ + kKthreadProcess, k.title_process);
  Store32(kthread_ + kKthreadThreadId, thread_id_);
  // Hardware thread. This is not cosmetic: guest code encodes the OWNER of a
  // spinlock as (1 << KPCR.current_cpu) - Geometry Wars 2's D3D does its own
  // lwarx/stwcx. spinlock that way at 0x820B4498 - so if every guest thread
  // reports cpu 0 they all claim the same owner bit and two of them can be
  // inside one lock at once. The console gives each thread one of six hardware
  // threads; honour the processor mask in the creation flags when the title
  // supplies one, and round-robin otherwise.
  uint32_t mask = (params_.creation_flags >> 24) & 0x3F;
  static std::atomic<uint32_t> next_cpu{0};
  SetActiveCpu(uint8_t(mask ? __builtin_ctz(mask) : (next_cpu.fetch_add(1) % 6)));

  context_->r1.u64 = stack_base_ - 0x40;
  context_->r13.u64 = kpcr_;
  context_->fpscr.InitHost();
  k.objects.BindGuestHeader(kthread_, shared_from_this());
}

void GuestThread::SetActiveCpu(uint8_t cpu) { Store8(kpcr_ + kKpcrCurrentCpu, cpu); }
void GuestThread::SetLastError(uint32_t error) { Store32(kthread_ + kKthreadLastError, error); }

uint32_t GuestThread::GetTls(uint32_t slot) const {
  if (slot >= tls_slot_count_) return 0;
  return Load32(tls_slots_ + slot * 4);
}
void GuestThread::SetTls(uint32_t slot, uint32_t value) {
  if (slot >= tls_slot_count_) return;
  Store32(tls_slots_ + slot * 4, value);
}

void GuestThread::BindCurrentHostThread() {
  if (!kpcr_) AllocateGuestState();
  t_current = this;
}

void GuestThread::RunGuestEntry() {
  {
    std::unique_lock lock(gate_mutex_);
    gate_cv_.wait(lock, [&] { return gate_open_.load(std::memory_order_acquire); });
    started_.store(true, std::memory_order_release);
    // Re-check the gate after publishing started_: a Suspend() that raced
    // between opening the gate and seeing started_ may have closed it again
    // expecting us to park here.
    if (!gate_open_.load(std::memory_order_acquire)) {
      gate_cv_.wait(lock, [&] { return gate_open_.load(std::memory_order_acquire); });
    }
  }
  REXKRNL_DEBUG("thread {:#x} '{}' start={:#010x} ctx={:#010x} stack={:#010x}-{:#010x} kthread={:#010x}",
                thread_id_, params_.name, params_.start_address, params_.start_context,
                stack_limit_, stack_base_, kthread_);
  int code = 0;
  if (params_.host_body) {
    params_.host_body(*this);
  } else if (params_.xapi_startup) {
    code = int(CallGuest(params_.xapi_startup, {params_.start_address, params_.start_context}));
  } else {
    code = int(CallGuest(params_.start_address, {params_.start_context}));
  }
  MarkExited(code);
}

// Runs on the guest thread inside the RT signal handler.
//
// This is the rexglue scheme (src/core/threading_posix.cpp WaitSuspended):
// each NtSuspendThread on a running thread queues one signal; the handler
// blocks in sem_wait until a matching resume posts. Signals are consumed one
// per handler entry, so a resume that lands before the handler entered is
// simply eaten there, and suspend/resume pairs - the only pattern titles use
// on running threads - stay balanced. The suspender never waits, which is
// what keeps mutual cross-suspension deadlock-free: freezing both members of
// a pair of threads that suspend each other is instant, the way the console
// kernel does it.
void GuestThread::SuspendSignalHandler() {
  while (sem_wait(&suspend_park_) == -1 && errno == EINTR) {
  }
}

// Called from Suspend() with gate_mutex_ held after suspend_count_ went up on
// a RUNNING thread. Fire-and-forget, like the reference kernel: the signal is
// queued and the handler will park the thread as soon as it is deliverable.
void GuestThread::PreemptiveSuspend() {
}

bool GuestThread::Start() {
  AllocateGuestState();
  // Init the park semaphore and publish the flag BEFORE the host thread
  // exists: Suspend() only queues preemption once the flag is visible, so a
  // suspend that races Start() can never signal a thread whose handler
  // primitives are not ready yet.
  suspend_park_init_.store(sem_init(&suspend_park_, 0, 0) == 0, std::memory_order_release);
  if (suspend_park_init_.load(std::memory_order_acquire)) {
    InstallSuspendSignalHandler();
  }
  auto self = std::static_pointer_cast<GuestThread>(shared_from_this());
  host_ = std::thread([self] {
    t_current = self.get();
    self->RunGuestEntry();
    t_current = nullptr;
  });
  return true;
}

// Wake helper shared by Resume()/ResumeCounted(): must be callable while the
// target thread is parked inside SuspendSignalHandler holding ANY mutex
// (including its own gate_mutex_), so it takes no locks the parked thread can
// hold. sem_post is async-signal-safe and does not require the waiter's locks.
void GuestThread::WakeUnlocked() {
  // Release every outstanding preemptive delivery. Stacked suspends queue one
  // signal each; only waking one token stranded the deeper levels.
  int32_t pending = preempt_pending_.exchange(0, std::memory_order_acq_rel);
  while (pending-- > 0) {
    sem_post(&suspend_park_);
  }
  if (gate_open_.exchange(true, std::memory_order_acq_rel) == false) {
    // We flipped the start gate open; wake whoever is parked on it.
    std::lock_guard<std::mutex> lock(gate_mutex_);
    gate_cv_.notify_all();
  }
}

void GuestThread::Resume() {
  suspend_count_.store(0, std::memory_order_acq_rel);
  WakeUnlocked();
}

// The counted forms behind NtSuspendThread / NtResumeThread / KeResumeThread.
// A thread created suspended starts with count 1 and parks at the gate in
// RunGuestEntry before touching any guest state, so resuming it is exact.
// Suspending a thread that is already RUNNING preempts it the way the console
// kernel does: an RT signal parks the host thread inside its signal handler
// until a matching resume, freezing every guest store mid-flight. OutRun's
// streaming model depends on this; refusing the call left two worker threads
// racing the main thread (heap free-list corruption -> SIGBUS in its
// allocator) and its streamed-in content (cars) never appearing.
// Counted suspend. LOCK-FREE on the target: this may run on a thread that is
// itself parked inside a signal handler holding another thread's gate_mutex_
// (OutRun cross-suspends its workers ~10^7 times a second), so taking any
// mutex here - including our own - can deadlock the pair.
//
// One signal per suspend level, queued unconditionally: every delivery
// consumes exactly one token from suspend_park_, and the resume side drains
// one token per outstanding level, so suspend/resume pairs stay balanced.
// Shortcuts (draining tokens on resume, or skipping the kill when a delivery
// is already queued) both lost wakeups under OutRun's rate and froze the
// front-end; see the 2026-09-26 HANDOVER note.
int32_t GuestThread::Suspend() {
  int32_t prev = suspend_count_.fetch_add(1, std::memory_order_acq_rel);
  if (!gate_open_.load(std::memory_order_acquire)) {
    return prev;  // parked at the start gate (or asked to park there): exact
  }
  {
    // Serialize the not-started decision with RunGuestEntry's gate passage:
    // it publishes started_ and re-checks the gate under gate_mutex_, so a
    // gate closed here either catches it parked at the gate or sees started_
    // and falls through to the preempt path below. pthread_kill itself stays
    // OUTSIDE any lock (the parked handler must never wait on this mutex).
    std::lock_guard<std::mutex> lock(gate_mutex_);
    if (!started_.load(std::memory_order_acquire)) {
      gate_open_.store(false, std::memory_order_release);
      return prev;
    }
  }
  if (CurrentThread() == this && suspend_park_init_.load(std::memory_order_acquire)) {
    preempt_pending_.fetch_add(1, std::memory_order_acq_rel);
    while (suspend_count_.load(std::memory_order_acquire) > 0) {
      if (sem_wait(&suspend_park_) == -1 && errno == EINTR) {
        continue;
      }
    }
    return prev;
  }
  if (suspend_park_init_.load(std::memory_order_acquire)) {
    preempt_pending_.fetch_add(1, std::memory_order_acq_rel);
    if (pthread_kill(host_.native_handle(), kSuspendSignal) != 0) {
      preempt_pending_.fetch_sub(1, std::memory_order_acq_rel);
      uint32_t n = suspend_warn_count_.fetch_add(1, std::memory_order_relaxed);
      if (n % 10000 == 0) {
        REXKRNL_WARN("NtSuspendThread on running thread {:#x} '{}': signal failed{}",
                     thread_id_, params_.name,
                     n == 0 ? "" : std::format(" [{} further failures]", n));
      }
    }
  } else {
    uint32_t n = suspend_warn_count_.fetch_add(1, std::memory_order_relaxed);
    if (n % 10000 == 0) {
      REXKRNL_WARN("NtSuspendThread on running thread {:#x} '{}': not suspended (host thread){}",
                   thread_id_, params_.name,
                   n == 0 ? "" : std::format(" [{} further refusals]", n));
    }
  }
  return prev;
}

// Counted resume. LOCK-FREE on the target, for the same reason as Suspend():
// the target may be parked inside its signal handler holding its own (or
// another thread's) gate_mutex_, so waking it must not take any mutex it can
// hold. This is the fix for the 1.5s OutRun stall: the old implementation
// locked gate_mutex_ to post the wake token while the parked thread held that
// same mutex across pthread_kill - instant permanent deadlock.
int32_t GuestThread::ResumeCounted() {
  int32_t prev = suspend_count_.load(std::memory_order_relaxed);
  for (;;) {
    if (prev == 0) {
      return 0;  // not suspended: console kernel also returns 0 and does nothing
    }
    if (suspend_count_.compare_exchange_weak(prev, prev - 1, std::memory_order_acq_rel)) {
      break;
    }
  }
  if (prev - 1 == 0) {
    WakeUnlocked();
  }
  return prev;
}

void GuestThread::Join() {
  if (host_.joinable()) host_.join();
}

void GuestThread::MarkExited(int code) {
  std::lock_guard lock(exit_mutex_);
  if (exited_.exchange(true)) return;
  exit_code_ = code;
  if (kthread_) {
    auto* h = reinterpret_cast<X_DISPATCH_HEADER*>(G(kthread_));
    h->signal_state = 1;
  }
  REXKRNL_DEBUG("thread {:#x} '{}' exited with {}", thread_id_, params_.name, code);
  exit_cv_.notify_all();
  DispatcherSignal();  // a thread object is a waitable object too
  // A suspender waiting for this thread to park must stop waiting: an exiting
  // thread can no longer run the signal handler. (park_mutex_/park_cv_ were
  // replaced by the atomic handler_parked_ handshake; nothing to acquire.)
}

WaitResult GuestThread::Wait(std::optional<std::chrono::nanoseconds> timeout) {
  std::unique_lock lock(exit_mutex_);
  if (exited_) return WaitResult::Signaled;
  if (!timeout) {
    exit_cv_.wait(lock, [&] { return exited_.load(); });
    return WaitResult::Signaled;
  }
  return exit_cv_.wait_for(lock, *timeout, [&] { return exited_.load(); }) ? WaitResult::Signaled
                                                                            : WaitResult::Timeout;
}

uint64_t CallGuest(uint32_t address, std::initializer_list<uint64_t> args) {
  PPCFunc* fn = LookupFunction(address);
  if (!fn) Fatal(std::format("CallGuest: no function at {:#010x}", address));
  PPCContext* ctx = CurrentContext();
  if (!ctx) Fatal("CallGuest from a host thread without guest state");
  // XN_TRACE_GFX_INTERRUPT=<n>: entry/exit around the first n deliveries of the
  // graphics interrupt callback. The handler takes a guest spinlock and then
  // calls through a device vtable, so "entry printed, exit never printed" is
  // the signature of it deadlocking and taking the interrupt thread with it -
  // which looks from outside exactly like interrupts having stopped.
  static const uint32_t trace_limit = [] {
    const char* v = std::getenv("XN_TRACE_GFX_INTERRUPT");
    return v ? uint32_t(std::strtoul(v, nullptr, 0)) : 0u;
  }();
  static std::atomic<uint32_t> traced{0};
  const bool trace = trace_limit && address == K().gfx_interrupt_callback &&
                     traced.load(std::memory_order_relaxed) < trace_limit;
  uint32_t seq = 0;
  if (trace) {
    seq = traced.fetch_add(1) + 1;
    GuestThread* self = CurrentThread();
    REXKRNL_INFO("gfx-interrupt #{} ENTER cb={:#010x} r3={} r4={:#010x} tid={:#x} cpu={}", seq,
                 address, args.size() ? *args.begin() : 0,
                 args.size() > 1 ? *(args.begin() + 1) : 0, self ? self->thread_id() : 0,
                 self ? Load8(self->kpcr() + kKpcrCurrentCpu) : 0xFF);
  }
  // Preserve the caller's argument/link registers; callee-saved ones are kept
  // by the generated code itself (non_volatile_as_local).
  PPCContext saved = *ctx;
  PPCRegister* regs[] = {&ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10};
  size_t i = 0;
  for (uint64_t a : args) {
    if (i < 8) regs[i]->u64 = a;
    i++;
  }
  ctx->r1.u32 -= 0x70;  // fresh linkage area below the interrupted frame
  ctx->lr = 0xFFFFFFFC;
  fn(*ctx, GuestBase());
  uint64_t result = ctx->r3.u64;
  uint32_t fpscr = ctx->fpscr.csr;
  *ctx = saved;
  ctx->fpscr.csr = fpscr;
  if (trace) REXKRNL_INFO("gfx-interrupt #{} EXIT -> {:#x}", seq, result);
  return result;
}

std::shared_ptr<GuestThread> CreateGuestThread(GuestThread::Params params) {
  auto t = std::make_shared<GuestThread>(std::move(params));
  K().objects.Add(t);
  t->Start();
  return t;
}

std::shared_ptr<GuestThread> CreateHostWorker(const std::string& name,
                                              std::function<void(GuestThread&)> body) {
  GuestThread::Params p;
  p.name = name;
  p.stack_size = 0x20000;
  p.host_body = std::move(body);
  return CreateGuestThread(std::move(p));
}

}  // namespace xn
