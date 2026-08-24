// Throughput benchmarks for the cq queues (v1 MutexQueue, the v2.1 SpscQueue,
// and the v2.5 MpmcQueue) against two industrial queues (roadmap v3):
// moodycamel::ConcurrentQueue and tbb::concurrent_bounded_queue. One templated
// harness runs every queue so the numbers compare directly.
//
// The contenders do not share our exact contract, so each gets a thin adapter
// and the differences are stated here rather than papered over:
//   - tbb::concurrent_bounded_queue is the close match: bounded, blocking
//     push/pop, MPMC. Its push/pop return void; the adapter returns true.
//   - moodycamel::ConcurrentQueue is UNBOUNDED and non-blocking, and is FIFO
//     only per producer, not across producers. Producers therefore never
//     block (an advantage the writeup must discount), and the adapter's pop
//     spins on try_dequeue with a yield, the usual shape for a non-blocking
//     consumer.
//
// Each benchmark uses Google Benchmark's multi-thread support: the first half
// of the threads produce, the second half consume, one queue op per benchmark
// iteration. Every thread runs the same iteration count, so pushes and pops
// balance and the harness keeps full control of run length. Reported items/s
// is total queue ops per second (a push and its pop count as two).
//
// Threads synchronize on Google Benchmark's start barrier before timing
// resumes, so spawn cost is outside the timed region. The registrations below
// set MinTime so a bare run is already long enough to be meaningful; add
// repetitions for a spread: ./queue_bench --benchmark_repetitions=10

#include <cq/mpmc_queue.hpp>
#include <cq/mutex_queue.hpp>
#include <cq/spsc_queue.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <thread>

#include <benchmark/benchmark.h>
#include <concurrentqueue.h>
#include <tbb/concurrent_queue.h>

namespace {

constexpr std::size_t kCapacity = 1024;

// Adapters onto the harness's shape: bool push(T), bool try_push(T),
// bool pop(T&).

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

BENCHMARK(BM_QueuePushPopSingleThread<MutexQueue>)
    ->MinTime(kMinTimeSeconds)
    ->Name("MutexQueue/single_thread_roundtrip");

BENCHMARK(BM_QueuePushPopSingleThread<SpscQueue>)
    ->MinTime(kMinTimeSeconds)
    ->Name("SpscQueue/single_thread_roundtrip");

BENCHMARK(BM_QueuePushPopSingleThread<MpmcQueue>)
    ->MinTime(kMinTimeSeconds)
    ->Name("MpmcQueue/single_thread_roundtrip");

BENCHMARK(BM_QueuePushPopSingleThread<TbbBoundedQueue>)
    ->MinTime(kMinTimeSeconds)
    ->Name("TbbBoundedQueue/single_thread_roundtrip");

BENCHMARK(BM_QueuePushPopSingleThread<MoodycamelQueue>)
    ->MinTime(kMinTimeSeconds)
    ->Name("MoodycamelQueue/single_thread_roundtrip");

}  // namespace
