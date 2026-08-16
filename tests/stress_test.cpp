// Checksum stress test (see README correctness policy): N producers push known
// values, consumers' totals must reconcile. Run under ThreadSanitizer.

#include <cq/mutex_queue.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

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

  std::vector<std::jthread> producers;
  producers.reserve(kProducers);
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&q, p] {
      for (int i = 0; i < kItemsPerProducer; ++i) {
        const auto value =
            (static_cast<std::uint64_t>(p) * kItemsPerProducer) + static_cast<std::uint64_t>(i) + 1;
        ASSERT_TRUE(q.push(value));
      }
    });
  }

  std::atomic<std::uint64_t> consumed_sum{0};
  std::atomic<std::uint64_t> consumed_count{0};
  std::vector<std::jthread> consumers;
  consumers.reserve(kConsumers);
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
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
  }

  for (auto& t : producers) {
    t.join();
  }
  q.close();
  for (auto& t : consumers) {
    t.join();
  }

  // Sum of 1..kTotalItems: each producer p pushes the contiguous block
  // [p*kItems+1, (p+1)*kItems].
  const std::uint64_t expected_sum = kTotalItems * (kTotalItems + 1) / 2;
  EXPECT_EQ(consumed_count.load(), kTotalItems);
  EXPECT_EQ(consumed_sum.load(), expected_sum);
  EXPECT_EQ(q.size(), 0U);
}

}  // namespace
}  // namespace cq
