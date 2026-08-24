// SpscQueue-specific tests: the capacity+1 overflow guard and the v2.1
// peer-index-cache paths. The family-wide contract is covered once for all
// queues in queue_contract_test.cpp.

#include <cq/spsc_queue.hpp>

#include <cstddef>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace cq {
namespace {

TEST(SpscQueue, MaxCapacityThrows) {
  // The ring allocates capacity + 1 slots; SIZE_MAX would wrap that to 0 and
  // silently construct a broken queue instead of failing to allocate.
  constexpr auto kMaxCapacity = std::numeric_limits<std::size_t>::max();
  EXPECT_THROW(SpscQueue<int>{kMaxCapacity}, std::length_error);
}

// A one-slot ring is full after every push and empty after every pop, so each
// side's cached view of the opposite index is stale on every single operation
// and has to be refreshed. An implementation that consults its cache but never
// re-reads the real index fails at the first try_pop — its tail cache never
// learns of the push (the fresh zero cache happens to license the first push,
// and the blocking variants would spin forever instead of returning false).
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

}  // namespace
}  // namespace cq
