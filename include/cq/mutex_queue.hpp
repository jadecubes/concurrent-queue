#ifndef CQ_MUTEX_QUEUE_HPP_
#define CQ_MUTEX_QUEUE_HPP_

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

namespace cq {

/// v1 baseline: bounded FIFO ring guarded by a single std::mutex, with
/// not_full / not_empty condition variables and close() shutdown semantics.
///
/// - push()/pop() block; try_push()/try_pop() never block.
/// - close() shuts the queue down; see close() for the full contract.
///
/// Notifications are unconditional (fired even when no thread waits) —
/// deliberate v1 simplicity; the benchmarks measure that cost as part of
/// the baseline.
///
/// Thread-safety: after construction, all member functions may be called
/// concurrently from any number of producer and consumer threads.
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

  // Not copyable or movable: blocked producers/consumers hold references to
  // mutex_ and the condition variables, so the queue needs a stable address.
  // Share it by reference (or shared_ptr) instead.
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

  /// Closes the queue and wakes all blocked producers and consumers.
  /// Idempotent. After close(), push() refuses new values; pop() drains
  /// what remains.
  void close();

  /// @return true once close() has been called. Advisory snapshot: may be
  ///   stale by the time it returns; do not build control flow on it — use
  ///   the return values of push()/pop() instead.
  [[nodiscard]] bool closed() const;

  /// @return Current number of queued elements. Advisory snapshot: may be
  ///   stale by the time it returns; meant for monitoring and tests, not
  ///   for emptiness/fullness decisions — use try_push()/try_pop().
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
