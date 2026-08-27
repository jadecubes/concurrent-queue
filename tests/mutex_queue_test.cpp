// Unit tests for cq::MutexQueue (v1: mutex + condition_variable bounded queue).

#include <cq/mutex_queue.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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

// The header promises the queue's invariants survive a throwing move
// assignment, and explicitly does NOT promise element values do. Nothing else
// in the suite can reach that path -- every other T here has a noexcept move
// assignment -- so this pins both halves against the stronger claim the
// contract used to make.
//
// The throw is armed by a counter rather than a flag on the element, so the
// element can be enqueued normally and turned hostile only for the dequeue.
int g_moves_until_throw = -1;      // negative: never throw
constexpr int kStolenMarker = -1;  // what a stolen-from value is left holding
constexpr int kSentinel = 99;      // pre-loaded into out, to see if it survives
constexpr int kHostileValue = 7;   // payload of the element whose push throws

// NOLINTBEGIN(misc-non-private-member-variables-in-classes) -- a two-field
// test payload; accessors would only obscure what the assertions check.
struct ThrowingMove {
  int value = 0;
  std::string label;

  ThrowingMove() = default;
  ThrowingMove(int v, std::string l) : value(v), label(std::move(l)) {}
  ThrowingMove(ThrowingMove&&) = default;
  ThrowingMove(const ThrowingMove&) = delete;
  ThrowingMove& operator=(const ThrowingMove&) = delete;
  ~ThrowingMove() = default;

  // Throwing from a move assignment is the entire point of this type, so the
  // two checks that forbid it are suppressed rather than satisfied.
  // NOLINTNEXTLINE(performance-noexcept-move-constructor,bugprone-exception-escape)
  ThrowingMove& operator=(ThrowingMove&& other) {
    value = std::exchange(other.value, kStolenMarker);  // first member stolen...
    const bool armed = g_moves_until_throw == 0;
    if (g_moves_until_throw >= 0) {
      --g_moves_until_throw;
    }
    if (armed) {
      throw std::runtime_error("move assignment failed");  // ...then this throws
    }
    label = std::move(other.label);
    return *this;
  }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

TEST(MutexQueue, ThrowingPopKeepsQueueInvariantsButNotElementValues) {
  MutexQueue<ThrowingMove> q(4);
  ASSERT_TRUE(q.try_push({1, "one"}));
  ASSERT_TRUE(q.try_push({2, "two"}));

  ThrowingMove out{kSentinel, "sentinel"};
  g_moves_until_throw = 0;  // the next move assignment is the pop's
  EXPECT_THROW(static_cast<void>(q.try_pop(out)), std::runtime_error);
  g_moves_until_throw = -1;

  // Queue side: the indices never moved, so nothing was lost or duplicated.
  EXPECT_EQ(q.size(), 2U);

  // Element side: the header promises nothing, and indeed both the
  // destination and the still-queued element were disturbed.
  EXPECT_EQ(out.value, 1) << "out was modified despite the failure";
  EXPECT_EQ(out.label, "sentinel") << "the throw landed between the two members";

  ThrowingMove retry;
  ASSERT_TRUE(q.try_pop(retry));
  EXPECT_EQ(retry.value, kStolenMarker) << "retrying yields a hollowed element, not the original";
  EXPECT_EQ(retry.label, "one");

  ThrowingMove neighbour;
  ASSERT_TRUE(q.try_pop(neighbour));
  EXPECT_EQ(neighbour.value, 2) << "the next element is untouched";
  EXPECT_EQ(q.size(), 0U) << "count still reconciles, which is why a checksum harness misses this";
}

TEST(MutexQueue, ThrowingPushEnqueuesNothing) {
  // The push side genuinely upholds its guarantee: enqueue_locked writes the
  // slot before it advances the indices, so a throw leaves the queue empty.
  MutexQueue<ThrowingMove> q(2);
  ThrowingMove hostile{kHostileValue, "seven"};
  g_moves_until_throw = 0;
  EXPECT_THROW(static_cast<void>(q.try_push(std::move(hostile))), std::runtime_error);
  g_moves_until_throw = -1;
  EXPECT_EQ(q.size(), 0U);
}

// try_push and try_pop return false for "not now" and "never again" alike, so
// closed() is the only way a non-blocking loop can terminate. Pins the idiom
// the header documents; without the closed() test these loops never exit.
TEST(MutexQueue, NonBlockingRetryLoopsTerminateViaClosed) {
  MutexQueue<int> q(1);
  ASSERT_TRUE(q.try_push(1));
  q.close();

  int spins = 0;
  while (!q.try_push(2)) {
    if (q.closed()) {
      break;
    }
    ASSERT_LT(++spins, 1000) << "try_push loop never terminated";
  }
  EXPECT_TRUE(q.closed());

  int out = 0;
  ASSERT_TRUE(q.try_pop(out));  // the pre-close element still drains
  spins = 0;
  while (!q.try_pop(out)) {
    if (q.closed()) {
      break;
    }
    ASSERT_LT(++spins, 1000) << "try_pop loop never terminated";
  }
  EXPECT_EQ(q.size(), 0U);
}

}  // namespace
}  // namespace cq
