// Thread harness shared by the queue test files (and by future queue
// variants' tests).
#ifndef CQ_TESTS_QUEUE_TEST_UTIL_HPP_
#define CQ_TESTS_QUEUE_TEST_UTIL_HPP_

#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

namespace cq {

// Long enough for a spawned thread to reach its blocking call; the tests stay
// correct (just less interesting) if it ever proves too short.
constexpr auto kSettleTime = std::chrono::milliseconds(20);

// Runs blocked_op on its own thread, gives it kSettleTime to reach its
// blocking call, runs unblock, and returns blocked_op's result after joining.
// The explicit join is load-bearing: result must not be read before it.
bool run_blocked(auto&& blocked_op, auto&& unblock) {
  bool result = false;
  std::jthread worker([&] { result = blocked_op(); });
  std::this_thread::sleep_for(kSettleTime);
  unblock();
  worker.join();
  return result;
}

// Launches count threads, each running body(thread_index).
std::vector<std::jthread> spawn_threads(int count, auto body) {
  std::vector<std::jthread> threads;
  threads.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    threads.emplace_back([body, i] { body(i); });
  }
  return threads;
}

}  // namespace cq

#endif  // CQ_TESTS_QUEUE_TEST_UTIL_HPP_
