# Q1 source audit — passing an item, revision 1

Pinned implementation: **8c68e32**, on **feat/mutex-queue**, an **unmerged branch**. `main` has only a README. Work is authored in the `tutorials/q1-passing-an-item` worktree; it does not assert that v1 has shipped on main. Paths and lines below refer to this pinned implementation.

Read: `include/cq/mutex_queue.hpp`, `include/cq/mutex_queue.ipp`, `tests/mutex_queue_test.cpp`, `tests/queue_test_util.hpp`, and `README.md`.

| Displayed operation | Definition / contract (file:line) | Exact contract |
|---|---|---|
| `P: push(job)` | `include/cq/mutex_queue.ipp:30–41`; `include/cq/mutex_queue.hpp:59–64` | Takes T by value. Waits in `not_full_` while full and open, releasing the mutex. Reacquires and checks `closed_ || size_ < capacity`. Closed returns false and discards the parameter; otherwise enqueues and notifies one `not_empty_` waiter, returns true. |
| `P: try_push(job)` | `include/cq/mutex_queue.ipp:44–54`; `include/cq/mutex_queue.hpp:66–86` | Takes T by value. Locks mutex; full or closed returns false and discards the parameter. Otherwise enqueues, unlocks, notifies one `not_empty_` waiter, returns true. No capacity wait, but mutex acquisition can block. |
| `C: pop(out)` | `include/cq/mutex_queue.ipp:57–68`; `include/cq/mutex_queue.hpp:88–92` | Waits in `not_empty_` while empty and open. Rechecks `closed_ || size_ > 0` under mutex. Drains queued items even after close; false only when closed and drained. Successful dequeue notifies one `not_full_` waiter. False leaves out intact. |
| `C: try_pop(out)` | `include/cq/mutex_queue.ipp:71–81`; `include/cq/mutex_queue.hpp:94–99` | Locks mutex; empty returns false and leaves out intact; otherwise dequeues, unlocks, notifies one `not_full_` waiter, returns true. Can drain a closed queue. No item wait; not lock-free. |
| `O: close()` | `include/cq/mutex_queue.ipp:84–93`; `include/cq/mutex_queue.hpp:101–104` | One-way, idempotent exchange of `closed_` under mutex. First close unlocks then notifies **both** CVs with `notify_all`; later calls return without another notification. Refuses producers, preserves pending items for consumers to drain. Does not join. |
| `closed()` | `include/cq/mutex_queue.ipp:96–99`; `include/cq/mutex_queue.hpp:106–108` | Locked advisory snapshot. False can become true immediately afterward; true remains true. Does not reserve a future operation. |
| `size()` | `include/cq/mutex_queue.ipp:102–105`; `include/cq/mutex_queue.hpp:110–112` | Locked advisory snapshot. Lock is released on return. A later push may block despite an earlier size < capacity check. |
| `capacity()` | `include/cq/mutex_queue.ipp:107–111`; `include/cq/mutex_queue.hpp:114–115` | Fixed positive capacity, no lock needed: buffer never resizes. Constructor rejects zero (`.ipp:20–24`). |
| `O: destroy the queue` | `include/cq/mutex_queue.hpp:23–25,46–132` | Implicit member destruction, no custom shutdown destructor. Queue must outlive every user. Destroying while a thread is blocked in push/pop is UB, including a notified waiter that has not returned. Finish all accesses and join all users before destruction. Valid destruction also destroys any undrained elements. Never execute the UB path in C++. |

## Ownership and witnesses

Both push signatures take **T by value**. For the lesson's movable Job, an rvalue moves into the parameter at the call, before the mutex and before rejection. Failure destroys this parameter; only a bool comes back. An lvalue copies into the parameter and remains intact. An rvalue expression does not force an arbitrary T to have a move constructor; the lesson assumes the movable Job described on screen. A moved-from payload has type-defined state, not universally an empty string.

`tests/mutex_queue_test.cpp:208–230`, **FailedPushConsumesRvaluesAndLeavesLvaluesIntact**, witnesses lvalue preservation, moved-from string, and loss of a unique_ptr on failed try_push. The reliable retry is `try_push(make_job())` each pass, or blocking `push` for an irreplaceable move-only value (`.hpp:68–82`). A blocking push can still discard its value if closed; it avoids the full-queue retry problem, not shutdown rejection.

Other witnesses: full push waits until pop (`tests/mutex_queue_test.cpp:85–96`), empty pop waits until push (`:77–83`), close drains (`:106–119`), close wakes both kinds of waiter (`:121–132`). `tests/queue_test_util.hpp:16–28` is the bounded observation pattern: launch asynchronous work, expect a timeout, unblock, collect result. Elapsed time is evidence that it has not returned, not proof of having entered the CV wait.

## Protected state, publication and limits

`enqueue_locked` (`.ipp:114–119`) move-assigns into `buffer_[tail_]`, advances tail and increments size. `dequeue_locked` (`:122–127`) move-assigns into out, advances head and decrements size. Indices wrap via `next` (`:130–132`). Mutex protection covers buffer, head, tail, size and closed; its release/acquisition publishes items and makes reuse of a popped slot safe. Notifications occur **after** unlocking. Wait atomically releases the mutex and enters the wait set; return requires reacquisition and predicate recheck. Notification is not completion or ownership.

Any number of producers and consumers is supported (`.hpp:14–15`); the scene uses one each. T must be default-constructible, move-assignable and constructible from call arguments (`:42–44`). Model Job moves do not throw. Actual throwing move assignment preserves queue indices/count but may damage payload values and out (`:27–35`, tests `:176–205,232–243`); the lesson makes no general exception-time value-conservation claim.

No abort API; no forced termination; no implicit join; no fairness, lock-free or wait-free progress guarantee. The `try_` operations omit CV waiting but still acquire the mutex. Coordinate producers before closing if every intended job must be accepted. Closing early is defined but can reject pending pushes. Close does not mean consumers have finished processing the jobs they popped.

The model stops at UB. It separates wait entry, notification and resumption to expose scheduling; other operations are single coarse steps. The mutex is held only within a step and never while sleeping. It omits intra-operation unlock/notify interleavings, spurious wakes and exceptions; the enumerated spaces are exhaustive only for these stated operation-level scenes, not every C++ execution. Destroyed queued items are accounted for separately from delivered items.

## README comparison

No contradictory v1 operation contract was found in the pinned README: it describes a bounded mutex/CV baseline and future v2/v2.5 lock-free work, but does **not** include the detailed ownership, shutdown or lifetime contract. The design spec's older README link and generic description of existing lock-free queues do not apply to this tree. Q1 follows the headers: only MutexQueue is taught; lock-free queues are **not built yet** here. The README's broad throughput conclusion is benchmark commentary, not an API progress guarantee, and is not taught as one.
