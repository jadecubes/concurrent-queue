# How they work

Why `SpscQueue` is faster than `MpmcQueue` on the workload both support, in
two diagrams.

## One writer per index

`SpscQueue`. The producer owns `tail_`, the consumer owns `head_`. Because
only one thread ever writes an index, publishing is a plain release store and
reading it is a plain acquire load — no atomic read-modify-write anywhere on
the hot path. Each side also caches the other's index, so a steady stream
mostly avoids reading the peer's cache line at all.

```mermaid
sequenceDiagram
    autonumber
    participant P as Producer
    participant R as Ring
    participant C as Consumer

    Note over P: has_room: compare tail+1 with cached head<br/>cache says room, so the peer line is never read
    P->>R: buffer_[tail] = value
    P->>R: tail_.store(tail+1, release)
    Note over R: this release pairs with the acquire below<br/>and publishes the slot write
    Note over C: has_data: cache says empty, so refresh
    C->>R: tail_.load(acquire)
    C->>R: out = move(buffer_[head])
    C->>R: head_.store(head+1, release)
    Note over R: publishes slot-free back to the producer
```

## Several writers per index

`MpmcQueue`, Vyukov's bounded design. A side claims a ticket by CAS-ing a
position counter it shares with every other thread on that side, so the claim
can lose and have to retry. The handoff then runs through a sequence counter
carried by each slot rather than through the shared indices.

```mermaid
sequenceDiagram
    autonumber
    participant A as Producer A
    participant B as Producer B
    participant S as Slot t mod N
    participant C as Consumer

    A->>S: sequence.load(acquire) - free for ticket t
    B->>S: sequence.load(acquire) - free for ticket t
    Note over A,B: both now want the same ticket
    A->>A: CAS enqueue_pos_ t to t+1 - wins
    B->>B: CAS fails - reload and retry
    A->>S: value = ...
    A->>S: sequence.store(2t+1, release)
    C->>S: sequence.load(acquire) - holds ticket t
    C->>S: out = move(value)
    C->>S: sequence.store(2(t+N), release)
    Note over C: slot is now free for the next lap
```

The two CAS steps — claiming the ticket, and losing the claim — have no
counterpart in the first diagram. Under contention they are where the threads
spend their time, and they are why `MpmcQueue` loses the 4+4 benchmark to a
plain mutex.

Two deviations from Vyukov's original, both for parity with the other queues:
capacity is arbitrary rather than a power of two (indexing by modulo, not a
mask), and the sequence encoding is doubled — free = 2·ticket, full =
2·ticket + 1 — because the classic encoding collides at capacity 1, where
"holds ticket t's data" and "free for ticket t+1" are the same number.
