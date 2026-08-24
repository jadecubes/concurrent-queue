// SpscQueue-specific tests (overflow guard, v2.1 cache paths); the shared
// contract lives in queue_contract_test.cpp.

#include <cq/spsc_queue.hpp>

#include <cstddef>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace cq {
namespace {

TEST(SpscQueue, MaxCapacityThrows) {
  // capacity + 1 would wrap SIZE_MAX to 0 and build a broken ring.
  constexpr auto kMaxCapacity = std::numeric_limits<std::size_t>::max();
  EXPECT_THROW(SpscQueue<int>{kMaxCapacity}, std::length_error);
}

// One slot: both caches are stale on every op and must refresh. A cache-only
// implementation fails at the first try_pop (a blocking pop would spin).
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

// Fill/drain cycles walk the stale-cache refresh path and wrap the ring
// repeatedly.
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

}  // namespace
}  // namespace cq
