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

Two things there are already right, and are the two people most often get
wrong: `t.run()` runs after the lock is released, and so does `notify_one()`.

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

All three promise the same things. Choose between them on threading, not on
behaviour.

| | |
|---|---|
| **Order** | FIFO |
| **Capacity** | Fixed at construction; never grows |
| **When full** | `push` waits; `try_push` returns `false` |
| **When empty** | `pop` waits; `try_pop` returns `false` |
| **Shutdown** | `close()` refuses new items and wakes every waiter |
| **Draining** | Items already queued still come out after `close()` |
| **Loss** | Nothing accepted is dropped, duplicated, or reordered |
| **Element type** | Any `T` that is `DefaultConstructible` and `MoveAssignable` |
| **Lifetime** | The queue must outlive every thread using it |

Every operation reports whether it succeeded, and every one is `[[nodiscard]]`.

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
| A blocked thread | sleeps | spins briefly, then sleeps | spins briefly, then sleeps |
| Built from | mutex + condition variables | atomics only | atomics only |

`MutexQueue` gives up nothing. `SpscQueue` gives up all but one thread per
side, which is what turns every index update into a plain store instead of an
atomic read-modify-write. `MpmcQueue` keeps the thread count and pays a CAS
per operation — the one trade that did not pay off.

Both diagrams, and why the CAS costs what it does: [docs/design.md](docs/design.md).

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

Every operation is `[[nodiscard]]`, because ignoring whether a push succeeded
is almost always a bug.

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
