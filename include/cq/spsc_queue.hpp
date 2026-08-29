#ifndef CQ_SPSC_QUEUE_HPP_
#define CQ_SPSC_QUEUE_HPP_

#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

#include "cq/backoff.hpp"
#include "cq/cache_line.hpp"

namespace cq {

/// v2: bounded FIFO ring for exactly one producer thread and one consumer
/// thread, synchronized with atomics only — no mutex, no condition variables.
/// The blocking push()/pop() spin briefly, then sleep with doubling timed
/// backoff — near-zero CPU while blocked, wakeup within kMaxSleep. Each side
/// caches the peer's index (added in v2.1), so a steady stream of pushes and
/// pops decides "there is room" / "there is data" without touching the other
/// core's cache line.
///
/// Thread-safety: at most one thread may call the producer side (push,
/// try_push) and at most one thread the consumer side (pop, try_pop),
/// concurrently with each other. close(), closed(), size(), and capacity()
/// may be called from any thread. closed() and size() return advisory
/// snapshots — drive control flow off the push/pop return values instead,
/// with one exception: try_push()/try_pop() return false for "not now" and
/// for "never again" alike, so a non-blocking retry loop needs closed() to
/// terminate.
///
/// Lifetime: the queue must outlive both threads using it — call close() and
/// join the producer/consumer before destruction. Destroying the queue while
/// a thread is spinning in push()/pop() is undefined behavior.
///
/// Exceptions: for single-element operations, if T's move assignment throws
/// the indices do not move — nothing is lost or duplicated — but element
/// values are not protected: a failed push() enqueues nothing, and a failed
/// pop() leaves both out and the still-queued element valid-but-unspecified.
/// Unreachable for a noexcept move assignment. try_push_n()/try_pop_n()
/// publish one index per batch, so a throw part-way through would lose or
/// duplicate elements; they static_assert a noexcept move assignment instead.
///
/// Arguments: push operations take T by value. An rvalue argument is moved
/// from at the call — even when the push fails, in which case the value is
/// discarded. An lvalue argument is copied and left intact. A try_push()
/// retry loop must therefore re-materialise its argument every pass.
///
/// @tparam T Element type. Must be DefaultConstructible (ring slots are
///   constructed up front) and MoveAssignable; try_push_n()/try_pop_n()
///   additionally require a noexcept move assignment and static_assert it.
template <typename T>
// The "excessive padding" the analyzer flags is deliberate: head_ and tail_
// each get a private cache line (see cq/cache_line.hpp).
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class SpscQueue {
 public:
  /// @param capacity Fixed number of usable slots; never resized.
  /// @throws std::invalid_argument if capacity is 0.
  /// @throws std::length_error if capacity + 1 (the ring allocates one extra,
  ///   permanently empty slot) overflows std::size_t.
  explicit SpscQueue(std::size_t capacity);

  // Not copyable or movable: both threads hold references to the same ring;
  // share the queue by reference instead.
  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;
  SpscQueue(SpscQueue&&) = delete;
  SpscQueue& operator=(SpscQueue&&) = delete;

  /// Enqueues a value, waiting while the queue is full (brief spin, then a
  /// timed sleep — near-zero CPU while blocked). Producer side.
  /// @param value Element to enqueue; see the class note on by-value arguments.
  /// @return false if the queue is closed; the value is discarded.
  [[nodiscard]] bool push(T value);

  /// Enqueues a value without blocking. Producer side.
  /// @param value Element to enqueue; see the class note on by-value arguments.
  /// @return false if the queue is full or closed. closed() tells them apart;
  ///   see the README's "Non-blocking loops" for the retry idiom.
  [[nodiscard]] bool try_push(T value);

  /// Dequeues into out, waiting while the queue is empty and open (brief
  /// spin, then a timed sleep). Consumer side.
  /// @param[out] out Receives the dequeued element on success.
  /// @return false once the queue is closed and drained.
  [[nodiscard]] bool pop(T& out);

  /// Dequeues into out without blocking. Consumer side.
  /// @param[out] out Receives the dequeued element on success; untouched on a
  ///   false return; disturbed if T's move assignment throws (see Exceptions).
  /// @return false if the queue is empty.
  [[nodiscard]] bool try_pop(T& out);

  /// Moves up to items.size() items from the front of items into the ring
  /// with a single index publish — the batched-publish optimization.
  /// Producer side.
  /// @param items Source span; the first k elements are moved from.
  /// @return k, the number enqueued (0 if the ring is full or closed).
  [[nodiscard]] std::size_t try_push_n(std::span<T> items);

  /// Moves up to out.size() items into the front of out with a single index
  /// publish. Consumer side.
  /// @param[out] out Destination span; the first k elements are written.
  /// @return k, the number dequeued (0 if the ring is empty).
  [[nodiscard]] std::size_t try_pop_n(std::span<T> out);

  /// Closes the queue: push() and try_push() refuse new values, pop() drains
  /// what remains and then returns false. Idempotent; callable from any
  /// thread. Waiting push()/pop() calls wake and return.
  ///
  /// To guarantee the consumer drains every item, stop the producer before
  /// calling close(): a push racing with close() may enqueue an item after
  /// the consumer has already observed the queue as closed and drained.
  void close() noexcept;

  /// @return true once close() has been called (advisory snapshot).
  [[nodiscard]] bool closed() const noexcept;

  /// @return Current number of queued elements (advisory snapshot).
  [[nodiscard]] std::size_t size() const noexcept;

  /// @return Fixed capacity set at construction.
  [[nodiscard]] std::size_t capacity() const noexcept;

 private:
  // Validates capacity and returns the slot count for the ring (capacity + 1).
  [[nodiscard]] static std::size_t ring_slots(std::size_t capacity);

  // The slot write and the index publish that make up one queue op; the
  // callers decide *whether* to run them (full/empty/closed checks).
  void enqueue(std::size_t tail, T&& value);
  void dequeue(std::size_t head, T& out);

  // Would writing at slot_after be safe (producer), and is there anything at
  // head to read (consumer)? Each checks this side's cached view of the
  // opposite index first and only re-reads the real one when the cache says
  // full/empty — that skipped read is the whole point, since it is the one
  // hot-path access that reaches into the other core's cache line. Neither is
  // const: both write back the value they refresh.
  [[nodiscard]] bool has_room(std::size_t slot_after) noexcept;
  [[nodiscard]] bool has_data(std::size_t head) noexcept;

  [[nodiscard]] std::size_t next(std::size_t index) const noexcept;

  // Classic Lamport ring: one slot is kept permanently empty so head_ ==
  // tail_ means empty and next(tail_) == head_ means full, with no shared
  // size counter. buffer_ therefore holds capacity + 1 slots.
  std::vector<T> buffer_;
  // head_ is written only by the consumer, tail_ only by the producer; each
  // side reads the other's index with acquire to see the slots it published.
  //
  // Each index shares its cache line with the *_cache_ member belonging to
  // the same side: that side's last-seen value of the opposite index. Since
  // both indices only advance and a cache is only ever refreshed from the
  // real index, a cache lags but never runs ahead — so "cache says not
  // full/not empty" is always true and the peer's line is never touched.
  // Only the equal case has to re-read, and may prove a false alarm.
  alignas(kCacheLineSize) std::atomic<std::size_t> head_ = 0;
  std::size_t tail_cache_ = 0;  // consumer-private view of tail_
  alignas(kCacheLineSize) std::atomic<std::size_t> tail_ = 0;
  std::size_t head_cache_ = 0;  // producer-private view of head_
  alignas(kCacheLineSize) std::atomic<bool> closed_ = false;
};

}  // namespace cq

#include "cq/spsc_queue.ipp"  // IWYU pragma: keep

#endif  // CQ_SPSC_QUEUE_HPP_
