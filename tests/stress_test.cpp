// Checksum stress test (see README correctness policy): N producers push known
// values, consumers' totals must reconcile. Run under ThreadSanitizer.

#include <cq/mutex_queue.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

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

// The checksum test above transfers std::uint64_t, so it only ever proves the
// queue's own state is consistent. It cannot prove that memory the producer
// wrote *before* pushing is visible to the consumer after popping -- and a
// scalar checksum reconciles either way, as an unsynchronised prototype
// demonstrated: TSan reported the race while the sum came out correct.
//
// This is the transitive half of the mutex's guarantee: the consumer never
// synchronises with the producer's heap writes directly, only with mutex_.
// Those writes are sequenced before the producer's unlock, which synchronises
// with the consumer's lock, which is sequenced before the read. Run under
// ThreadSanitizer, which checks that edge rather than the resulting value.
TEST(MutexQueueStress, PayloadWrittenBeforePushIsVisibleAfterPop) {
  constexpr int kProducers = 2;
  constexpr int kConsumers = 2;
  constexpr int kItemsPerProducer = 5'000;
  constexpr std::size_t kBodyLength = 64;
  constexpr std::size_t kQueueCapacity = 16;  // small enough to wrap constantly

  struct Payload {
    std::uint64_t seed = 0;
    std::array<std::uint64_t, kBodyLength> body{};
  };

  MutexQueue<std::unique_ptr<Payload>> q(kQueueCapacity);

  auto producers = test_util::spawn_threads(kProducers, [&q](int p) {
    for (int i = 0; i < kItemsPerProducer; ++i) {
      auto payload = std::make_unique<Payload>();
      payload->seed =
          (static_cast<std::uint64_t>(p) * kItemsPerProducer) + static_cast<std::uint64_t>(i);
      for (std::size_t k = 0; k < kBodyLength; ++k) {
        payload->body[k] = payload->seed + k;  // heap writes, before the push
      }
      if (!q.push(std::move(payload))) {
        ADD_FAILURE() << "producer " << p << " push failed at item " << i;
        return;
      }
    }
  });

  std::atomic<std::uint64_t> verified{0};
  std::atomic<std::uint64_t> torn{0};
  auto consumers = test_util::spawn_threads(kConsumers, [&](int /*c*/) {
    std::unique_ptr<Payload> payload;
    std::uint64_t local_verified = 0;
    std::uint64_t local_torn = 0;
    while (q.pop(payload)) {
      for (std::size_t k = 0; k < kBodyLength; ++k) {
        if (payload->body[k] != payload->seed + k) {
          ++local_torn;
          break;
        }
      }
      ++local_verified;
    }
    verified.fetch_add(local_verified, std::memory_order_relaxed);
    torn.fetch_add(local_torn, std::memory_order_relaxed);
  });

  producers.clear();  // joins every producer
  q.close();          // wake the consumers so they drain and exit
  consumers.clear();  // joins every consumer

  EXPECT_EQ(verified.load(), static_cast<std::uint64_t>(kProducers) * kItemsPerProducer);
  EXPECT_EQ(torn.load(), 0U);
  EXPECT_EQ(q.size(), 0U);
}

}  // namespace
}  // namespace cq
