// xenon-native: the kernel-wide interrupt lock used by REX_ENTER/LEAVE_GLOBAL_LOCK.
#pragma once
#include <mutex>
namespace rex::thread {
class global_critical_region {
 public:
  static std::recursive_mutex& mutex();
  static std::unique_lock<std::recursive_mutex> AcquireDirect() {
    return std::unique_lock<std::recursive_mutex>(mutex());
  }
  std::unique_lock<std::recursive_mutex> Acquire() { return AcquireDirect(); }
};
}  // namespace rex::thread
