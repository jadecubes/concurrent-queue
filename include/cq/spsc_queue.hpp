#ifndef CQ_SPSC_QUEUE_HPP_
#define CQ_SPSC_QUEUE_HPP_

#include <atomic>
#include <cstddef>
#include <vector>

namespace cq {

// Pad the producer- and consumer-owned atomics onto separate cache lines so a
// store on one side does not invalidate the other side's line (false sharing).
// A fixed constant rather than std::hardware_destructive_interference_size:
// that value moves with compiler version and tuning flags (GCC warns about
// any header use for exactly that reason), and 128 covers both x86-64
// adjacent-line prefetching and Apple/ARM64 hardware.
inline constexpr std::size_t kCacheLineSize = 128;

/// v2: bounded FIFO ring for exactly one producer thread and one consumer
/// thread, synchronized with atomics only — no mutex, no condition variables.
/// The blocking push()/pop() spin with std::this_thread::yield() instead of
/// sleeping. Each side caches the peer's index (added in v2.1), so a steady
/// stream of pushes and pops decides "there is room" / "there is data"
/// without touching the other core's cache line.
///
/// Thread-safety: at most one thread may call the producer side (push,
/// try_push) and at most one thread the consumer side (pop, try_pop),
/// concurrently with each other. close(), closed(), size(), and capacity()
/// may be called from any thread. closed() and size() return advisory
/// snapshots — drive control flow off the push/pop return values instead.
///
/// Lifetime: the queue must outlive both threads using it — call close() and
/// join the producer/consumer before destruction. Destroying the queue while
/// a thread is spinning in push()/pop() is undefined behavior.
///
/// Exceptions: if T's move assignment throws, the failing push()/try_push()
/// enqueues nothing and the failing pop()/try_pop() leaves the element
/// queued — the queue itself stays consistent.
///
/// @tparam T Element type. Must be DefaultConstructible (ring slots are
///   constructed up front) and MoveAssignable.
template <typename T>
// The "excessive padding" the analyzer flags is deliberate: head_ and tail_
// each get a private cache line (see kCacheLineSize above).
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

  /// Enqueues a value, spinning while the queue is full. Producer side.
  /// @param value Element to enqueue; consumed even when the push fails.
  /// @return false if the queue is closed (the value is dropped).
  [[nodiscard]] bool push(T value);

  /// Enqueues a value without blocking. Producer side.
  /// @param value Element to enqueue; consumed even when the push fails.
  /// @return false if the queue is full or closed.
  [[nodiscard]] bool try_push(T value);

  /// Dequeues into out, spinning while the queue is empty and open.
  /// Consumer side.
  /// @param[out] out Receives the dequeued element on success.
  /// @return false once the queue is closed and drained.
  [[nodiscard]] bool pop(T& out);

  /// Dequeues into out without blocking. Consumer side.
  /// @param[out] out Receives the dequeued element on success; untouched on
  ///   failure.
  /// @return false if the queue is empty.
  [[nodiscard]] bool try_pop(T& out);

  /// Closes the queue: push() and try_push() refuse new values, pop() drains
  /// what remains and then returns false. Idempotent; callable from any
  /// thread. Spinning push()/pop() calls return once they observe the close.
  ///
  /// To guarantee the consumer drains every item, stop the producer before
  /// calling close(): a push racing with close() may enqueue an item after
  /// the consumer has already observed the queue as closed and drained.
  void close();

  /// @return true once close() has been called (advisory snapshot).
  [[nodiscard]] bool closed() const;

  /// @return Current number of queued elements (advisory snapshot).
  [[nodiscard]] std::size_t size() const;

  /// @return Fixed capacity set at construction.
  [[nodiscard]] std::size_t capacity() const;

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

  [[nodiscard]] std::size_t next(std::size_t index) const;

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
