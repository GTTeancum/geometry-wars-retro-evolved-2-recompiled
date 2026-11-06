// Xbox stand-in for rex/thread/mutex.h: the guest's global interrupt lock
// (mfmsr/mtmsrd pairs) as one recursive host lock.
#pragma once

namespace rex::thread {

class RecursiveLock {
 public:
  void lock();
  void unlock();
};

struct global_critical_region {
  static RecursiveLock& mutex();
  struct Guard {
    Guard() { mutex().lock(); }
    ~Guard() { mutex().unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
  };
  static Guard AcquireDirect() { return {}; }
};

}  // namespace rex::thread
