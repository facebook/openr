/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/messaging/Queue.h>
#include <list>

namespace openr {
namespace messaging {

class ReplicateQueueBase {
 public:
  virtual ~ReplicateQueueBase() = default;

  virtual size_t getNumReaders() = 0;

  virtual size_t getNumWrites() = 0;

  virtual std::vector<RWQueueStats> getReplicationStats() = 0;
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
   */
  RQueue<ValueType> getReader(
      const std::optional<std::string>& readerId = std::nullopt);

  /**
   * Number of replicated streams/readers
   */
  size_t getNumReaders();

  /**
   * Open the underlying queue. ONLY used for UT purpose.
   */
  void
  open() {
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

 private:
  folly::Synchronized<std::list<std::shared_ptr<RWQueue<ValueType>>>> readers_;
  bool closed_{false}; // Protected by above Synchronized lock
  size_t writes_{0};
};

} // namespace messaging
} // namespace openr

#include <openr/messaging/ReplicateQueue-inl.h>
