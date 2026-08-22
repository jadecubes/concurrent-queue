// Unit tests for cq::MutexQueue (v1: mutex + condition_variable bounded queue).

#include <cq/mutex_queue.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "queue_test_util.hpp"

namespace cq {
namespace {

TEST(MutexQueue, StartsEmptyWithGivenCapacity) {
  const MutexQueue<int> q(4);
  EXPECT_EQ(q.capacity(), 4U);
  EXPECT_EQ(q.size(), 0U);
  EXPECT_FALSE(q.closed());
}

TEST(MutexQueue, ZeroCapacityThrows) { EXPECT_THROW(MutexQueue<int>(0), std::invalid_argument); }

TEST(MutexQueue, PopsInFifoOrder) {
  MutexQueue<int> q(4);
  ASSERT_TRUE(q.push(1));
  ASSERT_TRUE(q.push(2));
  ASSERT_TRUE(q.push(3));

  int out = 0;
  ASSERT_TRUE(q.pop(out));
  EXPECT_EQ(out, 1);
  ASSERT_TRUE(q.pop(out));
  EXPECT_EQ(out, 2);
  ASSERT_TRUE(q.pop(out));
  EXPECT_EQ(out, 3);
  EXPECT_EQ(q.size(), 0U);
}

TEST(MutexQueue, WrapsAroundRingBoundary) {
  constexpr int kRoundTrips = 10;
  MutexQueue<int> q(2);
  int out = 0;
  for (int i = 0; i < kRoundTrips; ++i) {
    ASSERT_TRUE(q.push(i));
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out, i);
  }
}

TEST(MutexQueue, TryPushFailsWhenFull) {
  MutexQueue<int> q(2);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_FALSE(q.try_push(3));
  EXPECT_EQ(q.size(), 2U);
}

TEST(MutexQueue, TryPopFailsWhenEmpty) {
  MutexQueue<int> q(2);
  int out = 0;
  EXPECT_FALSE(q.try_pop(out));
}

TEST(MutexQueue, SupportsMoveOnlyTypes) {
  MutexQueue<std::unique_ptr<int>> q(2);
  ASSERT_TRUE(q.push(std::make_unique<int>(42)));

  std::unique_ptr<int> out;
  ASSERT_TRUE(q.pop(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(*out, 42);
}

TEST(MutexQueue, PopBlocksUntilPush) {
  MutexQueue<int> q(1);
  int out = 0;
  EXPECT_TRUE(test_util::run_blocked([&] { return q.pop(out); },  //
                                     [&] { EXPECT_TRUE(q.push(7)); }));
  EXPECT_EQ(out, 7);
}

TEST(MutexQueue, PushBlocksUntilPopWhenFull) {
  MutexQueue<int> q(1);
  ASSERT_TRUE(q.push(1));
  int out = 0;
  EXPECT_TRUE(test_util::run_blocked([&] { return q.push(2); },
                                     [&] {
                                       EXPECT_TRUE(q.pop(out));
                                       EXPECT_EQ(out, 1);
                                     }));
  ASSERT_TRUE(q.pop(out));
  EXPECT_EQ(out, 2);
}

TEST(MutexQueue, PushAfterCloseFails) {
  MutexQueue<int> q(2);
  q.close();
  EXPECT_TRUE(q.closed());
  EXPECT_FALSE(q.push(1));
  EXPECT_FALSE(q.try_push(1));
}

TEST(MutexQueue, PopDrainsRemainingItemsAfterClose) {
  MutexQueue<int> q(4);
  ASSERT_TRUE(q.push(1));
  ASSERT_TRUE(q.push(2));
  q.close();

  int out = 0;
  EXPECT_TRUE(q.pop(out));
  EXPECT_EQ(out, 1);
  EXPECT_TRUE(q.try_pop(out));
  EXPECT_EQ(out, 2);
  EXPECT_FALSE(q.pop(out));
  EXPECT_FALSE(q.try_pop(out));
}

TEST(MutexQueue, CloseWakesBlockedPop) {
  MutexQueue<int> q(1);
  int out = 0;
  EXPECT_FALSE(test_util::run_blocked([&] { return q.pop(out); },  //
                                      [&] { q.close(); }));
}

TEST(MutexQueue, CloseWakesBlockedPush) {
  MutexQueue<int> q(1);
  ASSERT_TRUE(q.push(1));
  EXPECT_FALSE(test_util::run_blocked([&] { return q.push(2); },  //
                                      [&] { q.close(); }));
}

// --- timed variants -------------------------------------------------------
//
// try_*_for sits between the two existing shapes: push/pop wait forever,
// try_push/try_pop do not wait at all. These wait up to a bound.

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

  // Must actually have waited: a half-timeout lower bound distinguishes this
  // from try_push's immediate failure without being flaky on clock jitter.
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
  // close() must cut the wait short rather than leaving the caller to serve
  // out a long timeout it can no longer satisfy.
  constexpr auto kLongTimeout = std::chrono::seconds(5);
  MutexQueue<int> q(1);
  ASSERT_TRUE(q.push(1));  // full, so try_push_for would otherwise wait

  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(
      test_util::run_blocked([&] { return q.try_push_for(2, kLongTimeout); }, [&] { q.close(); }));
  EXPECT_LT(std::chrono::steady_clock::now() - start, kLongTimeout / 2);
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
}

}  // namespace
}  // namespace cq
