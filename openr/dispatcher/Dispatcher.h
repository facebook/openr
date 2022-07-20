/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include <string>

#include <openr/common/OpenrEventBase.h>
#include <openr/common/Types.h>
#include <openr/config/Config.h>
#include <openr/dispatcher/DispatcherQueue.h>

namespace openr {
/**
 * Dispatcher handles filtering keys coming from KvStore
 * and sends them to the other modules for processing.
 * The keys are filtered by a prefix that is provided
 * by the reader.
 *
 * Dispatcher will now subscribe to KvStore and other modules will now become
 * subscribers of Dispatcher
 */

class Dispatcher : public OpenrEventBase {
 public:
  explicit Dispatcher(
      // Reader Queue for receiving KvStore publications
      messaging::RQueue<KvStorePublication> kvStoreUpdatesQueue,
      DispatcherQueue& kvStorePublicationsQueue);
  virtual ~Dispatcher() override = default;

  /**
   * Override stop method of OpenrEventBase
   */
  void stop() override;

  /**
   * non-copyable
   */
  Dispatcher(Dispatcher const&) = delete;
  Dispatcher& operator=(Dispatcher const&) = delete;

  /**
   * Get new reader stream of the Dispatcher object. Stream will get closed
   * automatically when reader is destructed. Initialize filter for each reader
   * with the default prefix
   */
  messaging::RQueue<KvStorePublication> getReader(
      const std::vector<std::string>& prefixes = {});

  /**
   * Number of replicated streams/readers
   */
  size_t getNumReaders();

  /**
   * Number of messages sent on queue before replication
   */
  size_t getNumWrites();

  /**
   * Internal Queue stats for each replicated queue
   */
  std::vector<messaging::RWQueueStats> getReplicationStats();

 private:
  // Queue to publish KvStore Updates
  DispatcherQueue& kvStorePublicationsQueue_;
};

} // namespace openr
