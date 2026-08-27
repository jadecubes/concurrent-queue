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
// The yield is load-bearing, not politeness: without it a spinning side keeps
// barging the lock back from its counterpart, which then cannot make the
// progress that would let the spinner succeed. At capacity 1 or 2 every op
// depends on the counterpart, so one starvation episode dominates a whole run
// and the reported rate swings by orders of magnitude between invocations.
//
// The operation is taken by value, following the std:: algorithm convention:
// it is invoked repeatedly, so a forwarding reference would never actually be
// forwarded. Callers needing to observe its state changes capture by
// reference, as the benchmark call sites do.
//
// Deliberately not constrained with std::predicate: that subsumes
// std::regular_invocable, whose contract is equality-preserving invocation,
// and this helper exists precisely to call something whose answer changes
// between calls. It also constrains an rvalue F&&, while the body invokes an
// lvalue.
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
