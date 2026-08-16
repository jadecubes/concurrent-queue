// Thread harness shared by the queue test files (and by future queue
// variants' tests).
#ifndef CQ_TESTS_QUEUE_TEST_UTIL_HPP_
#define CQ_TESTS_QUEUE_TEST_UTIL_HPP_

#include <chrono>
#include <cstddef>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace cq {

// Long enough for a spawned thread to reach its blocking call; the tests stay
// correct (just less interesting) if it ever proves too short.
constexpr auto kSettleTime = std::chrono::milliseconds(5);

// Runs blocked_op on its own thread, confirms it is still blocked after
// kSettleTime, runs unblock, and returns blocked_op's result.
[[nodiscard]] auto run_blocked(auto&& blocked_op, auto&& unblock) {
  auto pending = std::async(std::launch::async, blocked_op);
  EXPECT_EQ(pending.wait_for(kSettleTime), std::future_status::timeout)
      << "operation returned without ever blocking";
  unblock();
  return pending.get();
}

// Launches count threads, each running body(thread_index). Hold the returned
// vector: dropping it joins every thread immediately.
[[nodiscard]] std::vector<std::jthread> spawn_threads(int count, auto body) {
  std::vector<std::jthread> threads;
  threads.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    threads.emplace_back([body, i] { body(i); });
  }
  return threads;
}

// inline: unlike the auto-parameter helpers above this is not a template, and
// the header is included from more than one TU.
inline void join_all(std::vector<std::jthread>& threads) {
  for (auto& t : threads) {
    t.join();
  }
}

}  // namespace cq

#endif  // CQ_TESTS_QUEUE_TEST_UTIL_HPP_
