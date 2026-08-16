// Throughput benchmarks for cq::MutexQueue (v1 baseline).
//
// Each benchmark uses Google Benchmark's multi-thread support: the first half
// of the threads produce, the second half consume, one queue op per benchmark
// iteration. Every thread runs the same iteration count, so pushes and pops
// balance and the harness keeps full control of run length. Reported items/s
// is the end-to-end transfer rate through the queue.
//
// Run: ./queue_bench --benchmark_repetitions=10

#include <cq/mutex_queue.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

#include <benchmark/benchmark.h>

namespace {

constexpr std::size_t kCapacity = 1024;

// Created/destroyed by the Setup/Teardown hooks below, which run once per
// repetition outside the threaded region.
std::unique_ptr<cq::MutexQueue<std::uint64_t>> shared_queue;

void setup_queue(const benchmark::State& /*state*/) {
  shared_queue = std::make_unique<cq::MutexQueue<std::uint64_t>>(kCapacity);
}
void teardown_queue(const benchmark::State& /*state*/) { shared_queue.reset(); }

void BM_MutexQueueThroughput(benchmark::State& state) {
  if (state.threads() % 2 != 0) {
    state.SkipWithError("thread count must be even (producer/consumer pairs)");
    return;
  }
  const bool is_producer = state.thread_index() < state.threads() / 2;
  auto& queue = *shared_queue;

  if (is_producer) {
    std::uint64_t item = 0;
    for (auto _ : state) {
      benchmark::DoNotOptimize(queue.push(item++));
    }
    // Count the producer side only: Google Benchmark sums the counter across
    // threads, and each item passes through one producer and one consumer.
    state.SetItemsProcessed(state.iterations());
  } else {
    std::uint64_t value = 0;
    for (auto _ : state) {
      benchmark::DoNotOptimize(queue.pop(value));
    }
  }
}

// SPSC: 1 producer + 1 consumer; MPMC: 4 + 4. Google Benchmark appends the
// /threads:N suffix to the reported name.
constexpr int kSpscThreads = 2;
constexpr int kMpmcThreads = 8;
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
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MutexQueuePushPopSingleThread)->Name("MutexQueue/single_thread_roundtrip");

}  // namespace
