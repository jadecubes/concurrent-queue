// Checksum stress test (see README correctness policy): N producers push known
// values, consumers' totals must reconcile. Run under ThreadSanitizer.

#include <cq/mutex_queue.hpp>

#include <atomic>
#include <cstdint>

#include <gtest/gtest.h>

#include "queue_test_util.hpp"

namespace cq {
namespace {

TEST(MutexQueueStress, ChecksumReconcilesAcrossProducersAndConsumers) {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kItemsPerProducer = 25'000;
  constexpr std::uint64_t kTotalItems = static_cast<std::uint64_t>(kProducers) * kItemsPerProducer;
  // Much smaller than the item count so the ring wraps and fills constantly.
  constexpr std::size_t kQueueCapacity = 64;

  MutexQueue<std::uint64_t> q(kQueueCapacity);

  auto producers = spawn_threads(kProducers, [&q](int p) {
    // One assertion per producer, not per item: each ASSERT expands to a full
    // AssertionResult, which is measurable 25k times per thread under TSan.
    bool all_pushed = true;
    for (int i = 0; all_pushed && i < kItemsPerProducer; ++i) {
      const auto value =
          (static_cast<std::uint64_t>(p) * kItemsPerProducer) + static_cast<std::uint64_t>(i) + 1;
      all_pushed = q.push(value);
    }
    EXPECT_TRUE(all_pushed);
  });

  std::atomic<std::uint64_t> consumed_sum{0};
  std::atomic<std::uint64_t> consumed_count{0};
  auto consumers = spawn_threads(kConsumers, [&](int /*c*/) {
    std::uint64_t local_sum = 0;
    std::uint64_t local_count = 0;
    std::uint64_t value = 0;
    while (q.pop(value)) {
      local_sum += value;
      ++local_count;
    }
    consumed_sum.fetch_add(local_sum, std::memory_order_relaxed);
    consumed_count.fetch_add(local_count, std::memory_order_relaxed);
  });

  join_all(producers);
  q.close();  // all items in; wake the consumers so they drain and exit
  join_all(consumers);

  // Sum of 1..kTotalItems: each producer p pushes the contiguous block
  // [p*kItems+1, (p+1)*kItems].
  const std::uint64_t expected_sum = kTotalItems * (kTotalItems + 1) / 2;
  EXPECT_EQ(consumed_count.load(), kTotalItems);
  EXPECT_EQ(consumed_sum.load(), expected_sum);
  EXPECT_EQ(q.size(), 0U);
}

}  // namespace
}  // namespace cq
