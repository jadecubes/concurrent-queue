# concurrent-queue

Three bounded, thread-safe FIFO queues in C++20 — a mutex baseline, a lock-free
single-producer/single-consumer ring, and a lock-free multi-producer ring —
measured against each other and against moodycamel and TBB.

## Start from the pattern you already know

Every C++ producer/consumer begins here:

```cpp
std::mutex m;
std::condition_variable cv;
std::queue<Task> q;

void consumer() {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [] { return !q.empty(); });  // predicate form: absorbs spurious wakeups
    Task t = std::move(q.front());
    q.pop();
    lk.unlock();                             // don't hold a lock across user code
    t.run();
}

void producer(Task t) {
    {
        std::lock_guard<std::mutex> lk(m);
        q.push(std::move(t));                // shared state: only under the lock
    }
    cv.notify_one();                         // cheaper released than held
}
```

Two things there are already right — and they are the two people most often
get wrong: `t.run()` runs after the lock is released, and so does
`notify_one()`.

Four things are missing, and they are what this repository is:

| Missing | What goes wrong | Fixed by |
|---|---|---|
| A bound | A fast producer grows the queue until the process runs out of memory | Fixed capacity — `push` waits, so the producer feels the backpressure |
| A way to stop | `cv.wait` never returns; the program hangs at exit | `close()` — refuses new items, wakes every waiter, still drains what is queued |
| Encapsulation | Three loose objects; any code can touch `q` having forgotten the lock | One object owns all three, and the lock is not reachable |
| A loop | The consumer handles one task and returns | `pop` until the queue is closed *and* drained |

Fix those four and you have `MutexQueue`. The other two throw the mutex away
entirely, which is only possible by giving something else up.

## The contract

All three promise the same things, with one exception noted below. Choose
between them on threading, not on behaviour.

| | |
|---|---|
| **Order** | FIFO |
| **Capacity** | Fixed at construction; never grows |
| **When full** | `push` waits; `try_push` returns `false` |
| **When empty** | `pop` waits; `try_pop` returns `false` |
| **Shutdown** | `close()` refuses new items and wakes every waiter |
| **Draining** | Items already queued still come out after `close()` |
| **Loss** | Nothing accepted is dropped, duplicated, or reordered |
| **Element type** | Any `T` that is `DefaultConstructible` and `MoveAssignable` — plus `noexcept` move assignment for `MpmcQueue` and for `SpscQueue`'s bulk ops, both of which `static_assert` it |
| **Lifetime** | The queue must outlive every thread using it |
| **Non-blocking loops** | `try_push`/`try_pop` return `false` for "not now" and "never again" alike; `closed()` is what lets a retry loop terminate — [see below](#non-blocking-loops) |
| **A throwing move** | The one place the three differ — see below |

Every operation reports whether it succeeded, and every one is `[[nodiscard]]`.

**If `T`'s move assignment can throw**, that is where the three part company.
`MutexQueue` and `SpscQueue` keep their indices intact — nothing is lost or
duplicated, and the count still reconciles — but the element values are not
protected: a failed pop leaves both the destination and the still-queued
element in valid-but-unspecified states, so retrying may yield a hollowed
element. `SpscQueue`'s bulk `try_push_n`/`try_pop_n` are the exception: they
publish one index per batch, so they reject a throwing `T` at compile time
rather than lose or duplicate elements mid-batch.
`MpmcQueue` cannot survive it at all: a throw strands a claimed ticket whose
sequence is never re-published, and every later operation on that slot spins
forever. It therefore refuses such a `T` at compile time rather than degrading
silently.

### Non-blocking loops

`try_push`/`try_pop` return `false` for "not now" and for "never again" alike,
so a loop that only tests the return value never terminates after `close()`.
`closed()` is the discriminator, and both sides have a trap.

**Producer.** `try_push` is a by-value sink, so a failed attempt has already
consumed an rvalue argument. Re-materialise it every pass:

```cpp
while (!q.try_push(make_value())) {   // NOT try_push(std::move(v))
    if (q.closed()) break;
}
```

A move-only value that cannot be re-created has no correct `try_push` loop at
all — after one failed attempt it is gone. Blocking `push()` narrows the window
to "closed" but does not close it: it too drops the value it was given.

**Consumer.** Observing `closed()` is not the same as the queue being empty: a
producer may have pushed between the failed `try_pop` and the check. Re-attempt
once after observing it, and take whatever that attempt gives — which is what
`SpscQueue::pop`/`MpmcQueue::pop` do verbatim (`if (closed()) return
try_pop(out);`). `MutexQueue::pop` reaches the same result differently, by
resolving "closed and drained" under a single lock:

```cpp
for (T item;;) {
    if (!q.try_pop(item)) {
        if (!q.closed()) continue;      // not now — keep trying
        if (!q.try_pop(item)) break;    // closed and drained
    }
    use(item);
}
```

The re-attempt is load-bearing on all three, not defensive. `try_pop`,
`closed()` and the second `try_pop` are three separate acquisitions — no lock
or ordering spans them — so breaking on `closed()` alone strands whatever
arrived in the window, and a form that breaks only when the re-attempt *fails*
is worse still: the element that attempt just retrieved is overwritten by the
next loop condition.

Measured against a producer that pushes twice and then closes, per 400 trials:

| | break on `closed()` alone | the loop above |
|---|---|---|
| `MutexQueue` | 139–178 lost | 0 |
| `SpscQueue` | 4–10 lost | 0 |
| `MpmcQueue` | 2–6 lost | 0 |

`MutexQueue` has the widest window by roughly thirty times, not the narrowest:
the lock hand-off after its failed `try_pop` is long enough for the producer to
complete both pushes *and* the close before the consumer reacquires. What it
genuinely lacks is a different hazard — a push racing `close()` — which is why
its `close()` carries no stop-producers-first precondition while `SpscQueue`'s
and `MpmcQueue`'s do. For those two, the re-attempt is also what orders the
consumer after the producer's last push.

## The three queues

They differ on one axis — **how many threads may touch each side** — and read
as a sequence of trades.

| | `MutexQueue` | `SpscQueue` | `MpmcQueue` |
|---|---|---|---|
| Producer threads | any number | **exactly one** | any number |
| Consumer threads | any number | **exactly one** | any number |
| Exceeding that | — | **undefined behaviour, no diagnostic** | — |
| Bounded wait (`try_push_for`) | yes | no | no |
| Bulk ops (`try_push_n` / `try_pop_n`) | no | yes | no |
| Throwing move assignment | survivable | survivable (single-element ops; bulk ops reject it) | rejected at compile time |
| A blocked thread | sleeps | spins briefly, then sleeps | spins briefly, then sleeps |
| Built from | mutex + condition variables | atomics only | atomics only |

`MutexQueue` gives up nothing. `SpscQueue` gives up all but one thread per
side, which is what turns every index update into a plain store instead of an
atomic read-modify-write. `MpmcQueue` keeps the thread count and pays a CAS
per operation — the one trade that did not pay off.

## How they work

All three enforce one invariant — the one the opening snippet enforces with
its lock:

> **The producer's write of an item must happen-before the consumer's read of it.**

Without it the consumer is not reading stale data — it is a data race, and
the compiler and CPU may hand it half an object. Four events form the hand-off:

```text
Producer thread                    Consumer thread
item write ──sb──▶ release ──sw──▶ acquire ──sb──▶ item read
```

`sb` is same-thread program order; `sw` is the cross-thread synchronization
edge. Together, they make the item write happen-before the item read.

For `SpscQueue`, the producer writes the payload before publishing `tail_`.
The consumer reads the payload only after its acquire load observes that
publication. If the load sees the old tail, synchronization has not occurred —
but the queue also appears empty, so the consumer does not touch the slot. The
availability check and synchronization are the same load.

`MutexQueue` gets the same ordering structurally from unlock/lock. The lock-free
queues encode it explicitly: SPSC publishes through `tail_`, while MPMC
publishes through each slot's sequence counter. The full derivation, against
the shipped code: [docs/design.md](docs/design.md).

## Usage

Header-only. Put `include/` on your include path and include the queue you
picked above.

The canonical producer/consumer shape — this compiles and runs as-is:

```cpp
#include <cq/mutex_queue.hpp>

#include <iostream>
#include <thread>

int main() {
  // Bounded: 64 slots. Declared before the threads, so it outlives them.
  cq::MutexQueue<int> queue(64);
  long long total = 0;

  std::jthread producer([&queue] {
    for (int i = 1; i <= 1000; ++i) {
      if (!queue.push(i)) {  // false => the queue closed; stop early
        return;
      }
    }
    queue.close();  // done producing: lets the consumer drain and exit
  });

  std::jthread consumer([&queue, &total] {
    int value = 0;
    while (queue.pop(value)) {  // false => closed *and* drained
      total += value;
    }
  });

  producer.join();
  consumer.join();
  std::cout << "sum = " << total << '\n';  // 500500
}
```

Two rules are doing the real work there, and both are easy to get wrong:

- **Someone must call `close()`.** `pop` blocks while the queue is empty and
  open, so without a close the consumer waits forever and the program hangs
  instead of exiting. `pop` returns `false` only once the queue is closed
  *and* drained, so closing loses nothing that was already pushed. With
  several producers, join them all before closing.
- **The queue must outlive the threads.** Declaring it before them is enough —
  destruction runs in reverse, so the `jthread`s join first. Destroying a
  queue while a thread sits in `push`/`pop` is undefined behavior.

`SpscQueue` and `MpmcQueue` are drop-in for the above except `try_push_for` /
`try_pop_for`, which only `MutexQueue` has. Use `try_push` / `try_pop` when
you cannot afford to wait at all.

## Performance

Apple M2 Pro, Release, 10 repetitions. An **op** is one `push` or one `pop`,
so moving an item costs two. Higher is better.

| | `MutexQueue` | `SpscQueue` | `MpmcQueue` |
|---|---|---|---|
| Single thread, push then pop | 105.1M | **1.56G** | 286M |
| 1 producer + 1 consumer | 35.7M | **613.7M** | 224M |
| 4 producers + 4 consumers | **21.6M** | not supported | 13.3M |

- **`SpscQueue` is ~17× the mutex** on the one shape it supports.
- **`MpmcQueue` loses to the mutex at 4+4** — 13.3M against 21.6M. Lock-free
  buys progress guarantees, not throughput: under contention its threads retry
  while the mutex's threads sleep.
- **Sleeping when blocked costs the SPSC pair ~9%** (602M → 547M in the same
  session); in exchange a blocked thread burns ~0.5% of a core instead of all
  of it. Bulk transfer buys the loss back sevenfold: `try_push_n` /
  `try_pop_n` publish the index once per batch and move **1.69G ops/s** at
  batches of 64.

Error bars, conditions, the comparison against moodycamel and TBB, and the
reasoning: [docs/results.md](docs/results.md).

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

## Layout

```
include/cq/     header-only queue implementations
tests/          GoogleTest unit + stress tests (run under ThreadSanitizer)
bench/          Google Benchmark throughput / latency benchmarks
docs/           design notes and benchmark writeups
```

## Where this is going

A thread pool built on `MpmcQueue`.
