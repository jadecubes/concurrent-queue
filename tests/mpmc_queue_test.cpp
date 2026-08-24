// Unit tests for cq::MpmcQueue (v2.5: Vyukov-style bounded MPMC queue with
// per-slot sequence counters).

#include <cq/mpmc_queue.hpp>

#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "queue_test_util.hpp"

namespace cq {
namespace {

TEST(MpmcQueue, StartsEmptyWithGivenCapacity) {
  const MpmcQueue<int> q(4);
  EXPECT_EQ(q.capacity(), 4U);
  EXPECT_EQ(q.size(), 0U);
  EXPECT_FALSE(q.closed());
}

TEST(MpmcQueue, ZeroCapacityThrows) { EXPECT_THROW(MpmcQueue<int>(0), std::invalid_argument); }

TEST(MpmcQueue, PopsInFifoOrder) {
  MpmcQueue<int> q(4);
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

TEST(MpmcQueue, WrapsAroundRingBoundary) {
  constexpr int kRoundTrips = 10;
  MpmcQueue<int> q(2);
  int out = 0;
  for (int i = 0; i < kRoundTrips; ++i) {
    ASSERT_TRUE(q.push(i));
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out, i);
  }
}

// Capacity 3 on purpose: a non-power-of-two size exercises the modular
// indexing, which the classic mask-based Vyukov layout does not allow.
TEST(MpmcQueue, FillsToExactlyCapacity) {
  MpmcQueue<int> q(3);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_TRUE(q.try_push(3));
  EXPECT_FALSE(q.try_push(4));
  EXPECT_EQ(q.size(), 3U);
}

TEST(MpmcQueue, TryPushFailsWhenFull) {
  MpmcQueue<int> q(2);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_FALSE(q.try_push(3));
  EXPECT_EQ(q.size(), 2U);
}

TEST(MpmcQueue, TryPopFailsWhenEmpty) {
  MpmcQueue<int> q(2);
  int out = 0;
  EXPECT_FALSE(q.try_pop(out));
}

TEST(MpmcQueue, SupportsMoveOnlyTypes) {
  MpmcQueue<std::unique_ptr<int>> q(2);
  ASSERT_TRUE(q.push(std::make_unique<int>(42)));

  std::unique_ptr<int> out;
  ASSERT_TRUE(q.pop(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(*out, 42);
}

TEST(MpmcQueue, PopBlocksUntilPush) {
  MpmcQueue<int> q(1);
  int out = 0;
  EXPECT_TRUE(test_util::run_blocked([&] { return q.pop(out); },  //
                                     [&] { EXPECT_TRUE(q.push(7)); }));
  EXPECT_EQ(out, 7);
}

TEST(MpmcQueue, PushBlocksUntilPopWhenFull) {
  MpmcQueue<int> q(1);
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

TEST(MpmcQueue, PushAfterCloseFails) {
  MpmcQueue<int> q(2);
  q.close();
  EXPECT_TRUE(q.closed());
  EXPECT_FALSE(q.push(1));
  EXPECT_FALSE(q.try_push(1));
}

TEST(MpmcQueue, PopDrainsRemainingItemsAfterClose) {
  MpmcQueue<int> q(4);
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

TEST(MpmcQueue, CloseWakesBlockedPop) {
  MpmcQueue<int> q(1);
  int out = 0;
  EXPECT_FALSE(test_util::run_blocked([&] { return q.pop(out); },  //
                                      [&] { q.close(); }));
}

TEST(MpmcQueue, CloseWakesBlockedPush) {
  MpmcQueue<int> q(1);
  ASSERT_TRUE(q.push(1));
  EXPECT_FALSE(test_util::run_blocked([&] { return q.push(2); },  //
                                      [&] { q.close(); }));
}

}  // namespace
}  // namespace cq
