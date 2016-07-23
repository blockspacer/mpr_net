#include "common/futex.h"
#include <stdint.h>
#include <string.h>
#include <condition_variable>
#include <mutex>
#include <boost/intrusive/list.hpp>

#include "fb/scope_guard.h"
#include "base/macros.h"

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>

using namespace std::chrono;

namespace fb { namespace detail {

namespace {

////////////////////////////////////////////////////
// native implementation using the futex() syscall

int nativeFutexWake(void* addr, int count, uint32_t wakeMask) {
  int rv = syscall(SYS_futex,
                   addr, /* addr1 */
                   FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG, /* op */
                   count, /* val */
                   nullptr, /* timeout */
                   nullptr, /* addr2 */
                   wakeMask); /* val3 */

  assert(rv >= 0);

  return rv;
}

template <class Clock>
struct timespec
timeSpecFromTimePoint(time_point<Clock> absTime)
{
  auto duration = absTime.time_since_epoch();
  if (duration.count() < 0) {
    // kernel timespec_valid requires non-negative seconds and nanos in [0,1G)
    duration = Clock::duration::zero();
  }
  auto secs = duration_cast<seconds>(duration);
  auto nanos = duration_cast<nanoseconds>(duration - secs);
  struct timespec result = { secs.count(), nanos.count() };
  return result;
}

FutexResult nativeFutexWaitImpl(void* addr,
                                uint32_t expected,
                                time_point<system_clock>* absSystemTime,
                                time_point<steady_clock>* absSteadyTime,
                                uint32_t waitMask) {
  assert(absSystemTime == nullptr || absSteadyTime == nullptr);

  int op = FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG;
  struct timespec ts;
  struct timespec* timeout = nullptr;

  if (absSystemTime != nullptr) {
    op |= FUTEX_CLOCK_REALTIME;
    ts = timeSpecFromTimePoint(*absSystemTime);
    timeout = &ts;
  } else if (absSteadyTime != nullptr) {
    ts = timeSpecFromTimePoint(*absSteadyTime);
    timeout = &ts;
  }

  // Unlike FUTEX_WAIT, FUTEX_WAIT_BITSET requires an absolute timeout
  // value - http://locklessinc.com/articles/futex_cheat_sheet/
  int rv = syscall(SYS_futex,
                   addr, /* addr1 */
                   op, /* op */
                   expected, /* val */
                   timeout, /* timeout */
                   nullptr, /* addr2 */
                   waitMask); /* val3 */

  if (rv == 0) {
    return FutexResult::AWOKEN;
  } else {
    switch(errno) {
      case ETIMEDOUT:
        assert(timeout != nullptr);
        return FutexResult::TIMEDOUT;
      case EINTR:
        return FutexResult::INTERRUPTED;
      case EWOULDBLOCK:
        return FutexResult::VALUE_CHANGED;
      default:
        assert(false);
        // EINVAL, EACCESS, or EFAULT.  EINVAL means there was an invalid
        // op (should be impossible) or an invalid timeout (should have
        // been sanitized by timeSpecFromTimePoint).  EACCESS or EFAULT
        // means *addr points to invalid memory, which is unlikely because
        // the caller should have segfaulted already.  We can either
        // crash, or return a value that lets the process continue for
        // a bit. We choose the latter. VALUE_CHANGED probably turns the
        // caller into a spin lock.
        return FutexResult::VALUE_CHANGED;
    }
  }
}

///////////////////////////////////////////////////////
// compatibility implementation using standard C++ API

// Our emulated futex uses 4096 lists of wait nodes.  There are two levels
// of locking: a per-list mutex that controls access to the list and a
// per-node mutex, condvar, and bool that are used for the actual wakeups.
// The per-node mutex allows us to do precise wakeups without thundering
// herds.

struct EmulatedFutexWaitNode : public boost::intrusive::list_base_hook<> {
  void* const addr_;
  const uint32_t waitMask_;

  // tricky: hold both bucket and node mutex to write, either to read
  bool signaled_;
  std::mutex mutex_;
  std::condition_variable cond_;

  EmulatedFutexWaitNode(void* addr, uint32_t waitMask)
    : addr_(addr)
    , waitMask_(waitMask)
    , signaled_(false)
  {
  }
};

struct EmulatedFutexBucket {
  std::mutex mutex_;
  boost::intrusive::list<EmulatedFutexWaitNode> waiters_;

  static const size_t kNumBuckets = 4096;
  static EmulatedFutexBucket* gBuckets;
  static std::once_flag gBucketInit;

  static EmulatedFutexBucket& bucketFor(void* addr) {
    std::call_once(gBucketInit, [](){
      gBuckets = new EmulatedFutexBucket[kNumBuckets];
    });
    uint64_t mixedBits = twang_mix64(
        reinterpret_cast<uintptr_t>(addr));
    return gBuckets[mixedBits % kNumBuckets];
  }
};

EmulatedFutexBucket* EmulatedFutexBucket::gBuckets;
std::once_flag EmulatedFutexBucket::gBucketInit;

int emulatedFutexWake(void* addr, int count, uint32_t waitMask) {
  auto& bucket = EmulatedFutexBucket::bucketFor(addr);
  std::unique_lock<std::mutex> bucketLock(bucket.mutex_);

  int numAwoken = 0;
  for (auto iter = bucket.waiters_.begin();
       numAwoken < count && iter != bucket.waiters_.end(); ) {
    auto current = iter;
    auto& node = *iter++;
    if (node.addr_ == addr && (node.waitMask_ & waitMask) != 0) {
      ++numAwoken;

      // we unlink, but waiter destroys the node
      bucket.waiters_.erase(current);

      std::unique_lock<std::mutex> nodeLock(node.mutex_);
      node.signaled_ = true;
      node.cond_.notify_one();
    }
  }
  return numAwoken;
}

FutexResult emulatedFutexWaitImpl(
        void* addr,
        uint32_t expected,
        time_point<system_clock>* absSystemTime,
        time_point<steady_clock>* absSteadyTime,
        uint32_t waitMask) {
  auto& bucket = EmulatedFutexBucket::bucketFor(addr);
  EmulatedFutexWaitNode node(addr, waitMask);

  {
    std::unique_lock<std::mutex> bucketLock(bucket.mutex_);

    uint32_t actual;
    memcpy(&actual, addr, sizeof(uint32_t));
    if (actual != expected) {
      return FutexResult::VALUE_CHANGED;
    }

    bucket.waiters_.push_back(node);
  } // bucketLock scope

  std::cv_status status = std::cv_status::no_timeout;
  {
    std::unique_lock<std::mutex> nodeLock(node.mutex_);
    while (!node.signaled_ && status != std::cv_status::timeout) {
      if (absSystemTime != nullptr) {
        status = node.cond_.wait_until(nodeLock, *absSystemTime);
      } else if (absSteadyTime != nullptr) {
        status = node.cond_.wait_until(nodeLock, *absSteadyTime);
      } else {
        node.cond_.wait(nodeLock);
      }
    }
  } // nodeLock scope

  if (status == std::cv_status::timeout) {
    // it's not really a timeout until we unlink the unsignaled node
    std::unique_lock<std::mutex> bucketLock(bucket.mutex_);
    if (!node.signaled_) {
      bucket.waiters_.erase(bucket.waiters_.iterator_to(node));
      return FutexResult::TIMEDOUT;
    }
  }
  return FutexResult::AWOKEN;
}

} // anon namespace


/////////////////////////////////
// Futex<> specializations

template <>
int
Futex<std::atomic>::futexWake(int count, uint32_t wakeMask) {
  return nativeFutexWake(this, count, wakeMask);
}

template <>
int
Futex<EmulatedFutexAtomic>::futexWake(int count, uint32_t wakeMask) {
  return emulatedFutexWake(this, count, wakeMask);
}

template <>
FutexResult
Futex<std::atomic>::futexWaitImpl(uint32_t expected,
                                  time_point<system_clock>* absSystemTime,
                                  time_point<steady_clock>* absSteadyTime,
                                  uint32_t waitMask) {
  return nativeFutexWaitImpl(
      this, expected, absSystemTime, absSteadyTime, waitMask);
}

template <>
FutexResult
Futex<EmulatedFutexAtomic>::futexWaitImpl(
        uint32_t expected,
        time_point<system_clock>* absSystemTime,
        time_point<steady_clock>* absSteadyTime,
        uint32_t waitMask) {
  return emulatedFutexWaitImpl(
      this, expected, absSystemTime, absSteadyTime, waitMask);
}

}} // namespace folly::detail
