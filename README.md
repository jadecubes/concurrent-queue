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

## Layout

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
cmake --build build-rel && ./build-rel/bench/queue_bench --benchmark_repetitions=10

# lint (same file sets CI checks; CI pins version 18, so match it locally
# — a different major version formats differently and CI will disagree)
git ls-files '*.hpp' '*.ipp' '*.cpp' | xargs clang-format --dry-run --Werror
git ls-files '*.cpp' | xargs clang-tidy -p build-rel
```

## Correctness policy

- Every test runs under ThreadSanitizer.
- A checksum stress test (N producers push known values; consumers' totals
  must reconcile) gates every queue variant.
- Benchmark numbers are reported as mean ± stddev over repeated runs, with
  machine specs.

## Results

Machine: Apple M2 Pro (12 cores), 32 GB, macOS 26. Release build,
`--benchmark_repetitions=10` (each run is `MinTime` 1s, set on the benchmark).
The machine was not idle — load average ~4.6 — so treat these as a floor.

An **op** is one `push` or one `pop`, so transferring an item costs two ops;
this is the unit Google Benchmark prints as `items_per_second`.

Per-op figures below are `1 / throughput` — the aggregate cost of one op across
the whole queue, not per-thread latency. CV is the coefficient of variation of
the **throughput** column, matching the `±` beside it, not of Google Benchmark's
Time column.

Those two are not interchangeable, and the reason is this repo's own benchmark
configuration rather than a quirk of the tool: a rate counter is divided by
whichever clock the benchmark selected. The threaded registrations chain
`UseRealTime()`, so for them the throughput CV and the Time CV are the same
quantity by construction. The single-thread round trip does not, so its
throughput is normalised by CPU time while its Time column still reports real
time — around 2.8x more variable here. A row mixing the two columns reads as
self-contradictory, which is exactly what the first row did before this was
noticed.

The consequence worth stating plainly: the round-trip row is ops per
CPU-second, the threaded rows are ops per wall-second. On an otherwise idle
machine they differ by about 1%, so the ~3x gap below is real — but the rows
are not strictly the same unit, and `bench/queue_bench.cpp`'s claim that they
"compare directly" is true of the item count and not of the clock.

| Benchmark (v1 MutexQueue) | Throughput | Per op | CV |
|---|---|---|---|
| single-thread push+pop round trip | 105.1M ± 0.7M ops/s | 9.5 ns (19.0 ns per round trip) | 0.7% |
| SPSC (1 producer, 1 consumer) | 35.7M ± 0.5M ops/s | 28.0 ns | 1.4% |
| MPMC (4 producers, 4 consumers) | 21.6M ± 0.1M ops/s | 46.3 ns | 0.5% |

The v1 story in one line: one mutex serializes everything, so **threads never
buy throughput** — the uncontended round trip moves ops ~3× faster than two
threads managing it, and going from 2 threads to 8 loses a further ~40%
(1.65× slower). That is the baseline v2's lock-free ring has to beat.

Two caveats on reading these numbers. First, v1 notifies its condition
variables on every op, even when no thread is waiting: a variant that keeps
waiter counts and skips the no-op notify measures ~20% faster on MPMC and
~26–37% faster on SPSC (the uncontended round trip is unchanged). This
baseline is deliberately the *simple* single-mutex design, not the fastest
one — worth remembering before crediting v2 with the whole gap. Second, the
numbers above are ops/s; halve them for items transferred per second
(SPSC ≈ 17.9M items/s, MPMC ≈ 10.8M).

_v2 → v3 to follow._
