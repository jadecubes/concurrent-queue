// SpscQueue-specific tests (overflow guard, v2.1 cache paths); the shared
// contract lives in queue_contract_test.cpp.

#include <cq/spsc_queue.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
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

// --- bulk operations ------------------------------------------------------
//
// try_push_n / try_pop_n move up to n items with one index publish per call.

TEST(SpscQueue, TryPushNStopsAtCapacity) {
  SpscQueue<int> q(4);
  ASSERT_TRUE(q.try_push(0));
  ASSERT_TRUE(q.try_push(1));
  std::array<int, 4> items = {2, 3, 4, 4};                  // the last item won't fit anyway
  EXPECT_EQ(q.try_push_n(items.data(), items.size()), 2U);  // only two slots left
  EXPECT_EQ(q.size(), 4U);
  int out = 0;
  for (int expected = 0; expected < 4; ++expected) {
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, expected);
  }
}

TEST(SpscQueue, TryPopNStopsAtAvailable) {
  SpscQueue<int> q(4);
  ASSERT_TRUE(q.try_push(7));
  ASSERT_TRUE(q.try_push(8));
  std::array<int, 4> out = {};
  EXPECT_EQ(q.try_pop_n(out.data(), out.size()), 2U);
  EXPECT_EQ(out[0], 7);
  EXPECT_EQ(out[1], 8);
  EXPECT_EQ(q.try_pop_n(out.data(), out.size()), 0U);  // empty
}

TEST(SpscQueue, BulkAndSingleOpsInterleavePreservingFifo) {
  SpscQueue<int> q(3);
  std::array<int, 2> items = {1, 2};
  EXPECT_EQ(q.try_push_n(items.data(), items.size()), 2U);
  ASSERT_TRUE(q.try_push(3));
  std::array<int, 2> out = {};
  EXPECT_EQ(q.try_pop_n(out.data(), out.size()), 2U);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 2);
  int single = 0;
  ASSERT_TRUE(q.try_pop(single));
  EXPECT_EQ(single, 3);
}

TEST(SpscQueue, BulkOpsWrapTheRing) {
  constexpr int kCycles = 10;
  SpscQueue<int> q(3);
  std::array<int, 3> out = {};
  for (int cycle = 0; cycle < kCycles; ++cycle) {
    std::array<int, 3> items = {cycle * 3, (cycle * 3) + 1, (cycle * 3) + 2};
    ASSERT_EQ(q.try_push_n(items.data(), items.size()), 3U);
    ASSERT_EQ(q.try_pop_n(out.data(), out.size()), 3U);
    EXPECT_EQ(out[0], cycle * 3);
    EXPECT_EQ(out[2], (cycle * 3) + 2);
  }
}

TEST(SpscQueue, TryPushNAfterCloseReturnsZero) {
  SpscQueue<int> q(4);
  q.close();
  std::array<int, 2> items = {1, 2};
  EXPECT_EQ(q.try_push_n(items.data(), items.size()), 0U);
}

// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks): the analyzer cannot
// see the ownership handoff through the ring on libstdc++ and reports a
// false leak; the asserts below prove both pointers arrive intact.
TEST(SpscQueue, BulkOpsSupportMoveOnlyTypes) {
  SpscQueue<std::unique_ptr<int>> q(4);
  std::array<std::unique_ptr<int>, 2> in = {std::make_unique<int>(1), std::make_unique<int>(2)};
  EXPECT_EQ(q.try_push_n(in.data(), in.size()), 2U);
  EXPECT_EQ(in[0], nullptr) << "pushed items must be moved from";
  std::array<std::unique_ptr<int>, 2> out;
  EXPECT_EQ(q.try_pop_n(out.data(), out.size()), 2U);
  EXPECT_EQ(*out[0], 1);
  EXPECT_EQ(*out[1], 2);
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

}  // namespace
}  // namespace cq
