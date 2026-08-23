#ifndef CQ_MPMC_QUEUE_HPP_
#define CQ_MPMC_QUEUE_HPP_

#include <atomic>
#include <cstddef>
#include <vector>

#include "cq/cache_line.hpp"

namespace cq {

/// v2.5: bounded FIFO ring for any number of producer and consumer threads,
/// synchronized with atomics only — Dmitry Vyukov's bounded MPMC design,
/// where every slot carries its own sequence counter and the two position
/// counters only hand out tickets. The blocking push()/pop() spin with
/// std::this_thread::yield() instead of sleeping.
///
/// Thread-safety: after construction, all member functions may be called
/// concurrently from any number of threads. closed() and size() return
/// advisory snapshots — drive control flow off the push/pop return values
/// instead.
///
/// Lifetime: the queue must outlive every thread using it — call close() and
/// join all producers/consumers before destruction. Destroying the queue
/// while a thread is spinning in push()/pop() is undefined behavior.
///
/// Exceptions: if T's move assignment throws while a slot is claimed, that
/// slot's sequence is never re-published and the queue degrades (later
/// operations on the slot spin); unlike the locked queue there is no way to
/// return a claimed ticket. Use element types whose move assignment cannot
/// throw.
///
/// @tparam T Element type. Must be DefaultConstructible (ring slots are
///   constructed up front) and MoveAssignable.
template <typename T>
// The "excessive padding" the analyzer flags is deliberate: each position
// counter gets a private cache line (see kCacheLineSize).
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class MpmcQueue {
 public:
  /// @param capacity Fixed number of slots; never resized.
  /// @throws std::invalid_argument if capacity is 0.
  explicit MpmcQueue(std::size_t capacity);

  // Not copyable or movable: every thread holds references to the same ring;
  // share the queue by reference instead.
  MpmcQueue(const MpmcQueue&) = delete;
  MpmcQueue& operator=(const MpmcQueue&) = delete;
  MpmcQueue(MpmcQueue&&) = delete;
  MpmcQueue& operator=(MpmcQueue&&) = delete;

  /// Enqueues a value, spinning while the queue is full.
  /// @param value Element to enqueue; consumed even when the push fails.
  /// @return false if the queue is closed (the value is dropped).
  [[nodiscard]] bool push(T value);

  /// Enqueues a value without blocking.
  /// @param value Element to enqueue; consumed even when the push fails.
  /// @return false if the queue is full or closed.
  [[nodiscard]] bool try_push(T value);

  /// Dequeues into out, spinning while the queue is empty and open.
  /// @param[out] out Receives the dequeued element on success.
  /// @return false once the queue is closed and drained.
  [[nodiscard]] bool pop(T& out);

  /// Dequeues into out without blocking.
  /// @param[out] out Receives the dequeued element on success; untouched on
  ///   failure.
  /// @return false if the queue is empty (including transiently, while a
  ///   producer has claimed the next slot but not yet published it).
  [[nodiscard]] bool try_pop(T& out);

  /// Closes the queue: push() and try_push() refuse new values, pop() drains
  /// what remains and then returns false. Idempotent; callable from any
  /// thread. Spinning push()/pop() calls return once they observe the close.
  ///
  /// To guarantee the consumers drain every item, close() must be called by a
  /// thread that has synchronized with all producers (typically after joining
  /// them): a push racing with close() may land after a consumer has already
  /// observed the queue as closed and drained.
  void close() noexcept;

  /// @return true once close() has been called (advisory snapshot).
  [[nodiscard]] bool closed() const noexcept;

  /// @return Current number of queued elements (advisory snapshot).
  [[nodiscard]] std::size_t size() const noexcept;

  /// @return Fixed capacity set at construction.
  [[nodiscard]] std::size_t capacity() const noexcept;

 private:
  // One ring slot. sequence encodes the slot's state relative to the lap
  // (doubled tickets — see the protocol comment in the .ipp):
  // == 2*ticket      -> free for the producer holding that ticket
  // == 2*ticket + 1  -> holds data for the consumer holding that ticket
  // anything smaller -> the previous lap has not finished with it yet
  struct Slot {
    std::atomic<std::size_t> sequence;
    T value;
  };

  // The claim-then-publish step for one side. Both return false when the ring
  // is full/empty (or transiently looks it); on success the argument is moved
  // exactly once, so the callers may retry with the same argument.
  [[nodiscard]] bool try_enqueue(T& value);
  [[nodiscard]] bool try_dequeue(T& out);

  [[nodiscard]] static std::vector<Slot> make_slots(std::size_t capacity);

  std::vector<Slot> slots_;
  // Tickets only ever increase; a slot is addressed by ticket % slots_.size().
  // The counters are on separate lines: producers hammer one, consumers the
  // other, and the slots' own sequence counters carry the actual handoff.
  alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_ = 0;
  alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_ = 0;
  alignas(kCacheLineSize) std::atomic<bool> closed_ = false;
};

}  // namespace cq

#include "cq/mpmc_queue.ipp"  // IWYU pragma: keep

#endif  // CQ_MPMC_QUEUE_HPP_
