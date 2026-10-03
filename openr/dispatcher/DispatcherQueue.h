/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <list>

#include <openr/common/Types.h>
#include <openr/messaging/Queue.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {

class DispatcherQueue : public messaging::ReplicateQueueBase {
 public:
  DispatcherQueue();
  virtual ~DispatcherQueue() override;

  /**
   * non-copyable
   */
  DispatcherQueue(DispatcherQueue const&) = delete;
  DispatcherQueue& operator=(DispatcherQueue const&) = delete;

  /**
   * movable
   */
  DispatcherQueue(DispatcherQueue&&) = default;
  DispatcherQueue& operator=(DispatcherQueue&&) = default;

  /**
   * Push any value into the queue. Will get replicated to the reader based off
   * given filter from the reader.
   * This also cleans up any lingering queue which has no active reader
   */
  bool push(KvStorePublication&& value);

  /**
   * Get new reader stream of this queue. Stream will get closed automatically
   * when reader is destructed. If the vector of prefixes is empty that means
   * there will be no filtering by prefix, and the reader will get every key
   * from Dispatcher. A prefix will be the start of any key coming from KvStore.
   *
   * `readerId` names the reader's underlying RWQueue and surfaces in
   * per-reader telemetry (getReplicationStats). Production readers must pass
   * an explicit functional ID (stable across restarts by construction);
   * unnamed readers fall back to a positional index (T98477650).
   *
   * With `suppressionPolicy` set, this reader keeps at most one pending element
   * per state key instead of an unbounded FIFO, which bounds the backlog even
   * when the reader is slow or stalled. Classification runs AFTER prefix
   * filtering, so the policy only ever sees keys this reader subscribes to.
   * Only affects THIS reader.
   *
   * NOTE: the policy's callbacks run under the reader queue's lock.
   * dispatcherQueue has a single producer -- the Dispatcher fiber -- so there
   * is no race between producers, but that same fiber feeds every OTHER
   * reader, so an expensive classifier or merge delays delivery to all of them.
   */
  messaging::RQueue<KvStorePublication> getReader(
      const std::vector<std::string>& prefixes = {},
      const std::string& readerId = "",
      std::optional<messaging::StateSuppressionPolicy<KvStorePublication>>
          suppressionPolicy = std::nullopt);

  /**
   * Number of replicated streams/readers
   */
  size_t getNumReaders() override;

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
  std::vector<messaging::RWQueueStats> getReplicationStats() override;

  /**
   * Names the queue for per-reader dwell telemetry (see
   * messaging::ReplicateQueueBase); (re)binds every reader's sample sink.
   */
  void setQueueName(const std::string& name) override;

  /**
   * DispatcherQueue API to get all of the filters for
   * each of the internal RW queues
   */
  std::unique_ptr<std::vector<std::vector<std::string>>> getFilters();

 private:
  /**
   * Filter all keys for the publicaton that don't start with any of the
   * provided prefixes. Only return the publication if the keyVals is not empty
   * or the expiredKeys field is not empty. Ex: prefixes = {adj}, keys =
   * {adj:10, prefix:1, adj:3, prefix:adj:5, adjacent} -> returned keys to
   * reader would be {adj:10, adj:3, adjacent}
   */
  std::optional<KvStorePublication> filterKeys(
      KvStorePublication& publication, std::vector<std::string>& prefixes);

  folly::Synchronized<std::list<std::shared_ptr<std::pair<
      std::shared_ptr<messaging::RWQueue<KvStorePublication>>,
      std::unique_ptr<std::vector<std::string>>>>>>
      readers_;
  bool closed_{false}; // Protected by above Synchronized lock
  size_t writes_{0};
  // Telemetry name assigned by Watchdog registration; empty until then.
  std::string queueName_;

#ifdef DispatcherQueue_TEST_FRIENDS
  DispatcherQueue_TEST_FRIENDS
#endif
};

} // namespace openr
