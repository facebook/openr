/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include "openr/messaging/Queue.h"
namespace openr::messaging {

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
std::string
RQueue<ValueType>::getReaderId() {
  return queue_->queueId();
}

template <typename ValueType>
RWQueue<ValueType>::RWQueue() = default;

template <typename ValueType>
RWQueue<ValueType>::RWQueue(const std::string& queueId) : queueId_(queueId) {}

template <typename ValueType>
RWQueue<ValueType>::RWQueue(
    const std::string& queueId,
    std::function<bool(ValueType&, ValueType&)> coalesceFn)
    : queueId_(queueId), coalesceFn_(std::move(coalesceFn)) {}

template <typename ValueType>
RWQueue<ValueType>::RWQueue(
    const std::string& queueId,
    StateSuppressionPolicy<ValueType> stateSuppressionPolicy)
    : queueId_(queueId),
      stateSuppressionQueue_(
          std::make_unique<StateSuppressionQueue>(
              std::move(stateSuppressionPolicy), nowFn_)) {}

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

  if (stateSuppressionQueue_ && stateSuppressionQueue_->shouldDrop(val)) {
    ++writes_;
    ++suppressions_;
    return true;
  }

  /*
   * Handed directly to a waiting reader: the message never waits in the
   * backlog, so its queue dwell time is zero and no timing is recorded. The
   * read side still counts it in reads_, which keeps the average honest.
   */
  if (pendingReads_.size()) {
    // Unblock a pending read
    auto& pendingRead = pendingReads_.front().get();
    pendingRead.data.emplace(std::forward<ValueTypeT>(val));
    pendingRead.baton.post();
    pendingReads_.pop_front();
  } else if (stateSuppressionQueue_) {
    if (stateSuppressionQueue_->push(std::forward<ValueTypeT>(val))) {
      ++suppressions_;
    }
  } else if (coalesceFn_ && !queue_.empty()) {
    /*
     * Offer the incoming value to be merged into the pending tail element. If
     * the coalescer consumes it (returns true) nothing is appended, bounding
     * the backlog even when the reader is slow/stalled; otherwise append it.
     * A consumed value keeps the surviving tail's enqueue time, so no clock
     * read happens on that path.
     */
    ValueType incoming(std::forward<ValueTypeT>(val));
    if (!coalesceFn_(queue_.back().value, incoming)) {
      queue_.emplace_back(std::move(incoming), nowFn_());
    } else {
      ++suppressions_;
    }
  } else {
    // Add data into the queue
    queue_.emplace_back(std::forward<ValueTypeT>(val), nowFn_());
  }
  ++writes_;

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

  /*
   * Post our own baton if read is immediate (for)
   * XXX: This will evenly distribute elements between readers when queue
   * and also ensures fiber-fairness
   */
  if (maybeImmediateRead.value()) {
    CHECK(pendingRead.data);
    pendingRead.baton.post();
  }

  // Wait for baton and read the data
  pendingRead.baton.wait();
  if (pendingRead.data) {
    ++reads_;
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
    ++reads_;
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

  /*
   * The clock is read only on paths that actually pop a message. When the
   * backlog is empty the reader just parks on pendingReads_, so no timestamp
   * is needed.
   */
  if (stateSuppressionQueue_ && !stateSuppressionQueue_->empty()) {
    const auto now = nowFn_();
    auto [value, enqueueTime] = stateSuppressionQueue_->pop();
    recordQueueDwellTime(enqueueTime, now);
    pendingRead.data.emplace(std::move(value));
    return true;
  }

  if (!queue_.empty()) {
    const auto now = nowFn_();
    auto entry = std::move(queue_.front());
    queue_.pop_front();
    recordQueueDwellTime(entry.enqueueTime, now);
    pendingRead.data.emplace(std::move(entry.value));
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
    assert(
        pendingReads_.empty() ||
        (queue_.empty() &&
         (!stateSuppressionQueue_ || stateSuppressionQueue_->empty())));
    // Set empy value to all pending reads
    while (pendingReads_.size()) {
      auto& pendingRead = pendingReads_.front().get();
      pendingRead.baton.post();
      pendingReads_.pop_front();
    }
    queue_.clear();
    if (stateSuppressionQueue_) {
      stateSuppressionQueue_->clear();
    }
  }
}

template <typename ValueType>
bool
RWQueue<ValueType>::isClosed() {
  std::lock_guard<std::mutex> l(lock_);
  return closed_;
}

template <typename ValueType>
std::string
RWQueue<ValueType>::getQueueId() {
  return queueId_;
}

template <typename ValueType>
size_t
RWQueue<ValueType>::size() {
  std::lock_guard<std::mutex> l(lock_);
  return stateSuppressionQueue_ ? stateSuppressionQueue_->size()
                                : queue_.size();
}

template <typename ValueType>
size_t
RWQueue<ValueType>::numPendingReads() {
  std::lock_guard<std::mutex> l(lock_);
  return pendingReads_.size();
}

template <typename ValueType>
size_t
RWQueue<ValueType>::numWrites() {
  std::lock_guard<std::mutex> l(lock_);
  return writes_;
}

template <typename ValueType>
size_t
RWQueue<ValueType>::numReads() {
  std::lock_guard<std::mutex> l(lock_);
  return reads_;
}

template <typename ValueType>
void
RWQueue<ValueType>::recordQueueDwellTime(
    std::chrono::steady_clock::time_point enqueueTime,
    std::chrono::steady_clock::time_point now) {
  const auto dwellUs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now - enqueueTime)
          .count());
  totalQueuedTimeUs_ += dwellUs;
  if (dwellUs > maxQueuedTimeUs_) {
    maxQueuedTimeUs_ = dwellUs;
  }
}

template <typename ValueType>
void
RWQueue<ValueType>::setNowFn(NowFn nowFn) {
  std::lock_guard<std::mutex> l(lock_);
  nowFn_ = nowFn;
  if (stateSuppressionQueue_) {
    stateSuppressionQueue_->setNowFn(nowFn);
  }
}

template <typename ValueType>
RWQueueStats
RWQueue<ValueType>::getStats() {
  std::lock_guard<std::mutex> l(lock_);
  /*
   * The ms conversion and averaging happen here on the poll path (Watchdog
   * tick), never per message: the hot path accumulates integer microseconds
   * only. Stamping integer milliseconds at record time would erase sub-ms
   * dwells (0.9ms would read as 0), so precision is kept until this point.
   */
  const double avgMs =
      reads_ ? static_cast<double>(totalQueuedTimeUs_) / reads_ / 1000.0 : 0.0;
  const double maxMs = static_cast<double>(maxQueuedTimeUs_) / 1000.0;
  return RWQueueStats{
      "",
      reads_,
      writes_,
      stateSuppressionQueue_ ? stateSuppressionQueue_->size() : queue_.size(),
      suppressions_,
      avgMs,
      maxMs};
}

} // namespace openr::messaging
