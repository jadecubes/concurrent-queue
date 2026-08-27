// Retry helper shared by the non-blocking throughput benchmarks.
#ifndef CQ_BENCH_TRY_OPERATION_HPP_
#define CQ_BENCH_TRY_OPERATION_HPP_

#include <concepts>
#include <cstddef>
#include <thread>
#include <type_traits>

namespace cq::bench {

// Retry until the operation succeeds, returning how many attempts failed.
//
// The yield is a deliberate policy choice, and it is not neutral — it is worth
// naming because it moves the numbers more than the queues do. Measured at
// capacity 1, where every op waits on its counterpart:
//
//     MutexQueue   0.58 M/s without → 5.11 M/s with   (yield buys 8.8x)
//     SpscQueue   20.9  M/s without → 7.84 M/s with   (yield costs 2.7x)
//     MpmcQueue   21.1  M/s without → 6.84 M/s with   (yield costs 3.1x)
//
// The mechanism only exists for the mutex queue: try_push and try_pop take a
// blocking lock_guard, so an unyielding spinner keeps barging the lock back
// from the counterpart that would have made room. The lock-free queues have no
// lock to barge, so there the yield is pure overhead.
//
// It stays anyway: without it the mutex row degenerates (155 retries per op at
// capacity 1) and a CI runner with two vCPUs would be far worse than this
// 12-core box. But any ratio taken from these rows is a statement about this
// policy as much as about the queues — see the sweep's comment in
// queue_bench.cpp.
//
// By value, per the std:: algorithm convention: the operation is invoked
// repeatedly, so a forwarding reference would never actually be forwarded.
//
// Not std::predicate: that subsumes std::regular_invocable, whose contract is
// equality-preserving invocation, and this helper exists precisely to call
// something whose answer changes between calls.
template <typename Operation>
  requires std::invocable<Operation&> && std::convertible_to<std::invoke_result_t<Operation&>, bool>
[[nodiscard]] std::size_t count_failures_until_success(Operation operation) {
  std::size_t failures = 0;
  while (!operation()) {
    ++failures;
    std::this_thread::yield();
  }
  return failures;
}

}  // namespace cq::bench

#endif  // CQ_BENCH_TRY_OPERATION_HPP_
