// Checksum stress test (see README correctness policy): N producers push known
// values, consumers' totals must reconcile. Run under ThreadSanitizer.

#include <cq/mutex_queue.hpp>
#include <cq/spsc_queue.hpp>

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

  auto producers = test_util::spawn_threads(kProducers, [&q](int p) {
    // Assert only on failure: a per-item ASSERT is measurable under TSan.
    for (int i = 0; i < kItemsPerProducer; ++i) {
      const auto value =
          (static_cast<std::uint64_t>(p) * kItemsPerProducer) + static_cast<std::uint64_t>(i) + 1;
      if (!q.push(value)) {
        ADD_FAILURE() << "producer " << p << " push failed at item " << i;
        break;
      }
    }
  });

  std::atomic<std::uint64_t> consumed_sum{0};
  std::atomic<std::uint64_t> consumed_count{0};
  auto consumers = test_util::spawn_threads(kConsumers, [&](int /*c*/) {
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

  producers.clear();  // joins every producer: all items are in
  q.close();          // wake the consumers so they drain and exit
  consumers.clear();  // joins every consumer: all items are out

  // Sum of 1..kTotalItems: each producer p pushes the contiguous block
  // [p*kItems+1, (p+1)*kItems].
  const std::uint64_t expected_sum = kTotalItems * (kTotalItems + 1) / 2;
  EXPECT_EQ(consumed_count.load(), kTotalItems);
  EXPECT_EQ(consumed_sum.load(), expected_sum);
  EXPECT_EQ(q.size(), 0U);
}

TEST(SpscQueueStress, ChecksumReconcilesAcrossProducerAndConsumer) {
  constexpr int kItems = 100'000;
  constexpr std::uint64_t kTotalItems = kItems;
  // Much smaller than the item count so the ring wraps and fills constantly.
  constexpr std::size_t kQueueCapacity = 64;

  SpscQueue<std::uint64_t> q(kQueueCapacity);

  // SPSC contract: exactly one producer thread and one consumer thread.
  auto producer = test_util::spawn_threads(1, [&q](int /*p*/) {
    for (std::uint64_t i = 1; i <= kTotalItems; ++i) {
      if (!q.push(i)) {
        ADD_FAILURE() << "push failed at item " << i;
        break;
      }
    }
  });

  std::uint64_t consumed_sum = 0;
  std::uint64_t consumed_count = 0;
  auto consumer = test_util::spawn_threads(1, [&](int /*c*/) {
    std::uint64_t value = 0;
    while (q.pop(value)) {
      consumed_sum += value;
      ++consumed_count;
    }
  });

  producer.clear();  // joins the producer: all items are in
  q.close();         // let the consumer drain and exit
  consumer.clear();  // joins the consumer: all items are out

  const std::uint64_t expected_sum = kTotalItems * (kTotalItems + 1) / 2;
  EXPECT_EQ(consumed_count, kTotalItems);
  EXPECT_EQ(consumed_sum, expected_sum);
  EXPECT_EQ(q.size(), 0U);
}

}  // namespace
}  // namespace cq
