// xenon-native: kernel objects (events, threads, files, listeners) and the handle table.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xtypes.h"

namespace xn {

enum class WaitResult { Signaled, Timeout };

class KObject : public std::enable_shared_from_this<KObject> {
 public:
  enum class Type { Event, Thread, File, NotifyListener, Semaphore, Mutant, Timer, Unknown };
  explicit KObject(Type type) : type_(type) {}
  virtual ~KObject() = default;
  Type type() const { return type_; }
  uint32_t handle() const { return handle_; }
  uint32_t guest_object() const { return guest_object_; }
  // Blocks until the object is signaled or the timeout elapses. Thread objects
  // are signaled when they exit; files are always signaled.
  virtual WaitResult Wait(std::optional<std::chrono::nanoseconds> timeout);
  virtual const char* name() const { return "object"; }

  // The seam KeWaitForMultipleObjects uses. Both are called with
  // DispatcherMutex() held, so a set of objects can be tested and then taken
  // without another multi-waiter slipping between the two.
  //   PollLocked   - would a wait on this object succeed right now? (no state change)
  //   ConsumeLocked - take the object (auto-reset event clears, semaphore
  //                   decrements, mutant becomes owned); no-op for the rest.
  virtual bool PollLocked() { return true; }
  virtual void ConsumeLocked() {}
  // One line of human-readable state for DumpWaits(), called with
  // DispatcherMutex() held.
  virtual std::string DescribeLocked() { return {}; }

 protected:
  friend class ObjectTable;
  Type type_;
  uint32_t handle_ = 0;
  uint32_t guest_object_ = 0;
};

// Shared enumeration seam: content packages and achievements serialize their
// own guest records; the XAM import handles status and overlapped completion.
class Enumerator : public KObject {
 public:
  Enumerator() : KObject(Type::Unknown) {}
  virtual uint32_t Read(uint32_t buffer, uint32_t capacity, uint32_t& count) = 0;
};

// An event. Its signal state lives under DispatcherMutex(), like Semaphore and
// Mutant, and every wait goes through WaitMultiple.
//
// It used to carry its own mutex and condition variable, with WaitMultiple
// reaching in through PollLocked/ConsumeLocked under a *different* lock. That
// left a window between poll and consume in which a plain single-object waiter
// could take the same auto-reset event - so two threads each believed they had
// been signalled once. That stole exactly one wakeup from this title's thread
// pool and deadlocked it in about half of all boots. One lock owns signal
// state now, so poll-and-take is atomic against every waiter.
class Event : public KObject {
 public:
  Event(bool manual_reset, bool initial) : KObject(Type::Event), manual_(manual_reset), signaled_(initial) {}
  // Returns the previous state.
  int32_t Set();
  int32_t Reset();
  int32_t Pulse();
  WaitResult Wait(std::optional<std::chrono::nanoseconds> timeout) override;
  bool PollLocked() override;
  void ConsumeLocked() override;
  bool signaled() const { return signaled_; }
  std::string DescribeLocked() override;
  const char* name() const override { return "event"; }

 private:
  void SyncFromGuestLocked();
  void MirrorToGuestLocked();
  bool manual_;
  bool signaled_;
};

class NotifyListener : public KObject {
 public:
  NotifyListener(uint64_t mask) : KObject(Type::NotifyListener), mask_(mask) {}
  void Enqueue(uint32_t id, uint32_t param);
  bool Dequeue(uint32_t match_id, uint32_t* id, uint32_t* param);
  uint64_t mask() const { return mask_; }
  const char* name() const override { return "notify-listener"; }

 private:
  uint64_t mask_;
  std::mutex mutex_;
  std::deque<std::pair<uint32_t, uint32_t>> queue_;
};

// A waitable timer (NtCreateTimer). Signal state lives under DispatcherMutex()
// like Event; one host thread waits out the due time, raises the signal, and
// re-arms while a period is set. Ion Assault drives a 10 ms tick worker off
// one of these - back when NtCreateTimer was a stub that returned success
// without writing the handle, the worker read back a zero handle, exited at
// boot, and the title spun on its loading screen forever.
//
// Lock order is mutex_ -> DispatcherMutex() everywhere; the fire path holds
// mutex_ while raising the signal, so Set/Cancel must take mutex_ first too.
class Timer : public KObject {
 public:
  explicit Timer(bool manual_reset) : KObject(Type::Timer), manual_(manual_reset) {}
  ~Timer() override { Stop(); }
  // Arms (or re-arms): fire at `due` from now, then every `period` while the
  // period is non-zero. Re-arming clears any pending signal.
  void Set(std::chrono::nanoseconds due, std::chrono::milliseconds period);
  // Cancels and clears the signal; returns the previous signal state (BOOLEAN).
  int32_t Cancel();
  WaitResult Wait(std::optional<std::chrono::nanoseconds> timeout) override;
  bool PollLocked() override;
  void ConsumeLocked() override;
  std::string DescribeLocked() override;
  const char* name() const override { return "timer"; }

 private:
  void Stop();
  void Run();
  void MirrorToGuestLocked();
  bool manual_;
  bool signaled_ = false;  // under DispatcherMutex()
  std::mutex mutex_;
  std::condition_variable cv_;
  uint64_t generation_ = 0;  // bumped on every Set/Cancel so stale fires no-op
  bool running_ = false;
  bool destroyed_ = false;
  std::chrono::steady_clock::time_point due_;
  std::chrono::milliseconds period_{0};
  std::thread thread_;
};

// A counted semaphore. Unlike Event, its state lives under DispatcherMutex(),
// so KeWaitForMultipleObjects over semaphores is exactly atomic rather than
// best-effort. Geometry Wars 2's audio library is the first user.
class Semaphore : public KObject {
 public:
  Semaphore(int32_t count, int32_t limit)
      : KObject(Type::Semaphore), count_(count), limit_(limit ? limit : 0x7FFFFFFF) {}
  // Adds `increment` to the count, capped at the limit; returns the previous
  // count, or -1 when the increment would exceed the limit (the guest's
  // STATUS_SEMAPHORE_LIMIT_EXCEEDED case).
  int32_t Release(int32_t increment);
  int32_t count() const;
  int32_t limit() const { return limit_; }
  WaitResult Wait(std::optional<std::chrono::nanoseconds> timeout) override;
  bool PollLocked() override;
  void ConsumeLocked() override;
  std::string DescribeLocked() override;
  const char* name() const override { return "semaphore"; }

 private:
  void MirrorToGuestLocked();
  int32_t count_;
  int32_t limit_;
};

// A recursive mutex owned by a guest thread (KTHREAD address). Also under
// DispatcherMutex().
class Mutant : public KObject {
 public:
  explicit Mutant(uint32_t initial_owner)
      : KObject(Type::Mutant), owner_(initial_owner), recursion_(initial_owner ? 1 : 0) {}
  // Returns the previous recursion count, or -1 when the caller does not own it.
  int32_t Release(bool abandon);
  WaitResult Wait(std::optional<std::chrono::nanoseconds> timeout) override;
  bool PollLocked() override;
  void ConsumeLocked() override;
  bool abandoned() const { return abandoned_; }
  std::string DescribeLocked() override;
  const char* name() const override { return "mutant"; }

 private:
  void MirrorToGuestLocked();
  uint32_t owner_ = 0;   // KTHREAD guest address, 0 = free
  int32_t recursion_ = 0;
  bool abandoned_ = false;
};

// The dispatcher: one mutex and one condition variable that every waitable
// object's *signal* path bumps, so a wait over several objects has something
// to sleep on. Signal paths must NOT take DispatcherMutex() - they change their
// own state under their own lock and then call DispatcherSignal(), which only
// bumps a generation counter and notifies. A multi-waiter holds
// DispatcherMutex(), reads the generation, polls, and sleeps on the cv with a
// predicate on the generation, so a signal that lands between the poll and the
// sleep cannot be missed.
std::mutex& DispatcherMutex();
std::condition_variable& DispatcherCv();
uint64_t DispatcherGeneration();
void DispatcherSignal();

// Waits on `objects` (already resolved). wait_all = every object, otherwise the
// first to become available. Returns the index taken (0 for wait_all), or
// kWaitTimeout.
constexpr size_t kWaitTimeout = size_t(-1);
size_t WaitMultiple(const std::vector<std::shared_ptr<KObject>>& objects, bool wait_all,
                    std::optional<std::chrono::nanoseconds> timeout);

class ObjectTable {
 public:
  // Registers the object, assigns a handle, optionally binds it to a guest
  // dispatcher header (the header gets our signature + handle so a later lookup
  // by guest address finds the same host object).
  uint32_t Add(std::shared_ptr<KObject> object, uint32_t guest_object = 0);
  void Remove(uint32_t handle);
  std::shared_ptr<KObject> Lookup(uint32_t handle);
  // Hold objects alive while file I/O runs outside the table lock.
  std::vector<std::shared_ptr<KObject>> Snapshot(KObject::Type type);
  template <class T>
  std::shared_ptr<T> Lookup(uint32_t handle) {
    return std::dynamic_pointer_cast<T>(Lookup(handle));
  }
  // Finds (or creates, for event-shaped headers) the host object for a guest
  // dispatcher header. Returns null for header types we do not model.
  std::shared_ptr<KObject> FromGuestHeader(uint32_t guest_address);
  void BindGuestHeader(uint32_t guest_address, const std::shared_ptr<KObject>& object);
  // Drops the binding for a guest address. Guest memory is REUSED - a critical
  // section or event living on a thread stack sits where a dead thread's one
  // used to - so whenever the guest (re)initialises a dispatcher object in
  // place, the old host object bound to that address must go, or the next
  // wait finds a stale Event with someone else's signal state. Call this from
  // every "initialise in place" import.
  void UnbindGuestHeader(uint32_t guest_address);

 private:
  std::mutex mutex_;
  uint32_t next_handle_ = 0x00000010;
  std::unordered_map<uint32_t, std::shared_ptr<KObject>> handles_;
  // A binding records the SHAPE of the guest header we saw when we attached.
  // The header is authoritative: if the guest has re-initialised the object in
  // place its type or size will differ, and the binding is dropped without
  // anyone having to tell us. Our own stamp cannot do this alone, because a
  // reused header carries the stamp along with it.
  struct Binding {
    std::shared_ptr<KObject> object;
    uint8_t type;
    uint8_t size;
  };
  std::unordered_map<uint32_t, Binding> by_guest_;
  // Creates and registers an object; the caller must hold mutex_.
  uint32_t AddLocked(const std::shared_ptr<KObject>& object, uint32_t guest_object);
};

// ---------------------------------------------------------------------------
// Wait diagnostics.
//
// A thread blocked on an object that is ALREADY available is a lost wakeup, and
// a dump that says so names the bug outright - much faster to read than "busy
// versus waiting". Every blocking wait registers what it is waiting on for as
// long as it blocks; DumpWaits() prints one line per blocked thread with each
// object's guest address, type, and whether a wait on it would succeed right
// now. Raise SIGUSR1 to trigger it.
struct WaitEntry {
  uint32_t guest_address;
  const char* type;
  bool available;    // PollLocked(): would a wait on this succeed now?
  std::string state; // owner / count / signalled, so a held object names its holder
  uint32_t handle;   // host object identity: a signal landing on a DIFFERENT
                     // handle for the same guest address is a stale binding
};
void DumpWaits(const char* reason);
void InstallWaitDumpSignalHandler();

// RAII: registers the calling thread as blocked on these objects.
class WaitScope {
 public:
  WaitScope(const std::vector<std::shared_ptr<KObject>>& objects, const char* kind);
  ~WaitScope();
 private:
  bool registered_ = false;
};

// Guest wait timeout (100 ns units; negative = relative, positive = absolute
// FILETIME, 0 = poll) to a host duration. nullopt = infinite.
std::optional<std::chrono::nanoseconds> TimeoutFromGuest(const uint64_t* timeout_ptr);

}  // namespace xn
