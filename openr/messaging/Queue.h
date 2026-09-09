/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <any>
#include <atomic>
#include <deque>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <folly/Expected.h>
#include <folly/container/F14Map.h>
#include <folly/fibers/Baton.h>
#if FOLLY_HAS_COROUTINES
#include <folly/coro/Task.h>
#endif

namespace openr::messaging {

enum class QueueError {
  QUEUE_CLOSED,
};

enum class StateSuppressionAction {
  /* Replace an older pending value with the same state key. */
  REPLACE_PENDING,
  /* Retain this value and reset suppression history for its key. */
  KEY_BARRIER,
};

/**
 * Describes how a queued value participates in keyed state suppression.
 *
 * Keys are scoped to one reader. The queue does not interpret their contents;
 * the caller must provide a collision-free encoding of its logical identity.
 */
struct StateSuppressionKey {
  std::string key;
  StateSuppressionAction action{StateSuppressionAction::REPLACE_PENDING};
};

/**
 * Explicit policy wrapper for constructing a keyed state-suppression reader.
 *
 * Keeping this distinct from the tail-coalescing callback makes the two queue
 * modes mutually exclusive and leaves `getReader("id", nullptr)` unambiguous.
 */
template <typename ValueType>
struct StateSuppressionPolicy {
  std::function<StateSuppressionKey(const ValueType&)> classify;
  /*
   * Preserve FIFO behavior while the pending depth is at or below this value.
   * A value of zero enables suppression as soon as data is queued.
   */
  size_t activationThreshold{0};
};

// Stats recording of
struct RWQueueStats {
  std::string queueId; // TODO: Change to const post T98477650
  const size_t reads{0};
  const size_t writes{0};
  const size_t size{0};
};

template <typename ValueType>
class RWQueue;

/**
 * Read-only interface for RWQueue class.
 */
template <typename ValueType>
class RQueue {
 public:
  explicit RQueue(std::shared_ptr<RWQueue<ValueType>> queue);
  virtual ~RQueue() = default;

  /**
   * Blocking read for native threads/fibers. In-case of fibers, the fiber
   * performing blocking read will be suspended.
   */
  folly::Expected<ValueType, QueueError> get();

#if FOLLY_HAS_COROUTINES
  /**
   * Read methods for co-routines
   */
  folly::coro::Task<folly::Expected<ValueType, QueueError>> getCoro();
#endif

  // Utility function to retrieve size of pending data in underlying queue
  size_t size();

  // Utility function to obtain name of the queue
  std::string getReaderId();

 protected:
  // We only hold reference of above queue
  std::shared_ptr<RWQueue<ValueType>> queue_{nullptr};
};

/**
 * Multiple writers and readers. We use lock internally to protect the data.
 * Code in critical path is minimal and ensures that readers/writers will never
 * block each other because of lock.
 *
 *There are various get (blocking and async) methods to retrieve typed object.
 *
 * After closing queue, all subsequent push are ignored and return false. All
 * subsequent reads return QUEUE_CLOSED error
 */
template <typename ValueType>
class RWQueue {
 public:
  RWQueue();
  explicit RWQueue(const std::string&);
  /**
   * Construct with an optional push-time coalescer. When set and the queue is
   * non-empty at push time, the incoming value is offered to merge into the
   * current tail element (existing) instead of being appended; if the coalescer
   * returns true it consumed `incoming` (nothing is appended), bounding the
   * queue depth for eventually-consistent consumers even when the reader is
   * slow/stalled. Returns false to append as usual (e.g. for non-mergeable
   * element types/boundaries).
   *
   * NOTE: `coalesceFn` is invoked while the queue's internal lock is held, so
   * it must be cheap/bounded -- an expensive coalescer serializes concurrent
   * producers. Intended for single-producer, latest-state-wins readers.
   */
  RWQueue(
      const std::string& queueId,
      std::function<bool(ValueType& existing, ValueType& incoming)> coalesceFn);
  /**
   * Construct with keyed state suppression. At most one pending suppressible
   * value is retained for each key. A replacement is moved to the tail so the
   * surviving values retain their production order. A key barrier is appended
   * and prevents later values for that key from replacing earlier state.
   *
   * Suppression begins only after the pending depth exceeds
   * `activationThreshold`. The existing FIFO backlog is classified and
   * compacted at activation. Suppression remains active until the queue drains.
   * The classifier runs under the queue lock and must be cheap.
   */
  RWQueue(
      const std::string& queueId,
      StateSuppressionPolicy<ValueType> stateSuppressionPolicy);
  ~RWQueue();

  /**
   * Non blocking push. Any typed value can be pushed!
   * Return true/false!!
   */
  template <typename ValueTypeT>
  bool push(ValueTypeT&& val);

  /**
   * Blocking read for native threads/fibers. In-case of fibers, the fiber
   * performing blocking read will be suspended.
   */
  folly::Expected<ValueType, QueueError> get();

#if FOLLY_HAS_COROUTINES
  /**
   * Read methods for co-routines
   */
  folly::coro::Task<folly::Expected<ValueType, QueueError>> getCoro();
#endif

  /**
   * Close the queue. All new push will be ignored and pending data will be lost
   */
  void close();
  bool isClosed();

  /**
   * Get the queue id (name)
   */
  std::string getQueueId();

  /**
   * Return size of the current queue (number of data elements)
   */
  size_t size();

  /**
   * Return number of active reads
   */
  size_t numPendingReads();

  /**
   * Return the number of messages written to the queue
   */
  size_t numWrites();

  /**
   * Return the number of messages processed by readers
   */
  size_t numReads();

  /**
   * Package and return the individual queue stats.
   */
  RWQueueStats getStats();

 private:
  // Name/id of the queue
  std::string queueId_;

  struct PendingRead {
    folly::fibers::Baton baton;
    std::optional<ValueType> data;
  };

  class StateSuppressionQueue {
   public:
    explicit StateSuppressionQueue(
        StateSuppressionPolicy<ValueType> stateSuppressionPolicy)
        : activationThreshold_(stateSuppressionPolicy.activationThreshold),
          suppressionActive_(activationThreshold_ == 0),
          classifyState_(std::move(stateSuppressionPolicy.classify)) {
      CHECK(classifyState_);
    }

    template <typename ValueTypeT>
    void
    push(ValueTypeT&& val) {
      ValueType incoming(std::forward<ValueTypeT>(val));
      if (!suppressionActive_) {
        queue_.emplace_back(PendingState{std::nullopt, std::move(incoming)});
        if (queue_.size() > activationThreshold_) {
          activateSuppression();
        }
        return;
      }

      queue_.emplace_back(
          PendingState{classifyState_(incoming), std::move(incoming)});
      indexPendingState(std::prev(queue_.end()));
    }

    ValueType
    pop() {
      auto stateIt = queue_.begin();
      if (suppressionActive_) {
        CHECK(stateIt->stateKey.has_value());
        const auto& stateKey = *stateIt->stateKey;
        /*
         * A barrier may have removed this mapping while leaving the value in
         * the list. Erase only when the index still names this value.
         */
        if (stateKey.action == StateSuppressionAction::REPLACE_PENDING) {
          if (auto it = pendingStateByKey_.find(std::string_view{stateKey.key});
              it != pendingStateByKey_.end() && it->second == stateIt) {
            pendingStateByKey_.erase(it);
          }
        }
      }
      auto value = std::move(stateIt->value);
      queue_.erase(stateIt);
      if (queue_.empty() && activationThreshold_ != 0) {
        pendingStateByKey_.clear();
        suppressionActive_ = false;
      }
      return value;
    }

    bool
    empty() const {
      return queue_.empty();
    }

    size_t
    size() const {
      return queue_.size();
    }

    void
    clear() {
      pendingStateByKey_.clear();
      queue_.clear();
      suppressionActive_ = activationThreshold_ == 0;
    }

   private:
    struct PendingState {
      std::optional<StateSuppressionKey> stateKey;
      ValueType value;
    };

    using StateIterator = typename std::list<PendingState>::iterator;

    void
    activateSuppression() {
      CHECK(!suppressionActive_);
      for (auto stateIt = queue_.begin(); stateIt != queue_.end(); ++stateIt) {
        stateIt->stateKey.emplace(classifyState_(stateIt->value));
        indexPendingState(stateIt);
      }
      suppressionActive_ = true;
    }

    void
    indexPendingState(StateIterator stateIt) {
      CHECK(stateIt->stateKey.has_value());
      const auto& stateKey = *stateIt->stateKey;
      if (stateKey.action == StateSuppressionAction::KEY_BARRIER) {
        pendingStateByKey_.erase(std::string_view{stateKey.key});
        return;
      }

      /*
       * Erase and append rather than overwrite in place. This preserves the
       * production order of surviving state: A1, B1, A2 becomes B1, A2.
       */
      if (auto it = pendingStateByKey_.find(std::string_view{stateKey.key});
          it != pendingStateByKey_.end()) {
        const auto previousStateIt = it->second;
        pendingStateByKey_.erase(it);
        queue_.erase(previousStateIt);
      }
      CHECK(pendingStateByKey_.emplace(std::string_view{stateKey.key}, stateIt)
                .second);
    }

    std::list<PendingState> queue_;
    folly::
        F14FastMap<std::string_view, typename std::list<PendingState>::iterator>
            pendingStateByKey_;
    size_t activationThreshold_{0};
    bool suppressionActive_{true};
    std::function<StateSuppressionKey(const ValueType&)> classifyState_;
  };

  /**
   * Implementation for reading a pending or future data element.
   *
   * @returns true/false indicating if immediate read is performed
   * @returns QUEUE_CLOSED error if queue is closed.
   */
  folly::Expected<bool, QueueError> getAnyImpl(PendingRead& pendingRead);

  // Lock to protect below private variables
  std::mutex lock_;

  // State of queue
  bool closed_{false};

  // Pending reads - readers are actively waiting for data
  std::deque<std::reference_wrapper<PendingRead>> pendingReads_;

  // Existing FIFO storage for ordinary and tail-coalescing readers.
  std::deque<ValueType> queue_;

  // Optional push-time coalescer (see constructor). Set once at construction;
  // nullptr means normal append behavior.
  std::function<bool(ValueType& existing, ValueType& incoming)> coalesceFn_{
      nullptr};

  /*
   * Allocated only for readers that explicitly enable keyed state suppression.
   * Ordinary and tail-coalescing readers continue through the existing queue_
   * and coalesceFn_ path.
   */
  std::unique_ptr<StateSuppressionQueue> stateSuppressionQueue_;

  // Sent messages
  size_t writes_{0};

  // Received messages
  std::atomic<size_t> reads_{0};
};

} // namespace openr::messaging

#include <openr/messaging/Queue-inl.h>
