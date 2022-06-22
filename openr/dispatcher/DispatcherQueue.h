/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <list>

#include <re2/re2.h>

#include <openr/common/Types.h>
#include <openr/messaging/Queue.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {
namespace dispatcher {

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
   * when reader is destructed.
   */
  messaging::RQueue<KvStorePublication> getReader(
      const std::string& filter = ".*");

  /**
   * Number of replicated streams/readers with a given regex
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

 private:
  /**
   * Filter all keys for the publicaton that don't match the provided filter
   */
  bool filterKeys(KvStorePublication& publication, re2::RE2& filter);

  folly::Synchronized<std::list<std::shared_ptr<std::pair<
      std::shared_ptr<messaging::RWQueue<KvStorePublication>>,
      std::unique_ptr<re2::RE2>>>>>
      readers_;
  bool closed_{false}; // Protected by above Synchronized lock
  size_t writes_{0};

#ifdef DispatcherQueue_TEST_FRIENDS
  DispatcherQueue_TEST_FRIENDS
#endif
};

} // namespace dispatcher
} // namespace openr
