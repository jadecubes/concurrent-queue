// The family contract, written once: every queue in cq shares the same
// bounded-FIFO close/drain interface by design, so the tests that express
// that contract run as typed tests over all of them. Queue-specific behavior
// (timed variants, index-cache paths, overflow guards) stays in the
// per-queue test files.

#include <cq/mpmc_queue.hpp>
#include <cq/mutex_queue.hpp>
#include <cq/spsc_queue.hpp>

#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "queue_test_util.hpp"

namespace cq {
namespace {

// Typed tests parametrize over one type, but a queue is a template — so the
// parameter is a family tag carrying the template as an alias.
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

// The fixture spells out the dependent-template names once, so the tests can
// say TestFixture::IntQueue instead of TypeParam::template Queue<int>.
template <typename Family>
class QueueContract : public ::testing::Test {
 protected:
  using IntQueue = typename Family::template Queue<int>;
  using MoveOnlyQueue = typename Family::template Queue<std::unique_ptr<int>>;
};

struct FamilyNames {
  // GoogleTest's name-generator API requires exactly this PascalCase name.
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

// Capacity 3 on purpose: a non-power-of-two, not-a-slot-count-plus-one size
// catches off-by-one full detection in every ring layout (the SPSC ring's
// spare slot, the MPMC ring's modular indexing).
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

}  // namespace
}  // namespace cq
