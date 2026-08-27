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
/// Thread-safety: after construction, all member functions may be called
/// concurrently from any number of producer and consumer threads.
///
/// try_push() and try_pop() return false for "not now" and for "never
/// again" alike, so a non-blocking retry loop needs closed() to terminate;
/// without that test it spins forever after close(). Blocking push()/pop()
/// need no such check — they return false only when retrying is futile.
/// See try_push() for how such a loop must handle its argument.
///
/// Lifetime: the queue must outlive every thread using it — call close()
/// and join all producers/consumers before destruction. Destroying the
/// queue while a thread is blocked in push()/pop() is undefined behavior.
///
/// Exceptions: if T's move assignment throws, the queue's own invariants
/// hold — its indices do not move, so nothing is lost or duplicated and the
/// element count still reconciles. Element *values* are not protected. A
/// failing push()/try_push() enqueues nothing, but a failing pop()/try_pop()
/// leaves both out and the still-queued element in valid-but-unspecified
/// states: retrying the pop yields a hollowed element, not the original.
/// For a T whose move assignment is noexcept the queue is never left in any
/// of these states — but push()/try_push() can still throw while
/// constructing their by-value parameter, before the queue is touched.
///
/// pop()/try_pop() take an out-parameter rather than returning
/// std::optional<T> so a drain loop can reuse one destination's capacity —
/// for T like std::string that is the difference between zero allocations
/// per item and one, inside the region these benchmarks measure.
///
/// @tparam T Element type. Must be DefaultConstructible (ring slots are
///   constructed up front), MoveAssignable, and constructible from whatever
///   each push() call site passes.
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
  /// @return false if the queue is closed (the value is dropped).
  [[nodiscard]] bool push(T value);

  /// Enqueues a value without blocking.
  ///
  /// A retry loop must re-materialise its argument every pass. This is a
  /// by-value sink, so a failed attempt has already consumed an rvalue
  /// argument, and retrying with the same object pushes a moved-from husk,
  /// silently and with no diagnostic:
  ///
  ///     while (!q.try_push(make_value())) {  // NOT try_push(std::move(v))
  ///       if (q.closed()) break;
  ///     }
  ///
  /// That form costs nothing: the prvalue initialises the parameter
  /// directly, so a failed spin constructs one T and copies none.
  /// try_push(v) on an lvalue is also correct and leaves v intact, but
  /// copy-constructs on every spin. What has no correct retry loop is a
  /// move-only value that cannot be re-created — it is unrecoverable after
  /// a failed pass and the API returns only bool. Use blocking push().
  ///
  /// @param value Element to enqueue, taken by value; see push() above.
  /// @return false if the queue is full or closed; closed() tells them apart.
  [[nodiscard]] bool try_push(T value);

  /// Dequeues into out, blocking while the queue is empty and open.
  /// @param[out] out Receives the dequeued element on success; untouched on
  ///   failure, unless T's move assignment throws (see Exceptions above).
  /// @return false once the queue is closed and drained.
  [[nodiscard]] bool pop(T& out);

  /// Dequeues into out without blocking.
  /// @param[out] out Receives the dequeued element on success; untouched on
  ///   failure, unless T's move assignment throws (see Exceptions above).
  /// @return false if the queue is empty, including once it is closed and
  ///   drained; closed() tells them apart.
  [[nodiscard]] bool try_pop(T& out);

  /// Closes the queue and wakes all blocked producers and consumers.
  /// Idempotent. After close(), push() refuses new values; pop() drains
  /// what remains.
  void close();

  /// @return true once close() has been called (advisory snapshot — for
  ///   logging, assertions, and terminating a non-blocking retry loop).
  [[nodiscard]] bool closed() const;

  /// @return Current number of queued elements (advisory snapshot — for
  ///   depth metrics and tests, not for deciding whether to push or pop).
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
