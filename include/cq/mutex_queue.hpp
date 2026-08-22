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
/// push/pop return values instead.
///
/// Lifetime: the queue must outlive every thread using it — call close()
/// and join all producers/consumers before destruction. Destroying the
/// queue while a thread is blocked in push()/pop() is undefined behavior.
///
/// Exceptions: if T's move assignment throws, the failing push()/try_push()
/// enqueues nothing and the failing pop()/try_pop() leaves the element
/// queued — the queue itself stays consistent.
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
  /// @param value Element to enqueue; consumed even when the push fails.
  /// @return false if the queue is closed (the value is dropped).
  [[nodiscard]] bool push(T value);

  /// Enqueues a value without blocking.
  /// @param value Element to enqueue; consumed even when the push fails.
  /// @return false if the queue is full or closed.
  [[nodiscard]] bool try_push(T value);

  /// Dequeues into out, blocking while the queue is empty and open.
  /// @param[out] out Receives the dequeued element on success.
  /// @return false once the queue is closed and drained.
  [[nodiscard]] bool pop(T& out);

  /// Dequeues into out without blocking.
  /// @param[out] out Receives the dequeued element on success; untouched on
  ///   failure.
  /// @return false if the queue is empty.
  [[nodiscard]] bool try_pop(T& out);

  /// Enqueues a value, blocking until a slot frees, the queue closes, or
  /// timeout elapses — the bounded middle ground between push(), which waits
  /// indefinitely, and try_push(), which does not wait at all.
  /// @tparam Rep Arithmetic type of the timeout's tick count.
  /// @tparam Period std::ratio giving the timeout's tick period.
  /// @param value Element to enqueue; consumed even when the push fails.
  /// @param timeout Longest time to wait. A non-positive timeout makes this
  ///   equivalent to try_push().
  /// @return false if the timeout elapsed with the queue still full, or if
  ///   the queue is closed (the value is dropped).
  template <typename Rep, typename Period>
  [[nodiscard]] bool try_push_for(T value, const std::chrono::duration<Rep, Period>& timeout);

  /// Dequeues into out, blocking until an element arrives, the queue closes
  /// and drains, or timeout elapses.
  /// @tparam Rep Arithmetic type of the timeout's tick count.
  /// @tparam Period std::ratio giving the timeout's tick period.
  /// @param[out] out Receives the dequeued element on success; untouched on
  ///   failure.
  /// @param timeout Longest time to wait. A non-positive timeout makes this
  ///   equivalent to try_pop().
  /// @return false if the timeout elapsed with the queue still empty, or
  ///   once the queue is closed and drained.
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
  [[nodiscard]] std::size_t capacity() const;

 private:
  // The *_locked helpers require mutex_ to be held by the caller.
  void enqueue_locked(T&& value);
  void dequeue_locked(T& out);

  [[nodiscard]] std::size_t next(std::size_t index) const;

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
