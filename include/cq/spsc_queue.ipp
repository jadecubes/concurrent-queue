// Member function definitions for cq::SpscQueue. Included at the bottom of
// spsc_queue.hpp — templates must be visible at every instantiation point,
// so this file cannot be compiled as a standalone translation unit.
#ifndef CQ_SPSC_QUEUE_IPP_
#define CQ_SPSC_QUEUE_IPP_

#include <algorithm>
#include <atomic>
#include <chrono>
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
// A blocked side needs no wakeup from the publisher: it sleeps with timed
// backoff and re-polls (see cq/backoff.hpp), so the publish path carries no
// waiter bookkeeping at all.
//
// close() is a release store; the consumer's acquire load of closed_ in pop()
// therefore also makes every push that preceded the close visible, which is
// what lets pop() decide "closed and drained" with one final try_pop.
//
// Index caching (v2.1): the acquire load of the peer's index is the one hot-
// path access that reaches for a cache line the other core owns, and under a
// steady stream it almost always reports the same thing — plenty of room,
// plenty of data. So each side keeps a private non-atomic copy of the last
// value it read (head_cache_ on the producer, tail_cache_ on the consumer)
// and consults that first, refreshing only when it says full/empty.
//
// Skipping the acquire on the fast path is safe: the *earlier* acquire that
// produced the cached value already synchronized with the peer's release
// store of it, and that transitively published every slot the peer had
// written (or finished moving out of) up to that index.
//
// What bounds the fast path to those slots is that each side re-reads the
// moment its cache says full/empty, so neither can run past what the cached
// value licenses — the producer stops at head_cache_ + capacity, the consumer
// at tail_cache_. Every slot the fast path touches is therefore one the peer
// was already done with as of the cached index, so it never reads a slot
// whose write it has not synchronized with, nor overwrites one the consumer
// has not finished moving out of.

template <typename T>
bool SpscQueue<T>::push(T value) {
  const auto tail = tail_.load(std::memory_order_relaxed);
  const auto slot_after = next(tail);
  // The closed check stays first so a close is honored even when there is
  // room.
  Backoff backoff;
  while (true) {
    if (closed_.load(std::memory_order_relaxed)) {
      return false;
    }
    if (has_room(slot_after)) {
      break;
    }
    backoff.wait();
  }
  enqueue(tail, std::move(value));
  return true;
}

template <typename T>
bool SpscQueue<T>::try_push(T value) {
  if (closed_.load(std::memory_order_relaxed)) {
    return false;
  }
  const auto tail = tail_.load(std::memory_order_relaxed);
  if (!has_room(next(tail))) {
    return false;  // full
  }
  enqueue(tail, std::move(value));
  return true;
}

template <typename T>
bool SpscQueue<T>::pop(T& out) {
  Backoff backoff;
  while (true) {
    if (try_pop(out)) {
      return true;
    }
    // Acquire pairs with close()'s release store: after seeing closed_, every
    // preceding push is visible, so one more failed try_pop means drained.
    if (closed_.load(std::memory_order_acquire)) {
      return try_pop(out);
    }
    backoff.wait();
  }
}

template <typename T>
bool SpscQueue<T>::try_pop(T& out) {
  const auto head = head_.load(std::memory_order_relaxed);
  if (!has_data(head)) {
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
std::size_t SpscQueue<T>::try_push_n(T* items, std::size_t n) {
  if (n == 0 || closed_.load(std::memory_order_relaxed)) {
    return 0;
  }
  auto tail = tail_.load(std::memory_order_relaxed);
  // Free slots as seen through the cache; refresh once if it says none.
  const auto free_slots = [&] {
    return (head_cache_ + buffer_.size() - 1 - tail) % buffer_.size();
  };
  if (free_slots() == 0) {
    head_cache_ = head_.load(std::memory_order_acquire);
  }
  const auto count = std::min(n, free_slots());
  for (std::size_t i = 0; i < count; ++i) {
    buffer_[tail] = std::move(items[i]);
    tail = next(tail);
  }
  if (count != 0) {
    tail_.store(tail, std::memory_order_release);  // one publish for the batch
  }
  return count;
}

template <typename T>
std::size_t SpscQueue<T>::try_pop_n(T* out, std::size_t n) {
  if (n == 0) {
    return 0;
  }
  auto head = head_.load(std::memory_order_relaxed);
  const auto available = [&] { return (tail_cache_ + buffer_.size() - head) % buffer_.size(); };
  if (available() == 0) {
    tail_cache_ = tail_.load(std::memory_order_acquire);
  }
  const auto count = std::min(n, available());
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = std::move(buffer_[head]);
    head = next(head);
  }
  if (count != 0) {
    head_.store(head, std::memory_order_release);  // one publish for the batch
  }
  return count;
}

template <typename T>
bool SpscQueue<T>::has_room(std::size_t slot_after) noexcept {
  if (slot_after != head_cache_) {
    return true;  // cached head already proves there is room
  }
  head_cache_ = head_.load(std::memory_order_acquire);
  return slot_after != head_cache_;
}

template <typename T>
bool SpscQueue<T>::has_data(std::size_t head) noexcept {
  if (head != tail_cache_) {
    return true;  // cached tail already proves there is data
  }
  tail_cache_ = tail_.load(std::memory_order_acquire);
  return head != tail_cache_;
}

template <typename T>
void SpscQueue<T>::close() noexcept {
  closed_.store(true, std::memory_order_release);
}

template <typename T>
bool SpscQueue<T>::closed() const noexcept {
  return closed_.load(std::memory_order_acquire);
}

template <typename T>
std::size_t SpscQueue<T>::size() const noexcept {
  // Two independent relaxed loads: the result is a snapshot that may be
  // stale by the time the caller looks at it, which the contract allows.
  const auto head = head_.load(std::memory_order_relaxed);
  const auto tail = tail_.load(std::memory_order_relaxed);
  return (tail + buffer_.size() - head) % buffer_.size();
}

template <typename T>
std::size_t SpscQueue<T>::capacity() const noexcept {
  return buffer_.size() - 1;
}

template <typename T>
std::size_t SpscQueue<T>::next(std::size_t index) const noexcept {
  return index + 1 == buffer_.size() ? 0 : index + 1;
}

}  // namespace cq

#endif  // CQ_SPSC_QUEUE_IPP_
