// Unit tests for cq::MutexQueue (v1: mutex + condition_variable bounded queue).

#include <cq/mutex_queue.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

namespace cq {
namespace {

// Long enough for a spawned thread to reach its blocking call; the tests stay
// correct (just less interesting) if it ever proves too short.
constexpr auto kSettleTime = std::chrono::milliseconds(20);

// Runs blocked_op on its own thread, gives it kSettleTime to reach its
// blocking call, runs unblock, and returns blocked_op's result after joining.
bool run_blocked(auto&& blocked_op, auto&& unblock) {
  bool result = false;
  std::jthread worker([&] { result = blocked_op(); });
  std::this_thread::sleep_for(kSettleTime);
  unblock();
  worker.join();
  return result;
}

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
  EXPECT_TRUE(run_blocked([&] { return q.pop(out); },  //
                          [&] { EXPECT_TRUE(q.push(7)); }));
  EXPECT_EQ(out, 7);
}

TEST(MutexQueue, PushBlocksUntilPopWhenFull) {
  MutexQueue<int> q(1);
  ASSERT_TRUE(q.push(1));
  int out = 0;
  EXPECT_TRUE(run_blocked([&] { return q.push(2); },
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
  EXPECT_FALSE(run_blocked([&] { return q.pop(out); },  //
                           [&] { q.close(); }));
}

TEST(MutexQueue, CloseWakesBlockedPush) {
  MutexQueue<int> q(1);
  ASSERT_TRUE(q.push(1));
  EXPECT_FALSE(run_blocked([&] { return q.push(2); },  //
                           [&] { q.close(); }));
}

}  // namespace
}  // namespace cq
