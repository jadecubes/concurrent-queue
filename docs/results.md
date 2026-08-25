# Benchmark results

The record behind the summary on the front page. Organised by milestone,
because that is the order the numbers arrived in. Two milestones are the same
class at different points in time:

| Milestone | Class | What changed |
|---|---|---|
| v1 | `MutexQueue` | Mutex + condition variables; the baseline |
| v2 | `SpscQueue` | Lock-free SPSC ring |
| v2.1 | `SpscQueue` | Cached peer indices — same class, optimised |
| v2.5 | `MpmcQueue` | Vyukov-style bounded MPMC |
| v3 | — | The three against moodycamel and TBB |

## How these were measured

Apple M2 Pro (12 cores), 32 GB, macOS 26. Release build,
`--benchmark_repetitions=10`, `MinTime` 1s per run. The machine was never idle
(load ~4–6), so read every figure as a floor.

An **op** is one `push` or one `pop`, so moving an item costs two. Halve for
items/s. Per-op figures are `1 / throughput` across the whole queue, not
per-thread latency.

Figures come from several sessions. Each session rebuilt its predecessors from
their own commits and ran them in the same binary as controls; every control
reproduced its published figure within noise, which is what makes the columns
comparable. The one measurement that did not survive re-testing is called out
below.

## Summary

| Shape | v1 `MutexQueue` | v2 `SpscQueue` | v2.1 `SpscQueue` | v2.5 `MpmcQueue` |
|---|---|---|---|---|
| Single thread, push then pop | 105.1M | **2.00G** | 1.56G | 286M |
| 1 producer + 1 consumer | 35.7M | 369M | **613.7M** | 224M |
| 4 producers + 4 consumers | **21.6M** | not supported | not supported | 13.3M |

## v1 — MutexQueue

| Shape | Throughput | Per op | CV |
|---|---|---|---|
| Single thread, push then pop | 105.1M ± 0.7M | 9.5 ns (19.0 ns per round trip) | 2.2% |
| 1 producer + 1 consumer | 35.7M ± 0.5M | 28.0 ns | 1.4% |
| 4 producers + 4 consumers | 21.6M ± 0.1M | 46.3 ns | 0.5% |

One mutex serialises everything, so **threads never buy throughput**: the
uncontended round trip moves ops ~3× faster than two threads managing it, and
going from 2 threads to 8 loses a further 40%.

This is deliberately the *simple* single-mutex design, not the fastest one — it
notifies on every op even with no thread waiting. A variant tracking waiter
counts measures ~20% faster at 4+4 and ~26–37% faster at 1p+1c. Worth knowing
before crediting the lock-free queues with the whole gap.

## v2 — SpscQueue

| Shape | Throughput | Per op | CV |
|---|---|---|---|
| Single thread, push then pop | 2.00G ± 0.01G | 0.50 ns (1.00 ns per round trip) | 0.7% |
| 1 producer + 1 consumer | 369M ± 14M | 2.7 ns | 3.9% |

Dropping the mutex buys **~10×** on the 1p+1c pair and **~19×** on the
uncontended round trip: an op becomes a handful of instructions with no atomic
read-modify-write. The new cost centre is cache coherence — the same ring that
moves 2.00G ops/s on one core drops to 369M once the two sides sit on
different cores and the `head_`/`tail_` lines ping-pong.

> **Retracted measurement.** An earlier session on a busier machine recorded
> 252M ± 55M for this pair at 21.9% CV. The spread was the tell; the figures
> above replace it, and v2.1's independent re-run agrees with them.

## v2.1 — cached peer indices

Each side keeps a private copy of the last value it read from the opposite
index and checks that first. Both indices only advance, so a cached "not full"
/ "not empty" cannot be wrong — a steady stream never reads the peer's cache
line at all.

| Shape | v2 | v2.1 | Change |
|---|---|---|---|
| 1 producer + 1 consumer | 361.7M ± 10.9M (5.53 ns) | **613.7M ± 6.3M** (3.26 ns) | **1.70× faster** |
| Single thread, push then pop | 2.006G ± 0.005G (0.998 ns) | 1.555G ± 0.005G (1.29 ns) | **0.77× — 23% slower** |

**The regression is not incidental.** The round-trip benchmark pushes one item
and immediately pops it, so the ring sits pinned at empty and *every* pop finds
its cache stale — paying the extra compare and write-back while never once
skipping a peer read. That is the cache's worst case. A ring with slack, which
is any real stream, skips nearly all of them.

Against the same-session v1 baseline: **v2 is ~10× v1, v2.1 is ~17×**.

At 3.26 ns/op the pair is now dominated by the handoff itself — the release
store to `tail_` must reach the other core before the consumer can advance,
and caching cannot remove that dependency. Publishing once per batch of N is
the next lever, trading latency for it.

## v2.5 — MpmcQueue

Design and encoding notes: [design.md](design.md).

| Shape | Throughput | vs v1 | CV |
|---|---|---|---|
| Single thread, push then pop | 286M ± 1M | 2.7× | 0.3% |
| 1 producer + 1 consumer | 224M ± 4M | 6.2× | 1.6% |
| 4 producers + 4 consumers | **13.3M ± 0.6M** | **0.62× — slower than the mutex** | 4.3% |

On the MPMC shape this queue exists for, it **loses to the mutex** (13.3M vs
21.6M), while winning the shapes with little or no contention.

Why, read from the shape of the numbers rather than a profiler: every op is an
atomic RMW on one of two position counters all eight threads hammer, so CAS
retry traffic thrashes exactly the cache lines the SPSC ring avoids — whereas
the mutex serialises through one futex and the losers sleep instead of
retrying.

## v3 — versus moodycamel and TBB

One binary, one session (moodycamel v1.0.4, oneTBB v2022.2.0). Read against
the contracts: TBB's bounded queue matches ours; `moodycamel::ConcurrentQueue`
is **unbounded** and FIFO only **per producer** — an easier question, counted
the same way.

| ops/s, mean ± stddev | v1 mutex | v2.1 SPSC | v2.5 MPMC | TBB bounded | moodycamel |
|---|---|---|---|---|---|
| Single thread, push then pop | 104.5M ± 0.2M | 1.54G ± 0.01G | 277.7M ± 0.8M | 113.3M ± 0.4M | 165.8M ± 1.9M |
| 1 producer + 1 consumer | 34.2M ± 0.4M | **602M ± 11M** | 216.2M ± 4.7M | 22.6M ± 2.0M | 78.9M ± 2.6M |
| 4 producers + 4 consumers | **21.0M ± 0.2M** | — | 13.6M ± 0.7M | 16.5M ± 0.5M | **43.5M ± 2.9M** |

**At 4+4 the ranking is moodycamel > mutex > TBB > ours.** The queue built for
the MPMC job finishes last on it, and *both* ticket-based designs lose to a
plain mutex. moodycamel escapes structurally — a sub-queue per producer — and
pays for it with the weaker per-producer-FIFO contract.

**With little contention the specialised designs win big.** At 1p+1c our SPSC
ring is 7.6× moodycamel and 27× TBB; even our Vyukov queue is 2.7× moodycamel.
Specialising to the actual concurrency shape beats generic cleverness.

**So: why does mine lose?** It keeps global FIFO and bounded semantics on one
shared ring. The queue that beat it gave both of those up. Lock-free bought
progress guarantees and large wins where contention is structurally limited —
not throughput under real MPMC contention.

_Roadmap complete through v3. Stretch (a thread pool) remains._
