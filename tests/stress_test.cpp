// Checksum stress gate (README correctness policy): producers push known
// values, consumers' totals must reconcile. Run under ThreadSanitizer.

#include <cq/mpmc_queue.hpp>
#include <cq/mutex_queue.hpp>
#include <cq/spsc_queue.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "queue_test_util.hpp"

namespace cq {
namespace {

// One gate run's thread shape; a struct so counts can't be swapped.
struct GateShape {
  int producers;
  int consumers;
  int items_per_producer;
  std::size_t capacity;
};

// Producer p pushes [p*items + 1, (p+1)*items]: the grand total is 1..N
// with a closed-form sum. A small capacity keeps the ring wrapping.
template <typename Queue>
void run_checksum_gate(GateShape shape) {
  const auto items_per_producer = shape.items_per_producer;
  const auto total_items =
      static_cast<std::uint64_t>(shape.producers) * static_cast<std::uint64_t>(items_per_producer);

  Queue q(shape.capacity);

  auto producers = test_util::spawn_threads(shape.producers, [&](int p) {
    // Assert only on failure: a per-item ASSERT is measurable under TSan.
    for (int i = 0; i < items_per_producer; ++i) {
      const auto value =
          (static_cast<std::uint64_t>(p) * static_cast<std::uint64_t>(items_per_producer)) +
          static_cast<std::uint64_t>(i) + 1;
      if (!q.push(value)) {
        ADD_FAILURE() << "producer " << p << " push failed at item " << i;
        break;
      }
    }
  });

  std::atomic<std::uint64_t> consumed_sum{0};
  std::atomic<std::uint64_t> consumed_count{0};
  auto consumers = test_util::spawn_threads(shape.consumers, [&](int /*c*/) {
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

  EXPECT_EQ(consumed_count.load(), total_items);
  EXPECT_EQ(consumed_sum.load(), total_items * (total_items + 1) / 2);
  EXPECT_EQ(q.size(), 0U);
}

TEST(MutexQueueStress, ChecksumReconcilesAcrossProducersAndConsumers) {
  constexpr auto kShape =
      GateShape{.producers = 4, .consumers = 4, .items_per_producer = 25'000, .capacity = 64};
  run_checksum_gate<MutexQueue<std::uint64_t>>(kShape);
}

// SPSC contract: one producer, one consumer.
TEST(SpscQueueStress, ChecksumReconcilesAcrossProducerAndConsumer) {
  constexpr auto kShape =
      GateShape{.producers = 1, .consumers = 1, .items_per_producer = 100'000, .capacity = 64};
  run_checksum_gate<SpscQueue<std::uint64_t>>(kShape);
}

// One slot: every item is a full handoff, every cache refresh forced.
TEST(SpscQueueStress, SingleSlotRingHandsOffEveryItem) {
  constexpr auto kShape =
      GateShape{.producers = 1, .consumers = 1, .items_per_producer = 20'000, .capacity = 1};
  run_checksum_gate<SpscQueue<std::uint64_t>>(kShape);
}

TEST(MpmcQueueStress, ChecksumReconcilesAcrossProducersAndConsumers) {
  constexpr auto kShape =
      GateShape{.producers = 4, .consumers = 4, .items_per_producer = 25'000, .capacity = 64};
  run_checksum_gate<MpmcQueue<std::uint64_t>>(kShape);
}

// One slot: every pair races for the same cell's sequence counter.
TEST(MpmcQueueStress, SingleSlotRingHandsOffEveryItem) {
  constexpr auto kShape =
      GateShape{.producers = 2, .consumers = 2, .items_per_producer = 5'000, .capacity = 1};
  run_checksum_gate<MpmcQueue<std::uint64_t>>(kShape);
}

}  // namespace
}  // namespace cq
