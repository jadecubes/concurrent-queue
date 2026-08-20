// Unit tests for cq::SpscQueue (v2: lock-free single-producer single-consumer
// bounded ring buffer).

#include <cq/spsc_queue.hpp>

#include <cstddef>
#include <limits>
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

TEST(SpscQueue, MaxCapacityThrows) {
  // The ring allocates capacity + 1 slots; SIZE_MAX would wrap that to 0 and
  // silently construct a broken queue instead of failing to allocate.
  constexpr auto kMaxCapacity = std::numeric_limits<std::size_t>::max();
  EXPECT_THROW(SpscQueue<int>{kMaxCapacity}, std::length_error);
}

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

// A one-slot ring is full after every push and empty after every pop, so each
// side's cached view of the opposite index is stale on every single operation
// and has to be refreshed. An implementation that consults its cache but never
// re-reads the real index deadlocks here on the first try_push.
TEST(SpscQueue, SingleSlotRingAlternatesPushAndPop) {
  constexpr int kRoundTrips = 100;
  SpscQueue<int> q(1);
  int out = 0;
  for (int i = 0; i < kRoundTrips; ++i) {
    ASSERT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(i)) << "one slot, so the second push must fail";
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, i);
    EXPECT_FALSE(q.try_pop(out)) << "drained, so the second pop must fail";
  }
}

// Fill to capacity, drain to empty, repeat. Each cycle walks both sides from
// "cache says there is room/data" through the stale-cache refresh and back,
// and enough cycles to carry the ring past its wrap point several times.
TEST(SpscQueue, RepeatedFillAndDrainCyclesPreserveFifoOrder) {
  constexpr int kCycles = 20;
  constexpr int kCapacity = 3;
  SpscQueue<int> q(kCapacity);
  int out = 0;
  for (int cycle = 0; cycle < kCycles; ++cycle) {
    const int base = cycle * kCapacity;
    for (int i = 0; i < kCapacity; ++i) {
      ASSERT_TRUE(q.try_push(base + i));
    }
    EXPECT_FALSE(q.try_push(-1));
    EXPECT_EQ(q.size(), static_cast<std::size_t>(kCapacity));
    for (int i = 0; i < kCapacity; ++i) {
      ASSERT_TRUE(q.try_pop(out));
      EXPECT_EQ(out, base + i);
    }
    EXPECT_FALSE(q.try_pop(out));
    EXPECT_EQ(q.size(), 0U);
  }
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
