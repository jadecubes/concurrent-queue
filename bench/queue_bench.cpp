// Throughput benchmarks: the cq queues vs moodycamel::ConcurrentQueue and
// tbb::concurrent_bounded_queue (roadmap v3), all on one templated harness.
//
// Adapter caveats, stated rather than papered over:
//   - tbb bounded: the close match (bounded, blocking, MPMC); its void
//     push/pop are adapted to return true.
//   - moodycamel: UNBOUNDED and FIFO only per producer — producers never
//     block, an advantage the writeup must discount. Its pop spins + yields.
//
// Shape: the first half of the threads push, the second half pop, one op per
// iteration, so pushes and pops balance. items/s = total ops/s (a push and
// its pop count as two). Spawn cost sits outside the timed region; add
// --benchmark_repetitions=10 for a spread.
//
// The /try_throughput sweeps use the non-blocking API instead, retrying a
// failed attempt inside the same iteration. Failures therefore cost time but
// never count as transferred work, so items/s stays the same unit as the
// blocking rows, and the pressure that caused them is reported separately as
// retries/op. Sweeping capacity is the point: it is where a lock-free ring's
// advantage over the mutex should narrow, since a shallow queue makes every
// op wait on its counterpart no matter how the waiting is implemented.

#include <cq/mpmc_queue.hpp>
#include <cq/mutex_queue.hpp>
#include <cq/spsc_queue.hpp>

#include "try_operation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <thread>

#include <benchmark/benchmark.h>
#include <concurrentqueue.h>
#include <tbb/concurrent_queue.h>

namespace {

constexpr std::size_t kCapacity = 1024;

// Adapters onto the harness's shape: bool push / try_push / pop.

class TbbBoundedQueue {
 public:
  explicit TbbBoundedQueue(std::size_t capacity) {
    queue_.set_capacity(static_cast<std::ptrdiff_t>(capacity));
  }

  bool push(std::uint64_t value) {
    queue_.push(value);  // blocks while full
    return true;
  }
  bool try_push(std::uint64_t value) { return queue_.try_push(value); }
  bool pop(std::uint64_t& out) {
    queue_.pop(out);  // blocks while empty
    return true;
  }

 private:
  tbb::concurrent_bounded_queue<std::uint64_t> queue_;
};

class MoodycamelQueue {
 public:
  // The "capacity" only pre-sizes the block pool; the queue stays unbounded.
  explicit MoodycamelQueue(std::size_t capacity) : queue_(capacity) {}

  bool push(std::uint64_t value) { return queue_.enqueue(value); }
  // Same as push: an unbounded queue has no "full" to refuse on.
  bool try_push(std::uint64_t value) { return queue_.enqueue(value); }
  bool pop(std::uint64_t& out) {
    while (!queue_.try_dequeue(out)) {
      std::this_thread::yield();
    }
    return true;
  }

 private:
  moodycamel::ConcurrentQueue<std::uint64_t> queue_;
};

// Created/destroyed by the Setup/Teardown hooks below, which run once per
// repetition outside the threaded region. One instance per queue type.
template <typename Queue>
std::unique_ptr<Queue> shared_queue;

template <typename Queue>
void setup_queue(const benchmark::State& /*state*/) {
  shared_queue<Queue> = std::make_unique<Queue>(kCapacity);
  // Half-full start: neither side begins blocked or spinning on the other, so
  // the measurement starts in steady state.
  bool prefilled = true;
  for (std::uint64_t i = 0; i < kCapacity / 2; ++i) {
    prefilled = prefilled && shared_queue<Queue>->try_push(i);
  }
  if (!prefilled) {
    std::abort();  // unreachable: fresh queue, stays below capacity
  }
}

template <typename Queue>
void teardown_queue(const benchmark::State& /*state*/) {
  shared_queue<Queue>.reset();
}

// Same as setup_queue, but takes the capacity from the registered Arg so one
// benchmark can sweep it.
template <typename Queue>
void setup_queue_at_capacity(const benchmark::State& state) {
  const auto capacity = static_cast<std::size_t>(state.range(0));
  shared_queue<Queue> = std::make_unique<Queue>(capacity);
  // Half-full where the half exists: at capacity 1 it is 0, so that sweep
  // point deliberately starts empty -- which is the retry pressure it is
  // there to measure.
  for (std::uint64_t i = 0; i < capacity / 2; ++i) {
    if (!shared_queue<Queue>->try_push(i)) {
      std::abort();  // unreachable: fresh queue, stays below capacity
    }
  }
}

template <typename Queue>
void BM_QueueThroughput(benchmark::State& state) {
  const bool is_producer = state.thread_index() < state.threads() / 2;
  auto& queue = *shared_queue<Queue>;

  if (is_producer) {
    std::uint64_t item = 0;
    for (auto _ : state) {
      benchmark::DoNotOptimize(queue.push(item++));
    }
  } else {
    std::uint64_t value = 0;
    for (auto _ : state) {
      benchmark::DoNotOptimize(queue.pop(value));
    }
  }
  // Benchmark accumulates real time as the sum over threads, so each thread
  // reports iterations * threads: the sum divided by (threads * wall) is then
  // the aggregate ops/s the file header promises.
  state.SetItemsProcessed(state.iterations() * state.threads());
}

// Non-blocking counterpart of BM_QueueThroughput.
template <typename Queue>
void BM_QueueTryThroughput(benchmark::State& state) {
  const bool is_producer = state.thread_index() < state.threads() / 2;
  auto& queue = *shared_queue<Queue>;
  std::size_t retries = 0;

  if (is_producer) {
    std::uint64_t item = 0;
    for (auto _ : state) {
      retries += cq::bench::count_failures_until_success([&] { return queue.try_push(item); });
      ++item;
    }
  } else {
    std::uint64_t value = 0;
    for (auto _ : state) {
      retries += cq::bench::count_failures_until_success([&] { return queue.try_pop(value); });
    }
  }

  state.SetItemsProcessed(state.iterations() * state.threads());
  // Counters sum across threads, and kAvgIterations divides by the iteration
  // total over *all* threads. Producers and consumers each own half of that
  // total, so doubling one side's retries before the divide yields that side's
  // retries per its own op -- for any even producer/consumer split, not just
  // the 1+1 these sweeps register.
  const auto side_retries = 2.0 * static_cast<double>(retries);
  state.counters.emplace(
      "push_retries/push",
      benchmark::Counter(is_producer ? side_retries : 0.0, benchmark::Counter::kAvgIterations));
  state.counters.emplace("pop_retries/pop", benchmark::Counter(is_producer ? 0.0 : side_retries,
                                                               benchmark::Counter::kAvgIterations));
  state.counters.emplace("retries/op", benchmark::Counter(static_cast<double>(retries),
                                                          benchmark::Counter::kAvgIterations));
}

// Uncontended single-thread round trip: the queue's raw per-op cost with no
// other thread in the picture.
template <typename Queue>
void BM_QueuePushPopSingleThread(benchmark::State& state) {
  Queue queue(kCapacity);
  std::uint64_t value = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(queue.push(1));
    benchmark::DoNotOptimize(queue.pop(value));
  }
  // A push and its pop are two queue ops — keep the unit identical to the
  // threaded benchmark so the rates compare directly.
  state.SetItemsProcessed(state.iterations() * 2);
}

using MpmcQueue = cq::MpmcQueue<std::uint64_t>;
using MutexQueue = cq::MutexQueue<std::uint64_t>;
using SpscQueue = cq::SpscQueue<std::uint64_t>;

// SPSC bulk transfer: kBulkBatch items per call, one index publish per batch.
// Each benchmark iteration moves one full batch on each side.
constexpr std::size_t kBulkBatch = 64;
void BM_SpscBulkThroughput(benchmark::State& state) {
  const bool is_producer = state.thread_index() == 0;
  auto& queue = *shared_queue<SpscQueue>;
  std::array<std::uint64_t, kBulkBatch> buf{};
  if (is_producer) {
    for (auto _ : state) {
      std::size_t done = 0;
      while (done < kBulkBatch) {
        done += queue.try_push_n(std::span{buf}.subspan(done));
      }
    }
  } else {
    for (auto _ : state) {
      std::size_t done = 0;
      while (done < kBulkBatch) {
        done += queue.try_pop_n(std::span{buf}.subspan(done));
      }
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kBulkBatch) *
                          state.threads());
}

// SPSC: 1 producer + 1 consumer; MPMC: 4 + 4. Google Benchmark appends the
// /threads:N suffix to the reported name. SpscQueue's contract allows one
// thread per side, so it registers only the 2-thread shape.
constexpr int kSpscThreads = 2;
constexpr int kMpmcThreads = 8;
static_assert(kSpscThreads % 2 == 0 && kMpmcThreads % 2 == 0,
              "producer/consumer pairing needs an even thread count");
// MinTime: contended runs need a second of samples to settle. Setting it here
// beats the flag's seconds form — ComputeMinTime prefers a non-zero registered
// min_time — so --benchmark_min_time=0.2s is accepted and ignored. Only the
// iteration form overrides, which is what the ctest smoke run uses (1x).
constexpr double kMinTimeSeconds = 1.0;

BENCHMARK(BM_QueueThroughput<MutexQueue>)
    ->Setup(setup_queue<MutexQueue>)
    ->Teardown(teardown_queue<MutexQueue>)
    ->Threads(kSpscThreads)
    ->Threads(kMpmcThreads)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("MutexQueue/throughput");

BENCHMARK(BM_QueueThroughput<SpscQueue>)
    ->Setup(setup_queue<SpscQueue>)
    ->Teardown(teardown_queue<SpscQueue>)
    ->Threads(kSpscThreads)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("SpscQueue/throughput");

BENCHMARK(BM_SpscBulkThroughput)
    ->Setup(setup_queue<SpscQueue>)
    ->Teardown(teardown_queue<SpscQueue>)
    ->Threads(kSpscThreads)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("SpscQueue/bulk64_throughput");

BENCHMARK(BM_QueueThroughput<MpmcQueue>)
    ->Setup(setup_queue<MpmcQueue>)
    ->Teardown(teardown_queue<MpmcQueue>)
    ->Threads(kSpscThreads)
    ->Threads(kMpmcThreads)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("MpmcQueue/throughput");

BENCHMARK(BM_QueueThroughput<TbbBoundedQueue>)
    ->Setup(setup_queue<TbbBoundedQueue>)
    ->Teardown(teardown_queue<TbbBoundedQueue>)
    ->Threads(kSpscThreads)
    ->Threads(kMpmcThreads)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("TbbBoundedQueue/throughput");

BENCHMARK(BM_QueueThroughput<MoodycamelQueue>)
    ->Setup(setup_queue<MoodycamelQueue>)
    ->Teardown(teardown_queue<MoodycamelQueue>)
    ->Threads(kSpscThreads)
    ->Threads(kMpmcThreads)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("MoodycamelQueue/throughput");

// Non-blocking sweeps. Capacity is the independent variable here, not a
// tuning constant, and every value is a power of two because MpmcQueue masks
// its indices. Only the cq queues are swept: moodycamel is unbounded, so it
// has no capacity to vary, and tbb's adapter has no try_pop yet.
// NOLINTBEGIN(readability-magic-numbers)
BENCHMARK(BM_QueueTryThroughput<MutexQueue>)
    ->Setup(setup_queue_at_capacity<MutexQueue>)
    ->Teardown(teardown_queue<MutexQueue>)
    ->ArgName("capacity")
    ->ArgsProduct({{1, 2, 8, 64, 1024}})
    ->Threads(kSpscThreads)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("MutexQueue/try_throughput");

BENCHMARK(BM_QueueTryThroughput<SpscQueue>)
    ->Setup(setup_queue_at_capacity<SpscQueue>)
    ->Teardown(teardown_queue<SpscQueue>)
    ->ArgName("capacity")
    ->ArgsProduct({{1, 2, 8, 64, 1024}})
    ->Threads(kSpscThreads)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("SpscQueue/try_throughput");

BENCHMARK(BM_QueueTryThroughput<MpmcQueue>)
    ->Setup(setup_queue_at_capacity<MpmcQueue>)
    ->Teardown(teardown_queue<MpmcQueue>)
    ->ArgName("capacity")
    ->ArgsProduct({{1, 2, 8, 64, 1024}})
    ->Threads(kSpscThreads)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("MpmcQueue/try_throughput");
// NOLINTEND(readability-magic-numbers)

// UseRealTime on the round trips too: Google Benchmark divides a rate counter
// by whichever clock the benchmark selected, so without it these rows report
// items/s per CPU-second while every threaded row above is per wall-second --
// not the same number, and not comparable, which is what the comment on
// SetItemsProcessed above claims they are.
BENCHMARK(BM_QueuePushPopSingleThread<MutexQueue>)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("MutexQueue/single_thread_roundtrip");

BENCHMARK(BM_QueuePushPopSingleThread<SpscQueue>)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("SpscQueue/single_thread_roundtrip");

BENCHMARK(BM_QueuePushPopSingleThread<MpmcQueue>)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("MpmcQueue/single_thread_roundtrip");

BENCHMARK(BM_QueuePushPopSingleThread<TbbBoundedQueue>)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("TbbBoundedQueue/single_thread_roundtrip");

BENCHMARK(BM_QueuePushPopSingleThread<MoodycamelQueue>)
    ->UseRealTime()
    ->MinTime(kMinTimeSeconds)
    ->Name("MoodycamelQueue/single_thread_roundtrip");

}  // namespace
