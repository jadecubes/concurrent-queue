# concurrent-queue

Learning project: build concurrent queues in C++20 from a locked baseline up to
lock-free, benchmark each step honestly, and compare the result against
industrial implementations.

> The main deliverable of this repo is the **benchmark writeup** (numbers,
> graphs, and the reasons behind them) — the code exists to produce it.

## Roadmap

- **v1 — mutex + condition_variable queue.** Bounded ring storage guarded by a
  `std::mutex`, with `not_full` / `not_empty` condition variables and
  `close()` shutdown semantics. The correctness and performance baseline.
- **v2 — lock-free SPSC ring buffer with atomics.** Same ring storage, no
  lock. Measure the difference against v1 with Google Benchmark.
- **v2.5 — bounded MPMC queue** (Vyukov-style, per-slot sequence counters).
- **v3 — compare against moodycamel and TBB.** Run the same benchmarks against
  `moodycamel::ConcurrentQueue` and `tbb::concurrent_bounded_queue`, then
  write up why mine loses (or wins).
- **Stretch — a thread pool** on top of the MPMC queue.

## Planned layout

```
include/cq/     header-only queue implementations
tests/          GoogleTest unit + stress tests (run under ThreadSanitizer)
bench/          Google Benchmark throughput / latency benchmarks
```

## Build & test

```sh
# tests (ThreadSanitizer on)
cmake -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure

# benchmarks (Release, no sanitizers)
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel && ./build-rel/queue_bench --benchmark_repetitions=10
```

## Correctness policy

- Every test runs under ThreadSanitizer.
- A checksum stress test (N producers push known values; consumers' totals
  must reconcile) gates every queue variant.
- Benchmark numbers are reported as mean ± stddev over repeated runs, with
  machine specs.

## Results

_To be filled in as v1 → v3 land._
