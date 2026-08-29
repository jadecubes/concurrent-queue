#ifndef CQ_MPMC_QUEUE_HPP_
#define CQ_MPMC_QUEUE_HPP_

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <vector>

#include "cq/backoff.hpp"
#include "cq/cache_line.hpp"

namespace cq {

/// v2.5: bounded FIFO ring for any number of producer and consumer threads,
/// synchronized with atomics only — Dmitry Vyukov's bounded MPMC design,
/// where every slot carries its own sequence counter and the two position
/// counters only hand out tickets. The blocking push()/pop() spin briefly,
/// then sleep with doubling timed backoff — near-zero CPU while blocked,
/// wakeup within kMaxSleep.
///
/// Thread-safety: after construction, all member functions may be called
/// concurrently from any number of threads. closed() and size() return
/// advisory snapshots — drive control flow off the push/pop return values
/// instead, with one exception: try_push()/try_pop() return false for "not
/// now" and for "never again" alike, so a non-blocking retry loop needs
/// closed() to terminate.
///
/// Lifetime: the queue must outlive every thread using it — call close() and
/// join all producers/consumers before destruction. Destroying the queue
/// while a thread is spinning in push()/pop() is undefined behavior.
///
/// Exceptions: a throwing move assignment would strand a claimed ticket whose
/// sequence is never re-published — the consumer can never advance past it and
/// the producer stalls once the ring wraps onto it — with no way to return the
/// ticket. A static_assert therefore requires a noexcept move assignment.
///
/// Arguments: push operations take T by value. An rvalue argument is moved
/// from at the call — even when the push fails, in which case the value is
/// discarded. An lvalue argument is copied and left intact. A try_push()
/// retry loop must therefore re-materialise its argument every pass.
///
/// @tparam T Element type. Must be DefaultConstructible (ring slots are
///   constructed up front) and nothrow-MoveAssignable (see Exceptions).
template <typename T>
// The "excessive padding" the analyzer flags is deliberate: each position
// counter gets a private cache line (see cq/cache_line.hpp).
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class MpmcQueue {
  static_assert(std::is_nothrow_move_assignable_v<T>,
                "MpmcQueue requires a T whose move assignment is noexcept: a throw would "
                "strand a claimed slot and permanently degrade the queue");

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
  /// @param value Element to enqueue; see the class note on by-value arguments.
  /// @return false if the queue is closed; the value is discarded.
  [[nodiscard]] bool push(T value);

  /// Enqueues a value without blocking.
  /// @param value Element to enqueue; see the class note on by-value arguments.
  /// @return false if the queue is full or closed. closed() tells them apart;
  ///   see the README's "Non-blocking loops" for the retry idiom.
  [[nodiscard]] bool try_push(T value);

  /// Dequeues into out, spinning while the queue is empty and open.
  /// @param[out] out Receives the dequeued element on success.
  /// @return false once the queue is closed and drained.
  [[nodiscard]] bool pop(T& out);

  /// Dequeues into out without blocking.
  /// @param[out] out Receives the dequeued element on success; untouched on a
  ///   false return; disturbed if T's move assignment throws (see Exceptions).
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

  // ticket -> slot position: a masked AND when capacity is a power of two,
  // a division otherwise.
  [[nodiscard]] std::size_t slot_index(std::size_t ticket) const noexcept;

  std::vector<Slot> slots_;
  std::size_t mask_;  // slots_.size() - 1 if it is a power of two, else 0
  // Tickets only ever increase; slot_index() maps them into the ring.
  // The counters are on separate lines: producers hammer one, consumers the
  // other, and the slots' own sequence counters carry the actual handoff.
  alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_ = 0;
  alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_ = 0;
  alignas(kCacheLineSize) std::atomic<bool> closed_ = false;
};

}  // namespace cq

#include "cq/mpmc_queue.ipp"  // IWYU pragma: keep

#endif  // CQ_MPMC_QUEUE_HPP_
