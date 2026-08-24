// MutexQueue-specific tests (the timed variants); the shared contract
// lives in queue_contract_test.cpp.

#include <cq/mutex_queue.hpp>

#include <chrono>

#include <gtest/gtest.h>

#include "queue_test_util.hpp"

namespace cq {
namespace {

// try_*_for waits up to a bound — between push/pop (forever) and try_ (never).

TEST(MutexQueue, TryPushForSucceedsImmediatelyWhenNotFull) {
  MutexQueue<int> q(2);
  EXPECT_TRUE(q.try_push_for(1, std::chrono::seconds(5)));
  EXPECT_EQ(q.size(), 1U);
}

TEST(MutexQueue, TryPopForSucceedsImmediatelyWhenNonEmpty) {
  MutexQueue<int> q(2);
  ASSERT_TRUE(q.push(9));
  int out = 0;
  EXPECT_TRUE(q.try_pop_for(out, std::chrono::seconds(5)));
  EXPECT_EQ(out, 9);
}

TEST(MutexQueue, TryPushForTimesOutWhenFull) {
  constexpr auto kTimeout = std::chrono::milliseconds(20);
  MutexQueue<int> q(1);
  ASSERT_TRUE(q.push(1));

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(q.try_push_for(2, kTimeout));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  // Half-timeout lower bound: proves it waited, without clock-jitter flakes.
  EXPECT_GE(elapsed, kTimeout / 2);
  EXPECT_EQ(q.size(), 1U) << "a timed-out push must not enqueue";
}

TEST(MutexQueue, TryPopForTimesOutWhenEmpty) {
  constexpr auto kTimeout = std::chrono::milliseconds(20);
  MutexQueue<int> q(1);
  int out = 0;

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(q.try_pop_for(out, kTimeout));
  EXPECT_GE(std::chrono::steady_clock::now() - start, kTimeout / 2);
}

TEST(MutexQueue, TryPushForSucceedsWhenSlotFreesInTime) {
  MutexQueue<int> q(1);
  ASSERT_TRUE(q.push(1));
  int out = 0;
  // Timeout far longer than the handoff: this must succeed, not time out.
  EXPECT_TRUE(test_util::run_blocked([&] { return q.try_push_for(2, std::chrono::seconds(5)); },
                                     [&] {
                                       EXPECT_TRUE(q.pop(out));
                                       EXPECT_EQ(out, 1);
                                     }));
  ASSERT_TRUE(q.pop(out));
  EXPECT_EQ(out, 2);
}

TEST(MutexQueue, TryPopForSucceedsWhenItemArrivesInTime) {
  MutexQueue<int> q(1);
  int out = 0;
  EXPECT_TRUE(test_util::run_blocked([&] { return q.try_pop_for(out, std::chrono::seconds(5)); },
                                     [&] { EXPECT_TRUE(q.push(7)); }));
  EXPECT_EQ(out, 7);
}

TEST(MutexQueue, TimedOperationsReturnPromptlyWhenClosed) {
  // close() must cut the wait short.
  constexpr auto kLongTimeout = std::chrono::seconds(5);
  MutexQueue<int> q(1);
  ASSERT_TRUE(q.push(1));  // full, so try_push_for would otherwise wait

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(
      test_util::run_blocked([&] { return q.try_push_for(2, kLongTimeout); }, [&] { q.close(); }));
  EXPECT_LT(std::chrono::steady_clock::now() - start, kLongTimeout / 2);

  // closed() is how a caller tells this false from a timeout.
  EXPECT_TRUE(q.closed());
}

TEST(MutexQueue, TryPopForDrainsRemainingItemsAfterClose) {
  MutexQueue<int> q(2);
  ASSERT_TRUE(q.push(1));
  q.close();

  int out = 0;
  EXPECT_TRUE(q.try_pop_for(out, std::chrono::seconds(5))) << "close must not discard queued items";
  EXPECT_EQ(out, 1);
  EXPECT_FALSE(q.try_pop_for(out, std::chrono::milliseconds(0)));
}

TEST(MutexQueue, ZeroTimeoutDoesNotWait) {
  // Degenerates to try_push/try_pop rather than blocking or being rejected.
  MutexQueue<int> q(1);
  int out = 0;
  EXPECT_FALSE(q.try_pop_for(out, std::chrono::milliseconds(0)));
  ASSERT_TRUE(q.push(1));
  EXPECT_FALSE(q.try_push_for(2, std::chrono::milliseconds(0)));
  EXPECT_TRUE(q.try_pop_for(out, std::chrono::milliseconds(0)));
  EXPECT_EQ(out, 1);

  // Docs promise "non-positive": negative must behave like zero.
  EXPECT_FALSE(q.try_pop_for(out, std::chrono::milliseconds(-1)));
}

}  // namespace
}  // namespace cq
