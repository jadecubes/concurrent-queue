// Member function definitions for cq::MpmcQueue. Included at the bottom of
// mpmc_queue.hpp — templates must be visible at every instantiation point,
// so this file cannot be compiled as a standalone translation unit.
#ifndef CQ_MPMC_QUEUE_IPP_
#define CQ_MPMC_QUEUE_IPP_

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cq {

template <typename T>
MpmcQueue<T>::MpmcQueue(std::size_t capacity) : slots_(make_slots(capacity)) {
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    // Lap zero: every slot starts free for the producer holding ticket i.
    // Relaxed is enough — no other thread can touch the queue yet.
    slots_[i].sequence.store(2 * i, std::memory_order_relaxed);
  }
}

template <typename T>
std::vector<typename MpmcQueue<T>::Slot> MpmcQueue<T>::make_slots(std::size_t capacity) {
  if (capacity == 0) {
    throw std::invalid_argument("MpmcQueue capacity must be > 0");
  }
  return std::vector<Slot>(capacity);
}

// Synchronization protocol (Vyukov's bounded MPMC queue):
//
// A side takes a ticket by CAS-incrementing its position counter, but only
// after the slot's sequence says the slot is in the right state for that
// ticket. A sequence of 2*t means "free for the producer holding ticket t";
// 2*t + 1 means "holds ticket t's data". (Vyukov's original uses t and t+1,
// which collides at capacity 1 — "data for ticket t" equals "free for ticket
// t+1" when consecutive tickets share the one slot; the factor of two keeps
// the two states apart by parity at every capacity.)
//
// All handoff synchronization runs through the sequences:
//   producer: CAS ticket, write slot.value, store sequence = 2*ticket+1
//             (release)
//   consumer: sequence acquire-load pairs with that release, so the value
//             write is visible; move it out, store sequence = 2*(ticket+N)
//             (release), which the next-lap producer's acquire-load pairs
//             with before it overwrites the slot.
// The CASes on the position counters are relaxed: they only distribute
// tickets, and a winner publishes nothing until its release store on the
// slot's sequence. A stale sequence read can only make a thread re-check or
// report full/empty — never claim a slot out of turn.
//
// The signed difference (seq - expected) classifies a slot: zero means ours,
// negative means the previous lap is unfinished (ring full, or a claimed but
// not-yet-published slot on the consumer side — reported as empty), positive
// means another thread of our side already took the ticket, so reload.
// Doubled tickets wrap at 2^63; with a non-power-of-two capacity the slot
// indexing would go wrong at that point, which at 10^9 ops/s is ~290 years
// away.
//
// close() is a release store and pop()'s acquire load pairs with it, so a
// closing thread that has synchronized with the producers (joined them)
// publishes all their pushes to any consumer that observes closed — that is
// what lets pop() decide "closed and drained" with one final try_pop.

template <typename T>
bool MpmcQueue<T>::push(T value) {
  // The closed check comes first so a close is honored even when the ring
  // has room.
  while (true) {
    if (closed_.load(std::memory_order_relaxed)) {
      return false;
    }
    if (try_enqueue(value)) {
      return true;
    }
    std::this_thread::yield();
  }
}

template <typename T>
bool MpmcQueue<T>::try_push(T value) {
  if (closed_.load(std::memory_order_relaxed)) {
    return false;
  }
  return try_enqueue(value);
}

template <typename T>
bool MpmcQueue<T>::pop(T& out) {
  while (true) {
    if (try_dequeue(out)) {
      return true;
    }
    // Acquire pairs with close()'s release store: after seeing closed_, every
    // push it publishes is visible, so one more failed try_dequeue means
    // drained.
    if (closed_.load(std::memory_order_acquire)) {
      return try_dequeue(out);
    }
    std::this_thread::yield();
  }
}

template <typename T>
bool MpmcQueue<T>::try_pop(T& out) {
  return try_dequeue(out);
}

template <typename T>
bool MpmcQueue<T>::try_enqueue(T& value) {
  auto ticket = enqueue_pos_.load(std::memory_order_relaxed);
  while (true) {
    auto& slot = slots_[ticket % slots_.size()];
    const auto seq = slot.sequence.load(std::memory_order_acquire);
    const auto dif = static_cast<std::ptrdiff_t>(seq - (2 * ticket));
    if (dif == 0) {  // Slot is free for this ticket — race to claim it
      if (enqueue_pos_.compare_exchange_weak(ticket, ticket + 1, std::memory_order_relaxed)) {
        slot.value = std::move(value);
        slot.sequence.store((2 * ticket) + 1, std::memory_order_release);
        return true;
      }
      // Lost the race; ticket was reloaded by the failed CAS.
    } else if (dif < 0) {
      return false;  // Previous lap still owns the slot: ring is full
    } else {
      ticket = enqueue_pos_.load(std::memory_order_relaxed);
    }
  }
}

template <typename T>
bool MpmcQueue<T>::try_dequeue(T& out) {
  auto ticket = dequeue_pos_.load(std::memory_order_relaxed);
  while (true) {
    auto& slot = slots_[ticket % slots_.size()];
    const auto seq = slot.sequence.load(std::memory_order_acquire);
    const auto dif = static_cast<std::ptrdiff_t>(seq - ((2 * ticket) + 1));
    if (dif == 0) {  // Slot holds data for this ticket — race to claim it
      if (dequeue_pos_.compare_exchange_weak(ticket, ticket + 1, std::memory_order_relaxed)) {
        out = std::move(slot.value);
        slot.sequence.store(2 * (ticket + slots_.size()), std::memory_order_release);
        return true;
      }
      // Lost the race; ticket was reloaded by the failed CAS.
    } else if (dif < 0) {
      return false;  // Nothing published for this ticket yet: empty
    } else {
      ticket = dequeue_pos_.load(std::memory_order_relaxed);
    }
  }
}

template <typename T>
void MpmcQueue<T>::close() noexcept {
  closed_.store(true, std::memory_order_release);
}

template <typename T>
bool MpmcQueue<T>::closed() const noexcept {
  return closed_.load(std::memory_order_acquire);
}

template <typename T>
std::size_t MpmcQueue<T>::size() const noexcept {
  // Two independent relaxed loads: the result is a snapshot that may be
  // stale (or momentarily torn between the two loads) by the time the caller
  // looks at it, which the contract allows. Clamp the racy edges.
  const auto tail = enqueue_pos_.load(std::memory_order_relaxed);
  const auto head = dequeue_pos_.load(std::memory_order_relaxed);
  const auto dif = static_cast<std::ptrdiff_t>(tail - head);
  if (dif < 0) {
    return 0;
  }
  const auto queued = static_cast<std::size_t>(dif);
  return queued < slots_.size() ? queued : slots_.size();
}

template <typename T>
std::size_t MpmcQueue<T>::capacity() const noexcept {
  return slots_.size();
}

}  // namespace cq

#endif  // CQ_MPMC_QUEUE_IPP_
