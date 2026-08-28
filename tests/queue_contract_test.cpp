// The shared queue contract, run as typed tests over every queue.
// Queue-specific behavior stays in the per-queue test files.

#include <cq/mpmc_queue.hpp>
#include <cq/mutex_queue.hpp>
#include <cq/spsc_queue.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "queue_test_util.hpp"

namespace cq {
namespace {

// Typed tests take one type; a queue is a template, so wrap it in a tag.
struct MutexFamily {
  template <typename T>
  using Queue = MutexQueue<T>;
  static constexpr const char* kName = "Mutex";
};
struct SpscFamily {
  template <typename T>
  using Queue = SpscQueue<T>;
  static constexpr const char* kName = "Spsc";
};
struct MpmcFamily {
  template <typename T>
  using Queue = MpmcQueue<T>;
  static constexpr const char* kName = "Mpmc";
};

// Spell the dependent type names once.
template <typename Family>
class QueueContract : public ::testing::Test {
 protected:
  using IntQueue = typename Family::template Queue<int>;
  using MoveOnlyQueue = typename Family::template Queue<std::unique_ptr<int>>;
  using StringQueue = typename Family::template Queue<std::string>;
};

struct FamilyNames {
  // Name fixed by GoogleTest's API.
  template <typename Family>
  static std::string GetName(int /*index*/) {  // NOLINT(readability-identifier-naming)
    return Family::kName;
  }
};

using AllFamilies = ::testing::Types<MutexFamily, SpscFamily, MpmcFamily>;
TYPED_TEST_SUITE(QueueContract, AllFamilies, FamilyNames);

TYPED_TEST(QueueContract, StartsEmptyWithGivenCapacity) {
  const typename TestFixture::IntQueue q(4);
  EXPECT_EQ(q.capacity(), 4U);
  EXPECT_EQ(q.size(), 0U);
  EXPECT_FALSE(q.closed());
}

TYPED_TEST(QueueContract, ZeroCapacityThrows) {
  using IntQueue = typename TestFixture::IntQueue;
  EXPECT_THROW(IntQueue(0), std::invalid_argument);
}

TYPED_TEST(QueueContract, PopsInFifoOrder) {
  typename TestFixture::IntQueue q(4);
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

TYPED_TEST(QueueContract, WrapsAroundRingBoundary) {
  constexpr int kRoundTrips = 10;
  typename TestFixture::IntQueue q(2);
  int out = 0;
  for (int i = 0; i < kRoundTrips; ++i) {
    ASSERT_TRUE(q.push(i));
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out, i);
  }
}

// Non-power-of-two capacity catches off-by-one full detection in every ring.
TYPED_TEST(QueueContract, FillsToExactlyCapacity) {
  typename TestFixture::IntQueue q(3);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_TRUE(q.try_push(3));
  EXPECT_FALSE(q.try_push(4));
  EXPECT_EQ(q.size(), 3U);
}

TYPED_TEST(QueueContract, TryPushFailsWhenFull) {
  typename TestFixture::IntQueue q(2);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_FALSE(q.try_push(3));
  EXPECT_EQ(q.size(), 2U);
}

TYPED_TEST(QueueContract, TryPopFailsWhenEmpty) {
  typename TestFixture::IntQueue q(2);
  int out = 0;
  EXPECT_FALSE(q.try_pop(out));
}

TYPED_TEST(QueueContract, SupportsMoveOnlyTypes) {
  typename TestFixture::MoveOnlyQueue q(2);
  ASSERT_TRUE(q.push(std::make_unique<int>(42)));

  std::unique_ptr<int> out;
  ASSERT_TRUE(q.pop(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(*out, 42);
}

TYPED_TEST(QueueContract, PopBlocksUntilPush) {
  typename TestFixture::IntQueue q(1);
  int out = 0;
  EXPECT_TRUE(test_util::run_blocked([&] { return q.pop(out); },  //
                                     [&] { EXPECT_TRUE(q.push(7)); }));
  EXPECT_EQ(out, 7);
}

TYPED_TEST(QueueContract, PushBlocksUntilPopWhenFull) {
  typename TestFixture::IntQueue q(1);
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

TYPED_TEST(QueueContract, PushAfterCloseFails) {
  typename TestFixture::IntQueue q(2);
  q.close();
  EXPECT_TRUE(q.closed());
  EXPECT_FALSE(q.push(1));
  EXPECT_FALSE(q.try_push(1));
}

TYPED_TEST(QueueContract, PopDrainsRemainingItemsAfterClose) {
  typename TestFixture::IntQueue q(4);
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

TYPED_TEST(QueueContract, CloseWakesBlockedPop) {
  typename TestFixture::IntQueue q(1);
  int out = 0;
  EXPECT_FALSE(test_util::run_blocked([&] { return q.pop(out); },  //
                                      [&] { q.close(); }));
}

TYPED_TEST(QueueContract, CloseWakesBlockedPush) {
  typename TestFixture::IntQueue q(1);
  ASSERT_TRUE(q.push(1));
  EXPECT_FALSE(test_util::run_blocked([&] { return q.push(2); },  //
                                      [&] { q.close(); }));
}

// try_push()/try_pop() return false for "full/empty, retry" and for "closed,
// never" alike. closed() is the only discriminator, so a non-blocking loop
// that omits it never terminates. Pins the idiom the headers document.
TYPED_TEST(QueueContract, NonBlockingRetryLoopsTerminateViaClosed) {
  typename TestFixture::IntQueue q(1);
  ASSERT_TRUE(q.try_push(1));
  // Full but still open: try_push fails while closed() says "keep retrying".
  // Without this the loops below break on their first pass and the test would
  // pass even if closed() were stuck at true.
  ASSERT_FALSE(q.try_push(2));
  ASSERT_FALSE(q.closed()) << "closed() must distinguish full-and-open from closed";
  q.close();

  int spins = 0;
  while (!q.try_push(2)) {
    if (q.closed()) {
      break;
    }
    ASSERT_LT(++spins, 1000) << "try_push loop never terminated";
  }

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

// push() takes T by value, so a failed push consumes an rvalue argument while
// leaving an lvalue one intact. That asymmetry is why a try_push retry loop
// must re-materialise its argument, and why move-only values that cannot be
// re-created have no correct retry loop at all.
TYPED_TEST(QueueContract, FailedPushConsumesRvaluesAndLeavesLvaluesIntact) {
  typename TestFixture::StringQueue q(1);
  ASSERT_TRUE(q.try_push("filler"));  // now full: every push below fails

  const std::string lvalue = "still here";
  EXPECT_FALSE(q.try_push(lvalue));
  EXPECT_EQ(lvalue, "still here") << "an lvalue argument is copied, not consumed";

  std::string rvalue = "consumed";
  EXPECT_FALSE(q.try_push(std::move(rvalue)));
  // Reading a moved-from object is the assertion, not an accident.
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_TRUE(rvalue.empty()) << "an rvalue argument is moved from even on failure";

  typename TestFixture::MoveOnlyQueue mq(1);
  ASSERT_TRUE(mq.try_push(std::make_unique<int>(1)));
  auto owned = std::make_unique<int>(2);
  EXPECT_FALSE(mq.try_push(std::move(owned)));
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_EQ(owned, nullptr) << "retrying with the same object would push a husk";
}

// A throwing move assignment is the one place the three queues genuinely
// differ, so it gets its own suite. MutexQueue and SpscQueue survive it with
// their indices intact; MpmcQueue cannot — a throw strands a claimed ticket
// whose sequence is never re-published — which is why it static_asserts the
// requirement instead of appearing here. That static_assert is its test.
//
// Nothing else in the suite can reach this path: every other T here has a
// noexcept move assignment. The throw is armed by a counter rather than a flag
// on the element, so the element enqueues normally and turns hostile only for
// the dequeue.
int g_moves_until_throw = -1;        // negative: never throw
constexpr int kStolenMarker = -999;  // what a stolen-from value is left holding
constexpr int kSentinel = 99;        // pre-loaded into out; the failed pop overwrites it

// NOLINTBEGIN(misc-non-private-member-variables-in-classes) -- a two-field test
// payload; accessors would only obscure what the assertions check.
struct ThrowingMove {
  int value = 0;
  std::string label;

  ThrowingMove() = default;
  ThrowingMove(int v, std::string l) : value(v), label(std::move(l)) {}
  ThrowingMove(ThrowingMove&&) = default;
  ThrowingMove(const ThrowingMove&) = delete;
  ThrowingMove& operator=(const ThrowingMove&) = delete;

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

template <typename Family>
class ThrowingMoveContract : public ::testing::Test {
 protected:
  using Queue = typename Family::template Queue<ThrowingMove>;
};

using SurvivingFamilies = ::testing::Types<MutexFamily, SpscFamily>;
TYPED_TEST_SUITE(ThrowingMoveContract, SurvivingFamilies, FamilyNames);

TYPED_TEST(ThrowingMoveContract, ThrowingPopKeepsInvariantsButNotElementValues) {
  typename TestFixture::Queue q(4);
  ASSERT_TRUE(q.try_push({1, "one"}));
  ASSERT_TRUE(q.try_push({2, "two"}));

  ThrowingMove out{kSentinel, "sentinel"};
  g_moves_until_throw = 0;  // the next move assignment is the pop's
  EXPECT_THROW(static_cast<void>(q.try_pop(out)), std::runtime_error);
  g_moves_until_throw = -1;

  // Queue side: the indices never moved, so nothing was lost or duplicated.
  EXPECT_EQ(q.size(), 2U);

  // Element side: the headers promise nothing, and indeed both the destination
  // and the still-queued element were disturbed.
  EXPECT_EQ(out.value, 1) << "out was modified despite the failure";
  EXPECT_EQ(out.label, "sentinel") << "the throw landed between the two members";

  ThrowingMove retry;
  ASSERT_TRUE(q.try_pop(retry));
  EXPECT_EQ(retry.value, kStolenMarker) << "retrying yields a hollowed element";
  EXPECT_EQ(retry.label, "one");

  ThrowingMove neighbour;
  ASSERT_TRUE(q.try_pop(neighbour));
  EXPECT_EQ(neighbour.value, 2) << "the next element is untouched";
  EXPECT_EQ(q.size(), 0U) << "the count still reconciles, which is why a checksum misses this";
}

TYPED_TEST(ThrowingMoveContract, ThrowingPushEnqueuesNothing) {
  // The push side genuinely upholds its guarantee: the slot is written before
  // the index advances, so a throw leaves the queue empty.
  typename TestFixture::Queue q(2);
  g_moves_until_throw = 0;
  EXPECT_THROW(static_cast<void>(q.try_push({7, "seven"})), std::runtime_error);
  g_moves_until_throw = -1;
  EXPECT_EQ(q.size(), 0U);
}

}  // namespace
}  // namespace cq
