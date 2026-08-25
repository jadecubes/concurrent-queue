# How they work

The three queues are one invariant enforced three ways. This document
derives each from the invariant and points at the lines in `include/cq/`
that carry it.

## The invariant

> **The producer's write of an item must happen-before the consumer's read of it.**

An item like `struct Task { int id; std::function<void()> fn; std::string name; }`
is several independent stores. Without synchronization another core may
observe them in any order — the compiler reorders independent stores, and
the CPU's store buffer drains them lazily (x86 is conservative; ARM and
Apple Silicon are not). In C++ terms this is not "reading a stale value";
it is a **data race, which is undefined behavior**. The consumer can see
the queue's count bumped while `name` still points at the previous
occupant's buffer.

The memory model gives one tool to rule this out:

```
sequenced-before   (sb)  program order within one thread
synchronizes-with  (sw)  a cross-thread edge supplied by a primitive
happens-before          the transitive closure of sb ∪ sw
```

If write W happens-before read R, R sees W. Every primitive that supplies
an `sw` edge has the same shape: a **release** publishes every write
sequenced before it; an **acquire** that observes the release sees them all.

```
item write ──sb──▶ [release] ──sw──▶ [acquire] ──sb──▶ item read
             └───────────── happens-before ─────────────┘
```

Everything below is a choice of what sits in the two brackets.

## `MutexQueue`: the edge is `unlock → lock`

The standard guarantees that, for one mutex, a prior `unlock()`
synchronizes-with a later successful `lock()`. `unlock` is the release,
`lock` is the acquire, and the whole critical section is what gets
published.

```
producer thread                         consumer thread
───────────────────────                 ────────────────────────
lock_guard lk(m)   ← lock
q.push(task)         (writes id/fn/name/size)
}                  ← unlock (release) ──┐
                                        │ synchronizes-with
                                        ▼
                                        cv.wait re-lock (acquire)
                                        q.empty()  → false
                                        q.front()  → reads id/fn/name
```

```
q.push ──sb──▶ unlock ──sw──▶ lock ──sb──▶ q.front()
        └─────────── happens-before ───────────┘
```

Two things make this the easy case:

- **Everything in the critical section is published**, whatever it is. You
  never decide which stores must precede the release; the lock does it.
- **`cv.wait` re-locks on every wake-up**, so whichever iteration finally
  sees a non-empty queue has passed through an acquire.

The correctness check collapses to one question: *are the producer's writes
and the consumer's reads of the container both inside the lock?* Both yes →
the invariant holds. The cost is the same fact from the other side: a
release/acquire over the whole section on every operation, plus the lock
serializing every thread — the numbers in [results.md](results.md).

## `SpscQueue`: the edge is `tail_`

### Layout

A ring of `capacity + 1` slots; `next()` wraps an index. `tail_` is written
only by the producer, `head_` only by the consumer. **One writer per atomic
means no CAS** — a plain store is the whole truth. Full is
`next(tail) == head`, empty is `head == tail`.

### The forward chain

`spsc_queue.ipp`, the two lines that matter:

```cpp
void enqueue(std::size_t tail, T&& value) {
  buffer_[tail] = std::move(value);                     // item write
  tail_.store(next(tail), std::memory_order_release);   // publish
}
```

```
producer thread                              consumer thread
───────────────────────                      ──────────────────────────
buffer_[tail] = task   (writes id/fn/name) ─┐
                                            │ sequenced-before
tail_.store(next, release) ──────┐         ─┘
                                 │ synchronizes-with
                                 │ (the load reads the new tail)
                                 ▼
                                             tail_.load(acquire)      ─┐
                                             head != tail → data       │ sequenced-before
                                             out = move(buffer_[head])─┘
```

Same shape as the mutex diagram; `tail_.store(release)` plays `unlock` and
`tail_.load(acquire)` plays `lock`.

### The reverse chain

The producer will later **overwrite** this slot. That write races with the
consumer's earlier read unless it has its own happens-before edge in the
other direction. `head_` supplies it:

```cpp
void dequeue(std::size_t head, T& out) {
  out = std::move(buffer_[head]);                       // item read
  head_.store(next(head), std::memory_order_release);   // hand the slot back
}
```

```
consumer thread                              producer thread
───────────────────────                      ──────────────────────────
out = move(buffer_[head])  (read done)   ─┐
                                          │ sequenced-before
head_.store(next, release) ──────┐       ─┘
                                 │ synchronizes-with
                                 ▼
                                             head_.load(acquire)
                                             next(tail) != head → room
                                             buffer_[tail] = task  (overwrite)
```

`tail_` carries items producer → consumer; `head_` hands slots back
consumer → producer.

### The cached peer index does not weaken the chain

The acquire load of the peer's index is the one hot-path access that reaches
for a cache line the other core owns. `has_room` / `has_data` consult a
private copy first and only re-load when it says full/empty:

```cpp
bool has_data(std::size_t head) noexcept {
  if (head != tail_cache_) return true;                 // no acquire this time
  tail_cache_ = tail_.load(std::memory_order_acquire);  // refresh
  return head != tail_cache_;
}
```

Skipping the acquire is sound because the *earlier* acquire that produced
the cached value already synchronized with the producer's release of it,
and happens-before is transitive: every slot written before that release is
published, and the consumer cannot run past `tail_cache_` without
refreshing. Each side only ever reads slots the cached index already
licenses.

The check for this queue is: *is the item write sequenced before
`tail_.store(release)`, and the item read sequenced after the
`tail_.load(acquire)` that licensed it?* Nothing enforces this for you —
swap the two lines of `enqueue` and the queue is silently racy.

## Why `SpscQueue` cannot take a second producer

Two producers both load `tail_ == 5`, both write `buffer_[5]`, both store
6 — one item is lost. The obvious fix is a CAS so only one of them advances
`tail_`:

```cpp
if (!tail_.compare_exchange_strong(t, next(t), std::memory_order_release)) retry;
buffer_[t] = std::move(v);      // write only after winning
```

Now trace the chain:

```
producer P1                                  consumer
───────────────────────                      ──────────────────────────
tail_ CAS 5→6 (release) ──────┐
                              │ synchronizes-with
                              ▼
                                             tail_.load(acquire) → 6
                                             head=5 != 6 → data
                                             out = move(buffer_[5])  ← read
buffer_[5] = task             ← write, sequenced AFTER the release
```

A release publishes only what is sequenced before it. The item write is
after the CAS, so it is outside the published set: the consumer's acquire
pairs correctly with the CAS and learns "slot 5 is claimed", not "slot 5 is
filled". Writing before the CAS is no better — until the CAS wins, the
producer does not know which slot is its.

This is the whole difficulty of MPMC: **claiming a slot and filling a slot
are two separate moments.** In SPSC they coincide (advancing `tail_` *is*
filling), so one index can express both. With several producers a shared
index can only express the claim.

## `MpmcQueue`: the edge moves onto each slot

### Layout (Vyukov's bounded MPMC queue)

- `enqueue_pos_` / `dequeue_pos_` only **hand out tickets**. They are
  contended with CAS, but they carry no item visibility, so the CAS is
  `relaxed`.
- Each slot carries a `sequence`. **This is the edge.** The encoding is
  doubled: `2t` means "free for the producer holding ticket t", `2t + 1`
  means "holds ticket t's item".

### The forward chain

`mpmc_queue.ipp`, the producer's winning path:

```cpp
if (dif == 0 && enqueue_pos_.compare_exchange_weak(ticket, ticket + 1, relaxed)) {
  slot.value = std::move(value);                                   // item write
  slot.sequence.store((2 * ticket) + 1, std::memory_order_release); // publish
}
```

```
producer (holding ticket t)                  consumer (holding ticket t)
───────────────────────                      ──────────────────────────
enqueue_pos_ CAS t→t+1 (relaxed)             dequeue_pos_ CAS t→t+1 (relaxed)
   ↑ ticket only — not on the chain             ↑ likewise

slot.value = task   (writes id/fn/name) ─┐
                                         │ sequenced-before
slot.sequence.store(2t+1, release) ─┐   ─┘
                                    │ synchronizes-with
                                    │ (the load reads 2t+1)
                                    ▼
                                             slot.sequence.load(acquire) → 2t+1 ─┐
                                             dif == 0 → filled                  │ sequenced-before
                                             out = move(slot.value)            ─┘
```

The consumer **never looks at `enqueue_pos_`**. It looks at its own slot's
`sequence`. A producer that won a ticket and was then descheduled has left
`sequence` at `2t`, so the consumer reports "not yet" instead of reading
half an item.

The reverse chain is symmetric: after reading, the consumer stores
`sequence = 2(t + N)` (release) — "free for ticket t + N", the next lap's
producer at this slot — and that producer's acquire load pairs with it
before overwriting.

The check for this queue: *is the item write sequenced before **this slot's**
`sequence.store(release)`, and the item read after **this slot's**
`sequence.load(acquire)`?* The ticket CAS can be relaxed precisely because
it does not appear in that sentence. A stale `sequence` read can only make a
thread re-check or report full/empty, never claim a slot out of turn.

### Two deviations from Vyukov, and the trade-off

Both deviations are for contract parity with the other queues: capacity is
arbitrary rather than a power of two (indexing by modulo, not a mask), and
the sequence encoding is doubled — free = 2·ticket, full = 2·ticket + 1 —
because the classic `t` / `t + 1` encoding collides at capacity 1, where
"holds ticket t's item" and "free for ticket t + 1" are the same number.

The trade-off the numbers show: a producer that won a ticket and stalled
blocks every consumer at that slot, even if later slots are already filled.
The design is obstruction-free in practice rather than strictly lock-free,
and under 4+4 contention the CAS retry traffic is what loses to the mutex —
`MpmcQueue` is not "a faster `SpscQueue`"; its progress guarantee differs.

## Summary

| | Release (plays `unlock`) | Acquire (plays `lock`) | Must precede the release | Reverse chain |
|---|---|---|---|---|
| `MutexQueue` | `unlock()` | `lock()`, incl. `cv.wait` re-lock | the whole critical section — automatic | same lock |
| `SpscQueue` | `tail_.store` | `tail_.load` | `buffer_[tail] = item` — by hand | `head_` |
| `MpmcQueue` | `slot.sequence.store(2t+1)` | `slot.sequence.load` | `slot.value = item` — by hand | `sequence.store(2(t+N))` |

From the lock to `MpmcQueue` the invariant does not change at all. Two
things do:

1. The edge goes from "publish everything in the section" to "publish only
   what you put before it" — ordering the item write becomes the code's job.
2. The edge goes from one shared index to one per slot — because with
   several producers a shared index can say "claimed" but not "filled".
