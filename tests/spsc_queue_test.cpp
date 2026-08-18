// Unit tests for cq::SpscQueue (v2: lock-free single-producer single-consumer
// bounded ring buffer).

#include <cq/spsc_queue.hpp>

#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "queue_test_util.hpp"

namespace cq {
namespace {

TEST(SpscQueue, StartsEmptyWithGivenCapacity) {
  const SpscQueue<int> q(4);
  EXPECT_EQ(q.capacity(), 4U);
  EXPECT_EQ(q.size(), 0U);
  EXPECT_FALSE(q.closed());
}

TEST(SpscQueue, ZeroCapacityThrows) { EXPECT_THROW(SpscQueue<int>(0), std::invalid_argument); }

TEST(SpscQueue, PopsInFifoOrder) {
  SpscQueue<int> q(4);
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

TEST(SpscQueue, WrapsAroundRingBoundary) {
  constexpr int kRoundTrips = 10;
  SpscQueue<int> q(2);
  int out = 0;
  for (int i = 0; i < kRoundTrips; ++i) {
    ASSERT_TRUE(q.push(i));
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out, i);
  }
}

TEST(SpscQueue, FillsToExactlyCapacity) {
  SpscQueue<int> q(3);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_TRUE(q.try_push(3));
  EXPECT_FALSE(q.try_push(4));
  EXPECT_EQ(q.size(), 3U);
}

TEST(SpscQueue, TryPushFailsWhenFull) {
  SpscQueue<int> q(2);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_FALSE(q.try_push(3));
  EXPECT_EQ(q.size(), 2U);
}

TEST(SpscQueue, TryPopFailsWhenEmpty) {
  SpscQueue<int> q(2);
  int out = 0;
  EXPECT_FALSE(q.try_pop(out));
}

TEST(SpscQueue, SupportsMoveOnlyTypes) {
  SpscQueue<std::unique_ptr<int>> q(2);
  ASSERT_TRUE(q.push(std::make_unique<int>(42)));

  std::unique_ptr<int> out;
  ASSERT_TRUE(q.pop(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(*out, 42);
}

TEST(SpscQueue, PopBlocksUntilPush) {
  SpscQueue<int> q(1);
  int out = 0;
  EXPECT_TRUE(test_util::run_blocked([&] { return q.pop(out); },  //
                                     [&] { EXPECT_TRUE(q.push(7)); }));
  EXPECT_EQ(out, 7);
}

TEST(SpscQueue, PushBlocksUntilPopWhenFull) {
  SpscQueue<int> q(1);
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

TEST(SpscQueue, PushAfterCloseFails) {
  SpscQueue<int> q(2);
  q.close();
  EXPECT_TRUE(q.closed());
  EXPECT_FALSE(q.push(1));
  EXPECT_FALSE(q.try_push(1));
}

TEST(SpscQueue, PopDrainsRemainingItemsAfterClose) {
  SpscQueue<int> q(4);
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

TEST(SpscQueue, CloseWakesBlockedPop) {
  SpscQueue<int> q(1);
  int out = 0;
  EXPECT_FALSE(test_util::run_blocked([&] { return q.pop(out); },  //
                                      [&] { q.close(); }));
}

TEST(SpscQueue, CloseWakesBlockedPush) {
  SpscQueue<int> q(1);
  ASSERT_TRUE(q.push(1));
  EXPECT_FALSE(test_util::run_blocked([&] { return q.push(2); },  //
                                      [&] { q.close(); }));
}

}  // namespace
}  // namespace cq
