// Member function definitions for cq::SpscQueue. Included at the bottom of
// spsc_queue.hpp — templates must be visible at every instantiation point,
// so this file cannot be compiled as a standalone translation unit.
#ifndef CQ_SPSC_QUEUE_IPP_
#define CQ_SPSC_QUEUE_IPP_

#include <atomic>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cq {

template <typename T>
SpscQueue<T>::SpscQueue(std::size_t capacity) : buffer_(ring_slots(capacity)) {}

template <typename T>
std::size_t SpscQueue<T>::ring_slots(std::size_t capacity) {
  if (capacity == 0) {
    throw std::invalid_argument("SpscQueue capacity must be > 0");
  }
  if (capacity == std::numeric_limits<std::size_t>::max()) {
    // capacity + 1 would wrap to 0 and construct a broken (empty) ring.
    throw std::length_error("SpscQueue capacity + 1 overflows std::size_t");
  }
  return capacity + 1;
}

// Synchronization protocol, both directions:
//   producer: write buffer_[tail_], then store tail_ (release)
//   consumer: load tail_ (acquire), then read buffer_[head_]
// The release/acquire pair on tail_ makes the slot write visible to the
// consumer; the mirror-image pair on head_ makes the consumer's move-out
// visible to the producer before it overwrites the slot. Each side loads its
// *own* index relaxed — it is the only writer of that index.
//
// close() is a release store; the consumer's acquire load of closed_ in pop()
// therefore also makes every push that preceded the close visible, which is
// what lets pop() decide "closed and drained" with one final try_pop.

template <typename T>
bool SpscQueue<T>::push(T value) {
  const auto tail = tail_.load(std::memory_order_relaxed);
  const auto slot_after = next(tail);
  // Wait for a free slot, bailing out if the queue closes first.
  while (true) {
    if (closed_.load(std::memory_order_relaxed)) {
      return false;
    }
    if (slot_after != head_.load(std::memory_order_acquire)) {
      break;
    }
    std::this_thread::yield();
  }
  enqueue(tail, std::move(value));
  return true;
}

template <typename T>
bool SpscQueue<T>::try_push(T value) {
  const auto tail = tail_.load(std::memory_order_relaxed);
  if (next(tail) == head_.load(std::memory_order_acquire) ||
      closed_.load(std::memory_order_relaxed)) {
    return false;  // full or closed
  }
  enqueue(tail, std::move(value));
  return true;
}

template <typename T>
bool SpscQueue<T>::pop(T& out) {
  while (true) {
    if (try_pop(out)) {
      return true;
    }
    // Acquire pairs with close()'s release store: after seeing closed_, every
    // preceding push is visible, so one more failed try_pop means drained.
    if (closed_.load(std::memory_order_acquire)) {
      return try_pop(out);
    }
    std::this_thread::yield();
  }
}

template <typename T>
bool SpscQueue<T>::try_pop(T& out) {
  const auto head = head_.load(std::memory_order_relaxed);
  if (head == tail_.load(std::memory_order_acquire)) {
    return false;  // empty
  }
  dequeue(head, out);
  return true;
}

template <typename T>
void SpscQueue<T>::enqueue(std::size_t tail, T&& value) {
  buffer_[tail] = std::move(value);
  tail_.store(next(tail), std::memory_order_release);
}

template <typename T>
void SpscQueue<T>::dequeue(std::size_t head, T& out) {
  out = std::move(buffer_[head]);
  head_.store(next(head), std::memory_order_release);
}

template <typename T>
void SpscQueue<T>::close() {
  closed_.store(true, std::memory_order_release);
}

template <typename T>
bool SpscQueue<T>::closed() const {
  return closed_.load(std::memory_order_acquire);
}

template <typename T>
std::size_t SpscQueue<T>::size() const {
  // Two independent relaxed loads: the result is a snapshot that may be
  // stale by the time the caller looks at it, which the contract allows.
  const auto head = head_.load(std::memory_order_relaxed);
  const auto tail = tail_.load(std::memory_order_relaxed);
  return (tail + buffer_.size() - head) % buffer_.size();
}

template <typename T>
std::size_t SpscQueue<T>::capacity() const {
  return buffer_.size() - 1;
}

template <typename T>
std::size_t SpscQueue<T>::next(std::size_t index) const {
  return index + 1 == buffer_.size() ? 0 : index + 1;
}

}  // namespace cq

#endif  // CQ_SPSC_QUEUE_IPP_
