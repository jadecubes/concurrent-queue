// Throughput benchmarks for cq::MutexQueue (v1 baseline).
//
// Each benchmark uses Google Benchmark's multi-thread support: the first half
// of the threads produce, the second half consume, one queue op per benchmark
// iteration. Every thread runs the same iteration count, so pushes and pops
// balance and the harness keeps full control of run length. Reported items/s
// is total queue ops per second (a push and its pop count as two).
//
// Thread spawn and the start barrier sit inside the timed region, so a
// min_time well above that (~50ms) is required for meaningful numbers:
// Run: ./queue_bench --benchmark_min_time=1s --benchmark_repetitions=10

#include <cq/mutex_queue.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include <benchmark/benchmark.h>

namespace {

constexpr std::size_t kCapacity = 1024;

// Created/destroyed by the Setup/Teardown hooks below, which run once per
// repetition outside the threaded region.
std::unique_ptr<cq::MutexQueue<std::uint64_t>> shared_queue;

void setup_queue(const benchmark::State& /*state*/) {
  shared_queue = std::make_unique<cq::MutexQueue<std::uint64_t>>(kCapacity);
  // Half-full start: neither side begins blocked on a condition variable, so
  // the measurement starts in steady state.
  bool prefilled = true;
  for (std::uint64_t i = 0; i < kCapacity / 2; ++i) {
    prefilled = prefilled && shared_queue->try_push(i);
  }
  if (!prefilled) {
    std::abort();  // unreachable: fresh queue, stays below capacity
  }
}
void teardown_queue(const benchmark::State& /*state*/) { shared_queue.reset(); }

void BM_MutexQueueThroughput(benchmark::State& state) {
  const bool is_producer = state.thread_index() < state.threads() / 2;
  auto& queue = *shared_queue;

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

// SPSC: 1 producer + 1 consumer; MPMC: 4 + 4. Google Benchmark appends the
// /threads:N suffix to the reported name.
constexpr int kSpscThreads = 2;
constexpr int kMpmcThreads = 8;
static_assert(kSpscThreads % 2 == 0 && kMpmcThreads % 2 == 0,
              "producer/consumer pairing needs an even thread count");
BENCHMARK(BM_MutexQueueThroughput)
    ->Setup(setup_queue)
    ->Teardown(teardown_queue)
    ->Threads(kSpscThreads)
    ->Threads(kMpmcThreads)
    ->UseRealTime()
    ->Name("MutexQueue/throughput");

// Uncontended single-thread round trip: the queue's raw locked cost.
void BM_MutexQueuePushPopSingleThread(benchmark::State& state) {
  cq::MutexQueue<std::uint64_t> queue(kCapacity);
  std::uint64_t value = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(queue.push(1));
    benchmark::DoNotOptimize(queue.pop(value));
  }
  // A push and its pop are two queue ops — keep the unit identical to the
  // threaded benchmark so the rates compare directly.
  state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_MutexQueuePushPopSingleThread)->Name("MutexQueue/single_thread_roundtrip");

}  // namespace
