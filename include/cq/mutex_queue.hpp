#ifndef CQ_MUTEX_QUEUE_HPP_
#define CQ_MUTEX_QUEUE_HPP_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

namespace cq {

/// v1 baseline: bounded FIFO ring guarded by a single std::mutex, with
/// not_full / not_empty condition variables and close() shutdown semantics.
///
/// Each operation comes in three waiting disciplines: push()/pop() wait
/// indefinitely, try_push()/try_pop() never wait, and try_push_for()/
/// try_pop_for() wait up to a caller-supplied bound.
///
/// Thread-safety: after construction, all member functions may be called
/// concurrently from any number of producer and consumer threads. closed()
/// and size() return advisory snapshots — drive control flow off the
/// push/pop return values instead, with one exception: try_push()/try_pop()
/// return false for "not now" and for "never again" alike, so a non-blocking
/// retry loop needs closed() to terminate. See try_push() for how such a loop
/// must handle its argument.
///
/// Lifetime: the queue must outlive every thread using it — call close()
/// and join all producers/consumers before destruction. Destroying the
/// queue while a thread is blocked in push()/pop() is undefined behavior.
///
/// Exceptions: if T's move assignment throws, the queue's own invariants hold
/// — its indices do not move, so nothing is lost or duplicated and the element
/// count still reconciles. Element *values* are not protected: a failing
/// push()/try_push() enqueues nothing, but a failing pop()/try_pop() leaves
/// both out and the still-queued element in valid-but-unspecified states, so
/// retrying the pop may yield a hollowed element rather than the original. None
/// of this is reachable for a T whose move assignment is noexcept.
///
/// @tparam T Element type. Must be DefaultConstructible (ring slots are
///   constructed up front) and MoveAssignable.
template <typename T>
class MutexQueue {
 public:
  /// @param capacity Fixed number of ring slots; never resized.
  /// @throws std::invalid_argument if capacity is 0.
  explicit MutexQueue(std::size_t capacity);

  // Not copyable or movable: waiters hold references to mutex_ and the
  // condition variables; share the queue by reference instead.
  MutexQueue(const MutexQueue&) = delete;
  MutexQueue& operator=(const MutexQueue&) = delete;
  MutexQueue(MutexQueue&&) = delete;
  MutexQueue& operator=(MutexQueue&&) = delete;

  /// Enqueues a value, blocking while the queue is full.
  /// @param value Element to enqueue, taken by value. An rvalue argument is
  ///   moved from at the call — including when the push fails, in which case
  ///   the value is discarded. An lvalue argument is copied and left intact.
  /// @return false if the queue is closed. The value is dropped: blocking
  ///   push() narrows the window in which a move-only argument can be lost,
  ///   but does not close it.
  [[nodiscard]] bool push(T value);

  /// Enqueues a value without blocking.
  ///
  /// A retry loop must re-materialise its argument every pass — this is a
  /// by-value sink, so a failed attempt has already consumed an rvalue and
  /// retrying with the same object pushes a moved-from husk. See the README's
  /// "Non-blocking loops" section for the worked idiom and its limits.
  /// @param value Element to enqueue, taken by value. An rvalue argument is
  ///   moved from at the call — including when the push fails, in which case
  ///   the value is discarded. An lvalue argument is copied and left intact.
  /// @return false if the queue is full or closed; closed() tells them apart,
  ///   and a retry loop needs it to terminate.
  [[nodiscard]] bool try_push(T value);

  /// Dequeues into out, blocking while the queue is empty and open.
  /// @param[out] out Receives the dequeued element on success.
  /// @return false once the queue is closed and drained.
  [[nodiscard]] bool pop(T& out);

  /// Dequeues into out without blocking.
  /// @param[out] out Receives the dequeued element on success; untouched on a
  ///   false return; disturbed if T's move assignment throws (see Exceptions).
  /// @return false if the queue is empty.
  [[nodiscard]] bool try_pop(T& out);

  /// Enqueues a value, blocking until a slot frees, the queue closes, or
  /// timeout elapses — the bounded middle ground between push(), which waits
  /// indefinitely, and try_push(), which does not wait at all.
  /// @tparam Rep Arithmetic type of the timeout's tick count.
  /// @tparam Period std::ratio giving the timeout's tick period.
  /// @param value Element to enqueue, taken by value. An rvalue argument is
  ///   moved from at the call — including when the push fails, in which case
  ///   the value is discarded. An lvalue argument is copied and left intact.
  /// @param timeout Longest time to wait. A non-positive timeout makes this
  ///   equivalent to try_push().
  /// @return false if the timeout elapsed with the queue still full, or if
  ///   the queue is closed (the value is dropped). To tell the two apart,
  ///   check closed(): close() is one-way, so a false from closed() after a
  ///   failed call reliably means "timed out, worth retrying".
  template <typename Rep, typename Period>
  [[nodiscard]] bool try_push_for(T value, const std::chrono::duration<Rep, Period>& timeout);

  /// Dequeues into out, blocking until an element arrives, the queue closes
  /// and drains, or timeout elapses.
  /// @tparam Rep Arithmetic type of the timeout's tick count.
  /// @tparam Period std::ratio giving the timeout's tick period.
  /// @param[out] out Receives the dequeued element on success; untouched on a
  ///   false return; disturbed if T's move assignment throws (see Exceptions).
  /// @param timeout Longest time to wait. A non-positive timeout makes this
  ///   equivalent to try_pop().
  /// @return false if the timeout elapsed with the queue still empty, or
  ///   once the queue is closed and drained. As with try_push_for(),
  ///   closed() distinguishes the two.
  template <typename Rep, typename Period>
  [[nodiscard]] bool try_pop_for(T& out, const std::chrono::duration<Rep, Period>& timeout);

  /// Closes the queue and wakes all blocked producers and consumers.
  /// Idempotent. After close(), push() refuses new values; pop() drains
  /// what remains.
  void close();

  /// @return true once close() has been called (advisory snapshot).
  [[nodiscard]] bool closed() const;

  /// @return Current number of queued elements (advisory snapshot).
  [[nodiscard]] std::size_t size() const;

  /// @return Fixed capacity set at construction.
  [[nodiscard]] std::size_t capacity() const noexcept;

 private:
  // The *_locked helpers require mutex_ to be held by the caller.
  void enqueue_locked(T&& value);
  void dequeue_locked(T& out);

  // Wait predicates, shared by the blocking and timed variants so the two
  // cannot drift apart. Both mean "stop waiting", which covers two outcomes
  // the caller still has to separate: the operation can proceed, or the
  // queue closed and the caller must give up.
  [[nodiscard]] bool not_full_or_closed_locked() const;
  [[nodiscard]] bool not_empty_or_closed_locked() const;

  [[nodiscard]] std::size_t next(std::size_t index) const noexcept;

  mutable std::mutex mutex_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::vector<T> buffer_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  std::size_t size_ = 0;
  bool closed_ = false;
};

}  // namespace cq

#include "cq/mutex_queue.ipp"  // IWYU pragma: keep

#endif  // CQ_MUTEX_QUEUE_HPP_
