// Member function definitions for cq::MutexQueue. Included at the bottom of
// mutex_queue.hpp — templates must be visible at every instantiation point,
// so this file cannot be compiled as a standalone translation unit.
#ifndef CQ_MUTEX_QUEUE_IPP_
#define CQ_MUTEX_QUEUE_IPP_

#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace cq {

template <typename T>
MutexQueue<T>::MutexQueue(std::size_t capacity) : buffer_(capacity) {
  if (capacity == 0) {
    throw std::invalid_argument("MutexQueue capacity must be > 0");
  }
}

template <typename T>
bool MutexQueue<T>::push(T value) {
  {
    std::unique_lock lock(mutex_);
    not_full_.wait(lock, [&] { return closed_ || size_ < buffer_.size(); });
    if (closed_) {
      return false;
    }
    enqueue_locked(std::move(value));
  }
  not_empty_.notify_one();
  return true;
}

template <typename T>
bool MutexQueue<T>::try_push(T value) {
  {
    const std::lock_guard lock(mutex_);
    if (closed_ || size_ == buffer_.size()) {
      return false;
    }
    enqueue_locked(std::move(value));
  }
  not_empty_.notify_one();
  return true;
}

template <typename T>
bool MutexQueue<T>::pop(T& out) {
  {
    std::unique_lock lock(mutex_);
    not_empty_.wait(lock, [&] { return closed_ || size_ > 0; });
    if (size_ == 0) {
      return false;  // closed and drained
    }
    dequeue_locked(out);
  }
  not_full_.notify_one();
  return true;
}

template <typename T>
bool MutexQueue<T>::try_pop(T& out) {
  {
    const std::lock_guard lock(mutex_);
    if (size_ == 0) {
      return false;
    }
    dequeue_locked(out);
  }
  not_full_.notify_one();
  return true;
}

template <typename T>
void MutexQueue<T>::close() {
  {
    const std::lock_guard lock(mutex_);
    closed_ = true;
  }
  not_full_.notify_all();
  not_empty_.notify_all();
}

template <typename T>
bool MutexQueue<T>::closed() const {
  const std::lock_guard lock(mutex_);
  return closed_;
}

template <typename T>
std::size_t MutexQueue<T>::size() const {
  const std::lock_guard lock(mutex_);
  return size_;
}

// buffer_ is never resized after construction, so no lock is needed.
template <typename T>
std::size_t MutexQueue<T>::capacity() const {
  return buffer_.size();
}

template <typename T>
void MutexQueue<T>::enqueue_locked(T&& value) {
  buffer_[tail_] = std::move(value);
  tail_ = next(tail_);
  ++size_;
}

template <typename T>
void MutexQueue<T>::dequeue_locked(T& out) {
  out = std::move(buffer_[head_]);
  head_ = next(head_);
  --size_;
}

template <typename T>
std::size_t MutexQueue<T>::next(std::size_t index) const {
  return index + 1 == buffer_.size() ? 0 : index + 1;
}

}  // namespace cq

#endif  // CQ_MUTEX_QUEUE_IPP_
