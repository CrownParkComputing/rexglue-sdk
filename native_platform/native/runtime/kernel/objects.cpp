#include "objects.h"

#include <atomic>
#include <array>
#include <csignal>
#include <format>
#include <map>
#include <string>
#include <thread>

#include <rex/chrono/clock.h>
#include <rex/logging.h>

#include "kernel.h"

namespace xn {

WaitResult KObject::Wait(std::optional<std::chrono::nanoseconds>) { return WaitResult::Signaled; }

namespace {
bool EventTraceEnabled() {
  static const bool on = std::getenv("XN_TRACE_EVENTS") != nullptr;
  return on;
}
}  // namespace

int32_t Event::Set() {
  int32_t prev;
  {
    std::lock_guard lock(DispatcherMutex());
    SyncFromGuestLocked();
    prev = signaled_;
    signaled_ = true;
    MirrorToGuestLocked();
  }
  DispatcherSignal();
  if (EventTraceEnabled()) {
    GuestThread* t = CurrentThread();
    REXKRNL_INFO("event SET   guest={:#010x} handle={:#x} was={} tid={:#x}", guest_object_,
                 handle_, prev, t ? t->thread_id() : 0);
  }
  return prev;
}
int32_t Event::Reset() {
  std::lock_guard lock(DispatcherMutex());
  SyncFromGuestLocked();
  int32_t prev = signaled_;
  signaled_ = false;
  MirrorToGuestLocked();
  return prev;
}
// Release every waiter that can take it right now, then clear. Nothing in the
// titles we run uses this (KePulseEvent is not imported by either); it is here
// so the object model is complete, not because it is exercised.
int32_t Event::Pulse() {
  int32_t prev;
  {
    std::lock_guard lock(DispatcherMutex());
    prev = signaled_;
    signaled_ = true;
    MirrorToGuestLocked();
  }
  DispatcherSignal();
  {
    std::lock_guard lock(DispatcherMutex());
    signaled_ = false;
    MirrorToGuestLocked();
  }
  return prev;
}
WaitResult Event::Wait(std::optional<std::chrono::nanoseconds> timeout) {
  return WaitMultiple({shared_from_this()}, false, timeout) == kWaitTimeout ? WaitResult::Timeout
                                                                           : WaitResult::Signaled;
}
bool Event::PollLocked() { SyncFromGuestLocked(); return signaled_; }
void Event::ConsumeLocked() {
  if (!manual_) {
    signaled_ = false;
    MirrorToGuestLocked();
  }
  if (EventTraceEnabled()) {
    GuestThread* t = CurrentThread();
    REXKRNL_INFO("event TAKE  guest={:#010x} handle={:#x} tid={:#x}", guest_object_, handle_,
                 t ? t->thread_id() : 0);
  }
}
std::string Event::DescribeLocked() {
  SyncFromGuestLocked();
  return std::format("{}, {}", signaled_ ? "signalled" : "clear", manual_ ? "manual" : "auto-reset");
}
void Event::SyncFromGuestLocked() {
  if (!guest_object_) return;
  auto* h = reinterpret_cast<X_DISPATCH_HEADER*>(G(guest_object_));
  if (h->wait_list_flink != kBoundHeaderSignature || h->wait_list_blink != handle_) return;
  // KeClearEvent is inlined by the XDK and writes SignalState directly.
  const auto raw = __atomic_load_n(reinterpret_cast<uint32_t*>(G(guest_object_ + 4)), __ATOMIC_ACQUIRE);
  signaled_ = __builtin_bswap32(raw) != 0;
}
void Event::MirrorToGuestLocked() {
  if (guest_object_) {
    auto* h = reinterpret_cast<X_DISPATCH_HEADER*>(G(guest_object_));
    if (h->wait_list_flink != kBoundHeaderSignature || h->wait_list_blink != handle_) return;
    h->signal_state = signaled_ ? 1 : 0;
  }
}

void NotifyListener::Enqueue(uint32_t id, uint32_t param) {
  std::lock_guard lock(mutex_);
  queue_.emplace_back(id, param);
}
bool NotifyListener::Dequeue(uint32_t match_id, uint32_t* id, uint32_t* param) {
  std::lock_guard lock(mutex_);
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if (!match_id || it->first == match_id) {
      *id = it->first;
      *param = it->second;
      queue_.erase(it);
      return true;
    }
  }
  return false;
}

// ---- timer ---------------------------------------------------------------------

void Timer::Set(std::chrono::nanoseconds due, std::chrono::milliseconds period) {
  {
    std::lock_guard lock(mutex_);
    due_ = std::chrono::steady_clock::now() + due;
    period_ = period;
    running_ = true;
    ++generation_;
    {
      std::lock_guard dlock(DispatcherMutex());
      signaled_ = false;
      MirrorToGuestLocked();
    }
    if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
  }
  cv_.notify_all();
}

int32_t Timer::Cancel() {
  std::unique_lock lock(mutex_);
  running_ = false;
  ++generation_;
  int32_t prev;
  {
    std::lock_guard dlock(DispatcherMutex());
    prev = signaled_;
    signaled_ = false;
    MirrorToGuestLocked();
  }
  lock.unlock();
  cv_.notify_all();
  return prev;
}

void Timer::Stop() {
  {
    std::lock_guard lock(mutex_);
    destroyed_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Timer::Run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    while (!destroyed_ && !running_) cv_.wait(lock);
    if (destroyed_) return;
    const uint64_t gen = generation_;
    cv_.wait_until(lock, due_, [&] { return destroyed_ || !running_ || generation_ != gen; });
    if (destroyed_) return;
    if (!running_ || generation_ != gen) continue;  // cancelled or re-armed
    if (std::chrono::steady_clock::now() < due_) continue;  // spurious wakeup
    {
      std::lock_guard dlock(DispatcherMutex());
      signaled_ = true;
      MirrorToGuestLocked();
    }
    DispatcherSignal();
    if (period_.count() > 0) {
      due_ += period_;
      const auto now = std::chrono::steady_clock::now();
      if (due_ < now) due_ = now + period_;  // late: skip ahead, don't burst
    } else {
      running_ = false;  // one-shot
    }
  }
}

bool Timer::PollLocked() { return signaled_; }
void Timer::ConsumeLocked() {
  if (!manual_) {
    signaled_ = false;
    MirrorToGuestLocked();
  }
}
WaitResult Timer::Wait(std::optional<std::chrono::nanoseconds> timeout) {
  return WaitMultiple({shared_from_this()}, false, timeout) == kWaitTimeout ? WaitResult::Timeout
                                                                           : WaitResult::Signaled;
}
std::string Timer::DescribeLocked() {
  return std::format("{}, {}, period {}ms", signaled_ ? "signalled" : "clear",
                     manual_ ? "manual" : "auto-reset", period_.count());
}
void Timer::MirrorToGuestLocked() {
  if (guest_object_) {
    auto* h = reinterpret_cast<X_DISPATCH_HEADER*>(G(guest_object_));
    h->signal_state = signaled_ ? 1 : 0;
  }
}

// ---- the dispatcher ---------------------------------------------------------

namespace {
std::mutex g_dispatch_mutex;
std::condition_variable g_dispatch_cv;
std::atomic<uint64_t> g_dispatch_gen{0};
// Backstop re-poll. A signal always notifies, so this only bounds the window
// where a *non-dispatcher* state change (a guest thread exiting inside code we
// do not own) would otherwise go unnoticed.
constexpr auto kDispatchRepoll = std::chrono::milliseconds(1);
}  // namespace

std::mutex& DispatcherMutex() { return g_dispatch_mutex; }
std::condition_variable& DispatcherCv() { return g_dispatch_cv; }
uint64_t DispatcherGeneration() { return g_dispatch_gen.load(std::memory_order_acquire); }
void DispatcherSignal() {
  g_dispatch_gen.fetch_add(1, std::memory_order_release);
  g_dispatch_cv.notify_all();
}

// ---- wait diagnostics -------------------------------------------------------

namespace {
struct BlockedThread {
  uint32_t thread_id;
  const char* kind;
  std::vector<std::shared_ptr<KObject>> objects;
  std::array<uint32_t, 7> guest_callers{};
};
std::mutex g_blocked_mutex;
std::map<const void*, BlockedThread> g_blocked;   // keyed by thread-local slot
std::atomic<bool> g_dump_requested{false};
thread_local char t_blocked_key = 0;
}  // namespace

WaitScope::WaitScope(const std::vector<std::shared_ptr<KObject>>& objects, const char* kind) {
  GuestThread* t = CurrentThread();
  std::array<uint32_t, 7> callers{};
  static const bool trace_stack = std::getenv("XN_TRACE_YIELDS") || std::getenv("XN_TRACE_WAITS");
  if (const auto* context = CurrentContext()) {
    callers[0] = uint32_t(context->lr);
    auto frame = context->r1.u32;
    for (unsigned i = 1; trace_stack && frame >= 0x10000 && i < callers.size(); ++i) {
      const auto parent = Load32(frame);
      if (parent <= frame || parent - frame > 0x10000) break;
      callers[i] = Load32(parent - 8);
      frame = parent;
    }
  }
  std::lock_guard lock(g_blocked_mutex);
  g_blocked[&t_blocked_key] = BlockedThread{t ? t->thread_id() : 0, kind, objects, std::move(callers)};
  registered_ = true;
}
WaitScope::~WaitScope() {
  if (!registered_) return;
  std::lock_guard lock(g_blocked_mutex);
  g_blocked.erase(&t_blocked_key);
}

void DumpWaits(const char* reason) {
  std::vector<std::pair<BlockedThread, std::vector<WaitEntry>>> rows;
  {
    std::lock_guard blocked(g_blocked_mutex);
    std::lock_guard dispatch(g_dispatch_mutex);
    for (auto& [key, b] : g_blocked) {
      std::vector<WaitEntry> entries;
      for (auto& o : b.objects) {
        entries.push_back(WaitEntry{o ? o->guest_object() : 0, o ? o->name() : "null",
                                    o ? o->PollLocked() : false,
                                    o ? o->DescribeLocked() : std::string(),
                                    o ? o->handle() : 0});
      }
      rows.emplace_back(b, std::move(entries));
    }
  }
  REXKRNL_INFO("=== blocked threads ({}): {} ===", rows.size(), reason);
  for (auto& [b, entries] : rows) {
    std::string text;
    std::string callers;
    for (auto address : b.guest_callers) {
      if (!address) break;
      callers += std::format(" {:#x}", address);
    }
    bool any_available = false;
    for (auto& e : entries) {
      if (e.available) any_available = true;
      text += std::format(" {}@{:#010x} handle={:#x} [{}]{}", e.type, e.guest_address, e.handle, e.state,
                          e.available ? " AVAILABLE" : "");
    }
    REXKRNL_INFO("  tid {:#x} waiting {} on{}{}; callers=[{}]", b.thread_id, b.kind, text,
                 any_available ? "   <-- LOST WAKEUP: waiting on something already available"
                               : "", callers);
  }
}

void InstallWaitDumpSignalHandler() {
  std::signal(SIGUSR1, [](int) { g_dump_requested.store(true, std::memory_order_relaxed); });
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (g_dump_requested.exchange(false, std::memory_order_relaxed)) DumpWaits("SIGUSR1");
    }
  }).detach();
}

size_t WaitMultiple(const std::vector<std::shared_ptr<KObject>>& objects, bool wait_all,
                    std::optional<std::chrono::nanoseconds> timeout) {
  if (objects.empty()) return 0;
  const auto deadline = timeout ? std::optional(std::chrono::steady_clock::now() + *timeout)
                                : std::nullopt;
  WaitScope scope(objects, wait_all ? "all" : "any");
  if (EventTraceEnabled()) {
    GuestThread* t = CurrentThread();
    for (auto& o : objects) {
      if (o) REXKRNL_INFO("wait  ENTER {}@{:#010x} handle={:#x} tid={:#x}", o->name(),
                          o->guest_object(), o->handle(), t ? t->thread_id() : 0);
    }
  }
  std::unique_lock lock(g_dispatch_mutex);
  for (;;) {
    const uint64_t gen = DispatcherGeneration();
    if (wait_all) {
      bool all = true;
      for (auto& o : objects) {
        if (!o || !o->PollLocked()) { all = false; break; }
      }
      if (all) {
        for (auto& o : objects) o->ConsumeLocked();
        return 0;
      }
    } else {
      for (size_t i = 0; i < objects.size(); i++) {
        if (objects[i] && objects[i]->PollLocked()) {
          objects[i]->ConsumeLocked();
          return i;
        }
      }
    }
    if (deadline && std::chrono::steady_clock::now() >= *deadline) return kWaitTimeout;
    auto wait_for = kDispatchRepoll;
    if (deadline) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          *deadline - std::chrono::steady_clock::now());
      if (left < wait_for) wait_for = left > std::chrono::milliseconds(0) ? left : std::chrono::milliseconds(0);
    }
    g_dispatch_cv.wait_for(lock, wait_for,
                           [&] { return DispatcherGeneration() != gen; });
  }
}

// ---- semaphore --------------------------------------------------------------

int32_t Semaphore::Release(int32_t increment) {
  int32_t prev;
  {
    std::lock_guard lock(g_dispatch_mutex);
    prev = count_;
    if (int64_t(count_) + increment > int64_t(limit_)) return -1;
    count_ += increment;
    MirrorToGuestLocked();
  }
  DispatcherSignal();
  return prev;
}
int32_t Semaphore::count() const {
  std::lock_guard lock(g_dispatch_mutex);
  return count_;
}
bool Semaphore::PollLocked() { return count_ > 0; }
void Semaphore::ConsumeLocked() {
  if (count_ > 0) --count_;
  MirrorToGuestLocked();
}
WaitResult Semaphore::Wait(std::optional<std::chrono::nanoseconds> timeout) {
  return WaitMultiple({shared_from_this()}, false, timeout) == kWaitTimeout ? WaitResult::Timeout
                                                                           : WaitResult::Signaled;
}
std::string Semaphore::DescribeLocked() {
  return std::format("count {} / limit {}", count_, limit_);
}

void Semaphore::MirrorToGuestLocked() {
  if (guest_object_) {
    auto* h = reinterpret_cast<X_DISPATCH_HEADER*>(G(guest_object_));
    h->signal_state = count_;
  }
}

// ---- mutant -----------------------------------------------------------------

int32_t Mutant::Release(bool abandon) {
  int32_t prev;
  {
    std::lock_guard lock(g_dispatch_mutex);
    GuestThread* t = CurrentThread();
    uint32_t me = t ? t->kthread() : 0;
    if (!abandon && owner_ && owner_ != me) return -1;
    prev = recursion_;
    if (abandon) {
      abandoned_ = true;
      recursion_ = 0;
      owner_ = 0;
    } else if (--recursion_ <= 0) {
      recursion_ = 0;
      owner_ = 0;
    }
    MirrorToGuestLocked();
  }
  DispatcherSignal();
  return prev;
}
bool Mutant::PollLocked() {
  GuestThread* t = CurrentThread();
  uint32_t me = t ? t->kthread() : 0;
  return owner_ == 0 || owner_ == me;
}
void Mutant::ConsumeLocked() {
  GuestThread* t = CurrentThread();
  owner_ = t ? t->kthread() : 0xFFFFFFFF;
  ++recursion_;
  abandoned_ = false;
  MirrorToGuestLocked();
}
WaitResult Mutant::Wait(std::optional<std::chrono::nanoseconds> timeout) {
  return WaitMultiple({shared_from_this()}, false, timeout) == kWaitTimeout ? WaitResult::Timeout
                                                                           : WaitResult::Signaled;
}
std::string Mutant::DescribeLocked() {
  GuestThread* t = CurrentThread();
  return std::format("owner kthread {:#010x}{}, recursion {}{}", owner_,
                     owner_ && t && owner_ == t->kthread() ? " (me)" : "", recursion_,
                     abandoned_ ? ", abandoned" : "");
}

void Mutant::MirrorToGuestLocked() {
  if (guest_object_) {
    auto* h = reinterpret_cast<X_DISPATCH_HEADER*>(G(guest_object_));
    h->signal_state = owner_ ? 0 : 1;
  }
}

uint32_t ObjectTable::AddLocked(const std::shared_ptr<KObject>& object, uint32_t guest_object) {
  uint32_t handle = next_handle_;
  next_handle_ += 4;
  // Aliases share the original dispatcher identity. Changing it would make
  // the stamped guest header look stale and split pointer/handle waiters.
  if (!object->handle_) object->handle_ = handle;
  handles_[handle] = object;
  if (guest_object) {
    auto* h = reinterpret_cast<X_DISPATCH_HEADER*>(G(guest_object));
    object->guest_object_ = guest_object;
    by_guest_[guest_object] = Binding{object, h->type, h->size};
    h->wait_list_flink = kBoundHeaderSignature;
    h->wait_list_blink = object->handle_;
  }
  return handle;
}

uint32_t ObjectTable::Add(std::shared_ptr<KObject> object, uint32_t guest_object) {
  std::lock_guard lock(mutex_);
  return AddLocked(object, guest_object);
}

void ObjectTable::Remove(uint32_t handle) {
  std::lock_guard lock(mutex_);
  auto it = handles_.find(handle);
  if (it == handles_.end()) return;
  // Objects bound to guest memory stay reachable by address (the guest may
  // still wait on the struct); only the handle goes away.
  handles_.erase(it);
}

std::shared_ptr<KObject> ObjectTable::Lookup(uint32_t handle) {
  std::lock_guard lock(mutex_);
  if (handle == 0xFFFFFFFE) {  // current thread pseudo-handle
    GuestThread* t = CurrentThread();
    return t ? t->shared_from_this() : nullptr;
  }
  auto it = handles_.find(handle);
  return it == handles_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<KObject>> ObjectTable::Snapshot(KObject::Type type) {
  std::lock_guard lock(mutex_);
  std::vector<std::shared_ptr<KObject>> result;
  for (const auto& [handle, object] : handles_)
    if (object->type() == type) result.push_back(object);
  return result;
}

void ObjectTable::BindGuestHeader(uint32_t guest_address, const std::shared_ptr<KObject>& object) {
  std::lock_guard lock(mutex_);
  auto* h = reinterpret_cast<X_DISPATCH_HEADER*>(G(guest_address));
  object->guest_object_ = guest_address;
  by_guest_[guest_address] = Binding{object, h->type, h->size};
  h->wait_list_flink = kBoundHeaderSignature;
  h->wait_list_blink = object->handle_;
}

void ObjectTable::UnbindGuestHeader(uint32_t guest_address) {
  std::lock_guard lock(mutex_);
  auto it = by_guest_.find(guest_address);
  if (it == by_guest_.end()) return;
  it->second.object->guest_object_ = 0;
  by_guest_.erase(it);
}

// The guest's dispatcher header is the authority; our host object is only a
// wait list attached to that memory.
//
// Everything happens under one lock. It used to release the lock between "is
// there a binding?" and "create one", and six guest threads reaching the same
// event in the same microsecond then built TWO Event objects for one address:
// five threads blocked on the object that lost the insert race and every
// signal went to the winner, so one worker never woke and the title's thread
// pool deadlocked. That was the residual stall.
std::shared_ptr<KObject> ObjectTable::FromGuestHeader(uint32_t guest_address) {
  std::lock_guard lock(mutex_);
  auto* h = reinterpret_cast<X_DISPATCH_HEADER*>(G(guest_address));
  const uint8_t type = h->type;
  const uint8_t size = h->size;

  auto it = by_guest_.find(guest_address);
  if (it != by_guest_.end()) {
    // Trust the binding only while the header still describes the same object.
    // A header the guest re-initialised in place reports a different type or
    // size and drops out here by itself, with no import to hook.
    if (it->second.type == type && it->second.size == size &&
        h->wait_list_flink == kBoundHeaderSignature &&
        h->wait_list_blink == it->second.object->handle()) return it->second.object;
    REXKRNL_DEBUG("dispatcher at {:#010x} changed shape (type {}->{}, size {}->{}); "
                  "dropping the stale {} binding",
                  guest_address, it->second.type, type, it->second.size, size,
                  it->second.object->name());
    it->second.object->guest_object_ = 0;
    by_guest_.erase(it);
  }

  // Initial state comes from the header too, not from a default.
  switch (type) {
    case 0:    // notification (manual-reset) event
    case 1: {  // synchronisation (auto-reset) event, also a critical section head
      auto ev = std::make_shared<Event>(type == 0, int32_t(h->signal_state) != 0);
      AddLocked(ev, guest_address);
      return ev;
    }
    case 2: {  // mutant
      auto m = std::make_shared<Mutant>(0);
      AddLocked(m, guest_address);
      return m;
    }
    case 5: {  // semaphore: signal_state is the count, the limit follows at +0x10
      auto sem = std::make_shared<Semaphore>(int32_t(h->signal_state),
                                             int32_t(Load32(guest_address + 0x10)));
      AddLocked(sem, guest_address);
      return sem;
    }
    default:
      REXKRNL_WARN("wait on unmodelled dispatcher header type {} at {:#010x}", type, guest_address);
      return nullptr;
  }
}

std::optional<std::chrono::nanoseconds> TimeoutFromGuest(const uint64_t* timeout_ptr) {
  if (!timeout_ptr) return std::nullopt;
  int64_t t = int64_t(*timeout_ptr);
  if (t == 0) return std::chrono::nanoseconds(0);
  if (t < 0) return std::chrono::nanoseconds(-t * 100);
  // Absolute FILETIME.
  int64_t now = int64_t(rex::chrono::Clock::QueryGuestSystemTime());
  int64_t delta = t - now;
  return std::chrono::nanoseconds(delta > 0 ? delta * 100 : 0);
}

}  // namespace xn
