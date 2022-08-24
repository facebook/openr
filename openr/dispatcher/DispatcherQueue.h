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
   */
  messaging::RQueue<KvStorePublication> getReader(
      const std::vector<std::string>& prefixes = {});

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

#ifdef DispatcherQueue_TEST_FRIENDS
  DispatcherQueue_TEST_FRIENDS
#endif
};

} // namespace openr
