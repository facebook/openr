/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/dispatcher/Dispatcher.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>

#include <folly/Overload.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>

#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

namespace fb303 = facebook::fb303;

namespace openr {
namespace dispatcher {
Dispatcher::Dispatcher(
    messaging::RQueue<KvStorePublication> kvStoreUpdatesQueue) {
  // fiber to process publications from KvStore
  addFiberTask([q = std::move(kvStoreUpdatesQueue), this]() mutable noexcept {
    XLOG(INFO) << "Starting KvStore updates processing fiber";
    while (true) {
      auto maybePub = q.get(); // perform read

      if (maybePub.hasError()) {
        XLOG(INFO) << fmt::format(
            "Terminating KvStore updates processing fiber, error: {}",
            maybePub.error());
        break;
      }

      // push the KvStore publication into the queues for replication/filtering
      kvStorePublicationsQueue_.push(std::move(maybePub).value());
    }
  });
}

void
Dispatcher::stop() {
  // close the producer queue
  kvStorePublicationsQueue_.close();

  // Invoke stop method of super class
  OpenrEventBase::stop();
}

messaging::RQueue<KvStorePublication>
Dispatcher::getReader(const std::vector<std::string>& prefixes) {
  return kvStorePublicationsQueue_.getReader(prefixes);
}

size_t
Dispatcher::getNumWrites() {
  return kvStorePublicationsQueue_.getNumWrites();
}

size_t
Dispatcher::getNumReaders() {
  return kvStorePublicationsQueue_.getNumReaders();
}

std::vector<messaging::RWQueueStats>
Dispatcher::getReplicationStats() {
  return kvStorePublicationsQueue_.getReplicationStats();
}

} // namespace dispatcher
} // namespace openr
