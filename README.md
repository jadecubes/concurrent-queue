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
  Each operation offers three waiting disciplines — wait indefinitely
  (`push` / `pop`), never wait (`try_push` / `try_pop`), or wait up to a
  caller-supplied bound (`try_push_for` / `try_pop_for`).
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
the whole queue, not per-thread latency.

| Benchmark (v1 MutexQueue) | Throughput | Per op | CV |
|---|---|---|---|
| single-thread push+pop round trip | 105.1M ± 0.7M ops/s | 9.5 ns (19.0 ns per round trip) | 2.2% |
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

### v3 — versus the industry (moodycamel, TBB)

Same machine and harness, one session (load average ~3.7), all three queues in
one binary (moodycamel `concurrentqueue` v1.0.4, oneTBB v2022.2.0); v1's own
numbers reproduced within 2% of the table above, which is what makes the
columns comparable. Contract differences that the numbers must
be read against: `tbb::concurrent_bounded_queue` matches v1's contract
(bounded, blocking, MPMC); `moodycamel::ConcurrentQueue` is **unbounded**
(producers never wait) and FIFO only **per producer** — it is answering an
easier question, and still gets to count the same ops.

| ops/s, mean ± stddev | v1 MutexQueue | tbb::concurrent_bounded_queue | moodycamel::ConcurrentQueue |
|---|---|---|---|
| single-thread round trip | 104.7M ± 1.4M | 114.3M ± 0.4M | 167.4M ± 2.2M |
| 2 threads (1p + 1c) | **35.4M ± 0.5M** | 22.9M ± 0.6M | 92.1M ± 2.9M |
| 8 threads (4p + 4c) | **21.3M ± 0.2M** | 16.5M ± 0.6M | 45.6M ± 1.8M |

Two findings, one expected and one not:

**moodycamel wins everything** — 2.6× v1 at 2 threads, 2.1× at 8, 1.6×
uncontended — and that is the honest gap to a mature lock-free design, *minus*
a discount the table cannot show: it never blocks a producer and never
promises global FIFO, so part of its lead is bought with a weaker contract,
not just better engineering. Its design (a sub-queue per producer, so
producers never contend with each other; consumers rotate across sub-queues)
is sharing-avoidance taken much further than the cached-peer-index
optimization in flight for our SPSC ring (#4).

**v1 beats TBB's bounded queue under contention** — 1.5× at 2 threads, 1.3×
at 8 — losing only the uncontended round trip (0.92×). A mechanistic reading
(from the shape of the numbers, not from profiling): a single short critical
section behind an uncontended-fast-path mutex costs one atomic handoff per
op, while TBB's bounded queue pays several atomic RMWs per op (ticket
dispensing plus slot state) to admit parallelism between operations — a price
that only pays off when there is parallelism to admit. With 64-bit items and
~28 ns critical sections there is none to find, so the simpler design wins.
The lesson v3 was meant to teach, in reverse: lock-free is not a synonym for
faster; it buys progress guarantees and scalability headroom, and both cost
per-op overhead.

As everywhere above, these are ops/s — halve for items/s.

_The SPSC rings (#3, #4) join this table when their PRs merge._
