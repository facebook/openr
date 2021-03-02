/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace openr {
namespace messaging {

template <typename ValueType>
RQueue<ValueType>::RQueue(std::shared_ptr<RWQueue<ValueType>> queue)
    : queue_(std::move(queue)) {
  assert(queue_);
}

template <typename ValueType>
folly::Expected<ValueType, QueueError>
RQueue<ValueType>::get() {
  return queue_->get();
}

#if FOLLY_HAS_COROUTINES
template <typename ValueType>
folly::coro::Task<folly::Expected<ValueType, QueueError>>
RQueue<ValueType>::getCoro() {
  auto val = co_await queue_->getCoro();
  co_return val;
}
#endif

template <typename ValueType>
size_t
RQueue<ValueType>::size() {
  return queue_->size();
}

template <typename ValueType>
RWQueue<ValueType>::RWQueue() {}

template <typename ValueType>
RWQueue<ValueType>::~RWQueue() {
  close();
}

template <typename ValueType>
template <typename ValueTypeT>
bool
RWQueue<ValueType>::push(ValueTypeT&& val) {
  std::lock_guard<std::mutex> l(lock_);

  // If queue is closed, don't enqueue
  if (closed_) {
    return false;
  }

  if (pendingReads_.size()) {
    // Unblock a pending read
    auto& pendingRead = pendingReads_.front().get();
    pendingRead.data = std::forward<ValueTypeT>(val);
    pendingRead.baton.post();
    pendingReads_.pop_front();
  } else {
    // Add data into the queue
    queue_.emplace_back(std::forward<ValueTypeT>(val));
  }

  return true;
}

template <typename ValueType>
folly::Expected<ValueType, QueueError>
RWQueue<ValueType>::get() {
  PendingRead pendingRead;

  // Queue is closed
  auto maybeImmediateRead = getAnyImpl(pendingRead);
  if (maybeImmediateRead.hasError()) {
    return folly::makeUnexpected(maybeImmediateRead.error());
  }

  // Post our own baton if read is immediate (for)
  // XXX: This will evenly distribute elements between readers when queue
  // and also ensures fiber-fairness
  if (maybeImmediateRead.value()) {
    CHECK(pendingRead.data);
    pendingRead.baton.post();
  }

  // Wait for baton and read the data
  pendingRead.baton.wait();
  if (pendingRead.data) {
    return std::move(pendingRead.data).value();
  }
  return folly::makeUnexpected(QueueError::QUEUE_CLOSED);
}

#if FOLLY_HAS_COROUTINES
template <typename ValueType>
folly::coro::Task<folly::Expected<ValueType, QueueError>>
RWQueue<ValueType>::getCoro() {
  PendingRead pendingRead;

  // Queue is closed
  auto maybeImmediateRead = getAnyImpl(pendingRead);
  if (maybeImmediateRead.hasError()) {
    co_return folly::makeUnexpected(maybeImmediateRead.error());
  }

  // Wait if there is no data
  if (maybeImmediateRead.value()) {
    CHECK(pendingRead.data);
    pendingRead.baton.post();
  }

  // Wait for baton and read the data
  co_await pendingRead.baton;
  if (pendingRead.data) {
    co_return std::move(pendingRead.data).value();
  }
  co_return folly::makeUnexpected(QueueError::QUEUE_CLOSED);
}
#endif

template <typename ValueType>
folly::Expected<bool, QueueError>
RWQueue<ValueType>::getAnyImpl(PendingRead& pendingRead) {
  std::lock_guard<std::mutex> l(lock_);

  // If queue is closed, return immediately
  if (closed_) {
    return folly::makeUnexpected(QueueError::QUEUE_CLOSED);
  }

  // Perform immediate read if data is available
  if (queue_.size()) {
    pendingRead.data = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  // Else enqueue read request
  pendingReads_.emplace_back(pendingRead);
  return false;
}

template <typename ValueType>
void
RWQueue<ValueType>::close() {
  std::lock_guard<std::mutex> l(lock_);

  if (not closed_) {
    closed_ = true;
    // Either one of these must be zero
    assert(pendingReads_.size() == 0 || queue_.size() == 0);
    // Set empy value to all pending reads
    while (pendingReads_.size()) {
      auto& pendingRead = pendingReads_.front().get();
      pendingRead.baton.post();
      pendingReads_.pop_front();
    }
    queue_.clear();
  }
}

template <typename ValueType>
bool
RWQueue<ValueType>::isClosed() {
  std::lock_guard<std::mutex> l(lock_);
  return closed_;
}

template <typename ValueType>
size_t
RWQueue<ValueType>::size() {
  std::lock_guard<std::mutex> l(lock_);
  return queue_.size();
}

template <typename ValueType>
size_t
RWQueue<ValueType>::numPendingReads() {
  std::lock_guard<std::mutex> l(lock_);
  return pendingReads_.size();
}

} // namespace messaging
} // namespace openr
