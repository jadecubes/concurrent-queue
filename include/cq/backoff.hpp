#ifndef CQ_BACKOFF_HPP_
#define CQ_BACKOFF_HPP_

#include <algorithm>
#include <chrono>
#include <thread>

namespace cq {

// Blocked-side wait policy: yield briefly, then sleep with doubling backoff.
// The sleeping side pays for its own wakeup latency (bounded by kMaxSleep);
// the publish path pays nothing at all — both alternatives measured worse. A
// futex gate the publisher must check cost 70% of the SPSC pair throughput,
// and even inlining this wait's sleep machinery into push()/pop() cost 4x on
// the uncontended round trip, which is why wait() is noinline.
/// wait() calls that yield before the first sleep.
inline constexpr int kSpinsBeforeSleep = 64;
/// First sleep duration; doubles on each subsequent sleep.
inline constexpr auto kInitialSleep = std::chrono::microseconds{4};
/// Sleep cap — the bound on wakeup latency once a waiter is asleep.
inline constexpr auto kMaxSleep = std::chrono::microseconds{1000};

#if defined(__GNUC__) || defined(__clang__)
#define CQ_NOINLINE [[gnu::noinline]]
#else
#define CQ_NOINLINE
#endif

/// One blocked wait: construct fresh, call wait() each time the predicate
/// still fails. Yields for the first kSpinsBeforeSleep calls, then sleeps
/// with doubling duration up to kMaxSleep.
class Backoff {
 public:
  /// One step of the schedule: a yield while spins remain, then a sleep
  /// whose duration doubles up to kMaxSleep. noinline keeps the cold sleep
  /// machinery out of the caller's inlining budget (see the file comment).
  CQ_NOINLINE void wait();

 private:
  int spins_ = 0;
  std::chrono::microseconds delay_ = kInitialSleep;
};

#undef CQ_NOINLINE

inline void Backoff::wait() {
  if (spins_ < kSpinsBeforeSleep) {
    ++spins_;
    std::this_thread::yield();
    return;
  }
  std::this_thread::sleep_for(delay_);
  delay_ = std::min(delay_ * 2, kMaxSleep);
}

}  // namespace cq

#endif  // CQ_BACKOFF_HPP_
