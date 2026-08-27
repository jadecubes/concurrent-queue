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
`--benchmark_repetitions=20` (each run is `MinTime` 1s, set on the benchmark).
The machine was not idle — load average ~3.4 — so treat these as a floor.
Re-measured as one coherent run after the single-thread registration gained
`UseRealTime()`; before that its throughput was normalised by CPU time while
the threaded rows used wall time, so the rows were not the same unit.

An **op** is one `push` or one `pop`, so transferring an item costs two ops;
this is the unit Google Benchmark prints as `items_per_second`.

Per-op figures below are `1 / throughput` — the aggregate cost of one op across
the whole queue, not per-thread latency. CV is the coefficient of variation of
the **throughput** column, matching the `±` beside it. Every registration now
sets `UseRealTime()`, which makes that the same quantity as the CV Google
Benchmark prints for its Time column; a rate counter is divided by whichever
clock the benchmark selected, so mixing the two columns in one row is what made
an earlier version of this table read as self-contradictory.

| Benchmark (v1 MutexQueue) | Throughput | Per op | CV |
|---|---|---|---|
| single-thread push+pop round trip | 102.6M ± 1.4M ops/s | 9.7 ns (19.5 ns per round trip) | 1.4% |
| SPSC (1 producer, 1 consumer) | 35.1M ± 1.2M ops/s | 28.5 ns | 3.4% |
| MPMC (4 producers, 4 consumers) | 20.4M ± 1.4M ops/s | 49.1 ns | 6.9% |

The v1 story in one line: one mutex serializes everything, so **threads never
buy throughput** — the uncontended round trip moves ops ~2.9× faster than two
threads managing it, and going from 2 threads to 8 loses a further ~42%
(1.7× slower). That is the baseline v2's lock-free ring has to beat.

The CV column is noticeably worse than the run this table previously carried,
and that is the machine rather than the queue: contended rows are the ones a
busy box perturbs most, which is why the MPMC row is the least precise here.
The ordering the story rests on survives it — the three means are separated by
far more than their spreads.

Two caveats on reading these numbers. First, v1 notifies its condition
variables on every op, even when no thread is waiting: a variant that keeps
waiter counts and skips the no-op notify measures ~20% faster on MPMC and
~26–37% faster on SPSC (the uncontended round trip is unchanged). This
baseline is deliberately the *simple* single-mutex design, not the fastest
one — worth remembering before crediting v2 with the whole gap. Second, the
numbers above are ops/s; halve them for items transferred per second
(SPSC ≈ 17.6M items/s, MPMC ≈ 10.2M).

_v2 → v3 to follow._
