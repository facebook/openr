/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <list>

#include <openr/messaging/Queue.h>

namespace openr::messaging {

class ReplicateQueueBase {
 public:
  virtual ~ReplicateQueueBase() = default;

  virtual size_t getNumReaders() = 0;

  virtual size_t getNumWrites() = 0;

  virtual std::vector<RWQueueStats> getReplicationStats() = 0;

  /*
   * Names the queue for per-reader telemetry keying
   * (`messaging.rw_queue.<name>-<reader>`). Called by Watchdog registration;
   * (re)binds every reader's dwell-sample sink so readers created before
   * registration are keyed correctly.
   */
  virtual void setQueueName(const std::string& name) = 0;
};

/**
 * Multiple writers and readers. Each reader gets every written element push by
 * every writer. Writer pays the cost of replicating data to all readers. If no
 * reader exists then all the messages are silently dropped.
 *
 * Pushed object must be copy constructible.
 */
template <typename ValueType>
class ReplicateQueue : public ReplicateQueueBase {
 public:
  /*
   * NOTE: there is no queue-level auto-ID scheme. Every production reader
   * passes an explicit functional ID at getReader time (see T98477650);
   * unnamed (test) readers fall back to a positional index in
   * getReplicationStats, as before. The queue-level name used for telemetry
   * keying is assigned separately via setQueueName (Watchdog registration).
   */
  ReplicateQueue();

  ~ReplicateQueue();

  /**
   * non-copyable
   */
  ReplicateQueue(ReplicateQueue const&) = delete;
  ReplicateQueue& operator=(ReplicateQueue const&) = delete;

  /**
   * movable
   */
  ReplicateQueue(ReplicateQueue&&) = default;
  ReplicateQueue& operator=(ReplicateQueue&&) = default;

  /**
   * Push any value into the queue. Will get replicated to all the readers.
   * This also cleans up any lingering queue which has no active reader
   */
  template <typename ValueTypeT>
  bool push(ValueTypeT&& value);

  /**
   * Get new reader stream of this queue. Stream will get closed automatically
   * when reader is destructed.
   *
   * `readerId` names the reader's RWQueue and surfaces in per-reader
   * telemetry (getReplicationStats). Production readers must pass an explicit
   * functional ID (stable across restarts by construction); unnamed readers
   * fall back to a positional index (T98477650).
   *
   * If `coalesceFn` is provided, this reader's backlog is coalesced at push
   * time (see RWQueue constructor): a newly-pushed value is offered to merge
   * into the reader's pending tail element instead of being appended. Use this
   * for eventually-consistent, latest-state-wins consumers to bound the
   * reader's backlog even when it is slow/stalled. Only affects THIS reader.
   * `coalesceFn` runs under the reader queue's lock, so it must be cheap (see
   * RWQueue constructor).
   */
  RQueue<ValueType> getReader(
      const std::optional<std::string>& readerId = std::nullopt,
      std::function<bool(ValueType& existing, ValueType& incoming)> coalesceFn =
          nullptr);

  /**
   * Get a reader that retains only the latest pending suppressible state for
   * each key. Key barriers remain queued and split suppression history only
   * for their own key. Suppression starts after the policy's activation
   * threshold is exceeded and remains active until the reader drains. State
   * suppression is isolated to this reader. `readerId` names the reader (see
   * above).
   */
  RQueue<ValueType> getReader(
      const std::optional<std::string>& readerId,
      StateSuppressionPolicy<ValueType> stateSuppressionPolicy);

  /**
   * Number of replicated streams/readers
   */
  size_t getNumReaders();

  /**
   * Open the underlying queue. ONLY used for UT purpose.
   */
  void
  open() {
    auto lockedReaders = readers_.wlock();
    closed_ = false;
  }

  /**
   * Close the underlying queue. All subsequent writes and reads will fails.
   */
  void close();

  /**
   * Number of messages sent on queue before replication
   */
  size_t getNumWrites() override;

  /**
   * Queue stats for each replicated queue
   */
  std::vector<RWQueueStats> getReplicationStats() override;

  /**
   * Names the queue for per-reader dwell telemetry (see ReplicateQueueBase).
   */
  void setQueueName(const std::string& name) override;

 private:
  folly::Synchronized<std::list<std::shared_ptr<RWQueue<ValueType>>>> readers_;
  bool closed_{false}; // Protected by above Synchronized lock
  size_t writes_{0};
  // Telemetry name assigned by Watchdog registration; empty until then.
  std::string queueName_;
};

} // namespace openr::messaging

#include <openr/messaging/ReplicateQueue-inl.h>
