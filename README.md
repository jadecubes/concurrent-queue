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
  *(v2.1 added a cached peer index on each side — see Results.)*
- **v2.5 — bounded MPMC queue** (Vyukov-style, per-slot sequence counters).
- **v3 — compare against moodycamel and TBB.** Run the same benchmarks against
  `moodycamel::ConcurrentQueue` and `tbb::concurrent_bounded_queue`, then
  write up why mine loses (or wins).
- **Stretch — a thread pool** on top of the MPMC queue. *(Open.)*

Everything above the stretch goal is implemented; the Results section below
carries each step's measurements.

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

### v2 — SpscQueue (lock-free SPSC ring)

Same machine and harness, measured in one session with v1 re-run alongside
(load average ~4.1; v1's SPSC figure reproduced at 35.8M ± 0.6M ops/s, within
noise of the table above — the ratios below use the same-session numbers). An
earlier session on a busier machine recorded 252M ± 55M for the SPSC pair with
a 21.9% CV; the spread was the tell, and the re-measured figures below replace
it.

| Benchmark (v2 SpscQueue) | Throughput | Per op | CV |
|---|---|---|---|
| single-thread push+pop round trip | 2.00G ± 0.01G ops/s | 0.50 ns (1.00 ns per round trip) | 0.7% |
| SPSC (1 producer, 1 consumer) | 369M ± 14M ops/s | 2.7 ns | 3.9% |

The v2 story: dropping the mutex buys **~10× on the SPSC pair** (369M vs
35.8M ops/s) and **~19× on the uncontended round trip**, where an op is a
handful of instructions with no atomic read-modify-write — each side loads the
other side's index and release-stores its own. The new cost center is the
cache coherence traffic itself: the same ring that moves 2.00G ops/s on one
core drops to 369M when producer and consumer sit on different cores and the
`head_`/`tail_` lines ping-pong between them. Caching the last-seen peer index
to skip most of those loads is the classic next step, left for a v2.x once the
unoptimized gap is on record.

### v2.1 — cached peer indices

(Numbered v2.1, not v2.5: the roadmap reserves v2.5 for the Vyukov-style
MPMC queue, and this is the "v2.x" follow-up the section above named.)

Same machine and harness. v2 was rebuilt from its own commit and run
alternately with v2.1 in one session (load average ~4.9), so both columns come
from the same conditions, and v1 ran inside both binaries as a control —
reproducing within 2% (35.4M vs 36.3M ops/s on SPSC), which is what makes the
comparison below worth reading.

The change: each side keeps a private, non-atomic copy of the last value it
read from the opposite index and consults that before touching the real one.
Because both indices only ever advance, a cached value that says "not full" /
"not empty" cannot be wrong — so a steady stream never reads the peer's cache
line at all, and only a ring that has actually run empty or full pays to
refresh.

| Benchmark | v2 | v2.1 | change |
|---|---|---|---|
| SPSC (1 producer, 1 consumer) | 361.7M ± 10.9M ops/s (5.53 ns, CV 3.0%) | **613.7M ± 6.3M ops/s** (3.26 ns, CV 1.0%) | **1.70× faster** |
| single-thread push+pop round trip | 2.006G ± 0.005G ops/s (0.998 ns, CV 0.2%) | 1.555G ± 0.005G ops/s (1.29 ns, CV 0.3%) | **0.77× — 23% slower** |

The optimization does what it was meant to on the benchmark that reflects the
queue's purpose, and it costs something real on the one that does not. That
regression is not incidental. The round-trip benchmark pushes one item and
immediately pops it, so the ring sits pinned at empty and *every* pop finds
its cache stale: it pays the extra compare and the write-back on every single
operation and never once gets to skip a peer read. That is precisely the
cache's worst case. A ring with slack — the SPSC benchmark, and any real
stream — skips nearly all of them. Worth stating plainly rather than quoting
only the number that flatters the change.

The v2 column here (361.7M ops/s) independently agrees with the re-measured
figure in the v2 table above (369M ± 14M) — both sessions replaced an early
noisy sample (252M, CV 21.9%) taken on a busier machine. Against the
same-session v1 baseline, **v2 is ~10× v1** and **v2.1 is ~17×** (613.7M vs
35.4M ops/s).

Where the remaining time goes: at 3.26 ns per op the pair is dominated by the
handoff itself — the producer's release store to `tail_` still has to reach the
consumer's core before the consumer can advance, and no amount of caching
removes that dependency. Publishing an index once per batch of N items is what
buys the next factor, and it trades latency to get it.

### v2.5 — MpmcQueue (Vyukov bounded MPMC)

Same machine and harness, one session containing v1, v2.1, and v2.5 (load
average ~5–6); the controls reproduced their tables above within noise (v1
SPSC 36.2M vs 36.3M, v2.1 SPSC 621.6M vs 613.7M ops/s), which is what makes
the columns comparable.

The design is Dmitry Vyukov's bounded MPMC queue: each slot carries its own
sequence counter and the two position counters only hand out tickets, so
producers synchronize with consumers per slot rather than through one shared
index pair. Two deviations from the canonical version, both for contract
parity with the other queues: capacity is arbitrary (indexing by modulo, not
a power-of-two mask), and the sequence encoding is doubled — free = 2·ticket,
full = 2·ticket + 1 — because the classic encoding collides at capacity 1,
where "holds ticket t's data" and "free for ticket t+1" are the same number.

| Benchmark (v2.5 MpmcQueue) | Throughput | vs v1 same shape | CV |
|---|---|---|---|
| single-thread push+pop round trip | 286M ± 1M ops/s | 2.7× | 0.3% |
| 2 threads (1 producer, 1 consumer) | 224M ± 4M ops/s | 6.2× | 1.6% |
| 8 threads (4 producers, 4 consumers) | **13.3M ± 0.6M ops/s** | **0.62× — slower than the mutex** | 4.3% |

The result worth stating plainly: on the MPMC shape this queue exists for, it
**loses to the v1 mutex baseline** (13.3M vs 21.6M ops/s), while winning the
shapes with little or no contention. A mechanistic reading (from the shape of
the numbers, not from profiling): every operation is an atomic RMW on one of
two position counters that all eight threads hammer, plus a slot-sequence
handoff — under full contention the CAS retry traffic thrashes exactly the
cache lines the SPSC ring so carefully avoided, whereas the mutex serializes
politely through one futex and the losers sleep instead of retrying. This
matches the v3 finding on the same machine that `tbb::concurrent_bounded_queue`
(also ticket-based) loses to the mutex too, and it is why moodycamel gives
each producer its own sub-queue instead of one shared ring. Lock-free buys
progress guarantees, not throughput.

### v3 — versus the industry (moodycamel, TBB)

The roadmap's destination: the whole family against two industrial queues, in
one binary and one session (moodycamel `concurrentqueue` v1.0.4, oneTBB
v2022.2.0; load average ~4). Every control reproduced its own table above
within noise, which is what makes the columns comparable. Contract differences
the numbers must be read against: `tbb::concurrent_bounded_queue` matches the
family contract (bounded, blocking, MPMC); `moodycamel::ConcurrentQueue` is
**unbounded** (producers never wait) and FIFO only **per producer** — it is
answering an easier question, and still gets to count the same ops.

| ops/s, mean ± stddev | v1 mutex | v2.1 SPSC | v2.5 MPMC | tbb bounded | moodycamel |
|---|---|---|---|---|---|
| single-thread round trip | 104.5M ± 0.2M | 1.54G ± 0.01G | 277.7M ± 0.8M | 113.3M ± 0.4M | 165.8M ± 1.9M |
| 2 threads (1p + 1c) | 34.2M ± 0.4M | **602M ± 11M** | 216.2M ± 4.7M | 22.6M ± 2.0M | 78.9M ± 2.6M |
| 8 threads (4p + 4c) | **21.0M ± 0.2M** | — | 13.6M ± 0.7M | 16.5M ± 0.5M | **43.5M ± 2.9M** |

Three findings:

**On the MPMC shape, the ranking is moodycamel > mutex > TBB > our Vyukov.**
Written plainly: the queue this repo built for the MPMC job finishes last on
it, and the two ticket-based designs (ours and TBB's) both lose to a plain
mutex. The mechanism (inferred from the numbers, not profiled): every
ticket-based operation is an atomic RMW on a counter all eight threads hammer,
and under full contention the CAS retry traffic costs more than the mutex's
polite futex queue, where losers sleep instead of re-fetching cache lines.
moodycamel escapes the same trap structurally — a sub-queue per producer means
producers never contend with each other — but pays for it with the weaker
per-producer-FIFO contract.

**With little or no contention, the specialized designs win big.** Our SPSC
ring at 1p+1c (602M ops/s) is 7.6× moodycamel and 27× TBB; even our Vyukov
queue at 1p+1c (216M) is 2.7× moodycamel and 9.6× TBB — shared-counter designs
fly when only two threads share the counters. Specialization to the actual
concurrency shape buys more than any amount of generic cleverness.

**Nobody beats the mutex at 4p+4c except moodycamel** — and it changes the
contract to do so. That is the honest summary of what lock-free bought this
repo: enormous wins where contention is structurally limited (SPSC), progress
guarantees everywhere, and a throughput *loss* under real MPMC contention.
"Why mine loses": ours keeps global FIFO and bounded semantics on one shared
ring; the winner gave those up.

As everywhere above, these are ops/s — halve for items/s.

_Roadmap complete through v3. Stretch (a thread pool) remains._
