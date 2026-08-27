# v2 — Lock-free SPSC ring buffer

Status: accepted 2026-08-27
Date: 2026-08-27

## Goal

Measure what removing the lock actually buys, and decompose *why*. The
deliverable is the README Results table, not the queue.

A throwaway prototype (measured 2026-08-27, Apple M2 Pro, Release, 10
repetitions, load avg ~5) already establishes the target:

| Config | Throughput | Per op | vs v1 `try_` |
|---|---|---|---|
| v1 `MutexQueue`, blocking (published baseline) | 35.8M ops/s | 27.9 ns | 0.88x |
| v1 `MutexQueue`, `try_` path | 40.9M ops/s | 24.4 ns | 1.00x |
| SPSC, neither optimisation | 112.9M ops/s | 8.9 ns | **2.8x** |
| SPSC, padded + index-cached | 209.6M ops/s | 4.8 ns | **5.1x** |

The headline finding to reproduce and write up: **removing the lock is worth
2.8x; the remaining 1.8x is cache-line discipline.** A lock-free queue built
without padding or index caching collects only part of the win.

## Non-goals

- Not a general-purpose queue. MPMC is v2.5 (Vyukov, per-slot sequence counters).
- No blocking `push`/`pop`, no `close()`. See "Decision 1".
- Not competing with moodycamel/TBB. That is v3.
- No dynamic resizing, no allocation on the hot path.

## Public API

```cpp
template <typename T>
class SpscQueue {
 public:
  explicit SpscQueue(std::size_t capacity);   // power of two, >= 1
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;
  SpscQueue(SpscQueue&&) = delete;
  SpscQueue& operator=(SpscQueue&&) = delete;

  [[nodiscard]] bool try_push(T value);   // false if full
  [[nodiscard]] bool try_pop(T& out);     // false if empty
  [[nodiscard]] std::size_t size() const;      // advisory snapshot
  [[nodiscard]] std::size_t capacity() const;  // fixed
};
```

`T`: DefaultConstructible + MoveAssignable, same as v1.

### Threading contract (the part that differs sharply from v1)

**Exactly one producer thread and exactly one consumer thread — for the
lifetime of the queue.** Not "one at a time": the roles must be pinned to
specific threads. Two threads calling `try_push` concurrently corrupts the
queue silently, and there is no read-modify-write anywhere to detect it.

Handing a role to a different thread is allowed only across an existing
happens-before edge (join, or a release/acquire the caller supplies).

`capacity()` and `size()` are callable from either thread; `size()` is an
advisory snapshot that may be stale in the conservative direction only.

## Design

### Indices: free-running 64-bit counters, power-of-two capacity

`head_` and `tail_` count monotonically and are masked at access
(`buffer_[i & mask_]`). Chosen over the wrapped-index / waste-one-slot scheme
because it: keeps the full capacity, replaces `next()`'s branch with a mask,
makes `size()` a plain subtraction, and is the shape v2.5's Vyukov MPMC needs.

Cost: capacity must be a power of two. The constructor throws
`std::invalid_argument` for zero and for any non-power-of-two capacity — a
tighter contract than v1's `capacity > 0`. Capacity 1 is legal (`mask_ == 0`,
every index resolves to slot 0) and is the sharpest test of the slot-reuse
edge, since every push after the first overwrites the slot the consumer just
moved from.

Wraparound of a 64-bit counter is not handled: at 200M ops/s it takes ~2900
years. Documented, not defended against.

### Single-writer ownership

| Member | Producer | Consumer |
|---|---|---|
| `tail_` | writes | reads (acquire) |
| `head_` | reads (acquire) | writes |
| `head_cache_` | private | — |
| `tail_cache_` | — | private |
| `buffer_[i & mask_]` | writes when `i` in `[tail_, head_+cap)` | reads when `i` in `[head_, tail_)` |

No member is written by both threads. That is what removes the need for
mutual exclusion; nothing here is a read-modify-write.

### Memory ordering — the two edges

```
producer:  buffer_[t & mask_] = value        (W)
           tail_.store(t+1, release)         (R)
                                                 -> synchronizes-with
consumer:                                        tail_.load(acquire)   (A)
                                                 out = buffer_[h & mask_]  (Rd)
```

1. **Data edge (producer -> consumer).** (W) sequenced-before (R); (R)
   synchronizes-with (A); (A) sequenced-before (Rd). Therefore (W)
   happens-before (Rd). The consumer only reaches (Rd) by having observed the
   release store, so staleness and visibility are linked: a stale `tail_`
   means it does not read the slot at all.
2. **Slot-reuse edge (consumer -> producer).** The consumer's (Rd) is
   sequenced-before `head_.store(release)`; the producer's `head_.load(acquire)`
   synchronizes-with it, and is sequenced-before its overwrite of that slot.
   This is the write-after-read the mutex was implicitly ordering in v1.

Loading one's *own* index uses `relaxed`: no other thread writes it, so
sequenced-before in the owning thread already guarantees the value.

Invariants: `head_ <= tail_ <= head_ + capacity`, and both advance
monotonically. Consequence: a stale read of either index always errs toward
"less available", never more.

### Cache-line layout

Producer line: `tail_` + `head_cache_`. Consumer line: `head_` + `tail_cache_`.
Separated with `alignas(kCacheLine)`, where `kCacheLine` is a named constant
set to 128. Explicitly *not* `std::hardware_destructive_interference_size`:
libstdc++ emits an ABI-stability warning for its use in headers, and the value
must stay identical across the ablation builds for their comparison to mean
anything. 128 is chosen over 64 because Apple Silicon prefetches in 128-byte
pairs; the sweep will confirm whether 64 suffices.

The producer reads `head_` only when `tail_ - head_cache_ == capacity`, i.e.
only when it believes the queue is full; likewise the consumer. In steady
state with a non-trivial queue depth, neither thread touches the other's line,
and payload transfer amortises over the 8 `uint64_t` that share a 64-byte line.

### Ablation switches

The measurements require padded/unpadded and cached/uncached builds, but the
public type must not carry benchmark knobs. Implement as
`detail::SpscRing<T, bool Padded, bool Cached>` in an internal header, with
`template <typename T> using SpscQueue = detail::SpscRing<T, true, true>`.
The benchmark includes the internal header; tests and users see only
`SpscQueue<T>`.

### Misuse detection

TSan cannot catch "two producers by accident" unless a test spawns two. Add a
debug-only (`NDEBUG`-off) check: record the calling `thread::id` on first
`try_push` / `try_pop` and assert it never changes. Zero cost in Release,
catches the single most likely misuse of this type.

## Files

```
include/cq/detail/spsc_ring.hpp + .ipp   implementation + ablation params
include/cq/spsc_queue.hpp                public alias, docs, contract
tests/spsc_queue_test.cpp                unit tests
tests/spsc_stress_test.cpp               checksum + FIFO + payload visibility
bench/queue_bench.cpp                    extended (see below)
```

Follows v1's header/`.ipp` split and its self-include convention.

## Testing

Unit: construction contract (0 and non-power-of-two throw; 1 succeeds), FIFO order,
wraparound on a small ring, `try_push` on full, `try_pop` on empty, move-only
`T`, `size()`/`capacity()`.

Stress (1 producer + 1 consumer, under TSan):
1. **Checksum + strict FIFO.** v1's checksum only proves nothing is lost or
   duplicated; SPSC additionally guarantees strict ordering, so assert
   `value == last + 1`. Free, and strictly stronger than v1's assertion.
2. **Payload visibility.** Push `std::unique_ptr<std::array<int, N>>`, filled
   by the producer, every element verified by the consumer. v1 gained exactly
   this test in `4499592` (`PayloadWrittenBeforePushIsVisibleAfterPop`), so
   v2's job is to port the harness, not to invent it. Note that under a single
   mutex the test cannot fail alone for the property it names — the same lock
   carries queue state and the transitive edge — so its value is entirely
   forward-looking: a relaxed-index ring reconciles its checksum perfectly
   while TSan flags the payload race, which is the regression v2 can actually
   have.
3. **Small ring, high volume** (capacity 1 and 2, millions of items) so the
   slot-reuse edge is exercised constantly.

## Benchmark plan

Extend `bench/queue_bench.cpp`, keeping its existing harness shape (half the
threads produce, prefill to half capacity, one op per iteration):

- `MutexQueue/try` at 2 threads — **new**, so v1 and v2 are compared on the
  same API. The existing blocking registration stays as the published baseline.
- `SpscQueue/try` at 2 threads.
- Ablations: no-padding, no-index-cache, neither.
- **Queue-depth sweep**: capacity 1, 2, 8, 64, 1024 for both v1 and v2. This
  tests the open hypothesis that the advantage collapses toward 1x in strict
  lock-step, where both designs are bounded by cross-core latency rather than
  by the lock. The prototype's 5.1x was measured at depth 1024 only.

  The v1 half of this sweep already exists (PR #14). Two things it learned
  apply directly to v2's registration: a retry loop with no backoff makes the
  shallow capacities swing ~400x run to run, and every registration must set
  `UseRealTime()` or its throughput is normalised by a different clock than
  its neighbours.
- Single-thread round trip for `SpscQueue`.

MPMC registrations do not apply — the type does not support it.

## README changes

Replace the Results section with v1 and v2 side by side, plus the mechanism
decomposition (instruction cost vs cross-core traffic) and the ablation table.
Keep the existing caveat that the v1 baseline is deliberately the simple
design, and state the machine load for the new runs.

## Decisions

**Decision 1 — no blocking API. SETTLED 2026-08-27.** `SpscQueue` provides
only `try_`. Blocking would require a condition variable, which requires a
mutex, which contaminates the claim v2 exists to test. v1 already provides
blocking semantics for anyone who needs them, and the benchmark comparison is
made fair by adding `MutexQueue/try` rather than by adding blocking to v2.

Accepted cost: v1 and v2 are not drop-in substitutes, which matters for the
stretch-goal thread pool. Revisit at v2.5, where the MPMC queue is the one a
pool would actually use.

The fairness half of this is already done: PR #14 added a `MutexQueue/try_`
throughput registration over a capacity sweep, so v1 and v2 are compared on
the same API without adding blocking to v2.

One consequence to document on `SpscQueue::try_push`, since there is no
blocking `push()` to fall back on: a retry loop must re-materialise its
argument every pass (`while (!q.try_push(make_value()))`), because the
by-value sink has already consumed an rvalue on the failed attempt. That
covers `std::unique_ptr<Payload>` — which the test plan below pushes — as
long as the payload can be re-created. A move-only value that cannot be is
unsupported by this API, and the header must say so.

**Decision 2 — power-of-two capacity.** Tighter than v1's contract. Accepted
for the mask, and because v2.5 needs it too.

**Decision 3 — ablations ship in the internal header, not behind `#ifdef`.**
Template parameters keep every variant compiled and testable.

## Open questions

- Does the 5.1x hold at small queue depths? Unmeasured; the depth sweep answers
  it. Treat the current figure as depth-1024-specific until then.
- Should the depth sweep also re-measure v1's blocking path, or only `try_`?
  Only `try_` is needed for the comparison; blocking adds runtime.
