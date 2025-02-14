/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/dispatcher/Dispatcher.h>

#include <openr/dispatcher/DispatcherQueue.h>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>

#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

namespace fb303 = facebook::fb303;

namespace openr {
Dispatcher::Dispatcher(
    messaging::RQueue<KvStorePublication> kvStoreUpdatesQueue,
    DispatcherQueue& kvStorePublicationsQueue)
    : kvStorePublicationsQueue_(kvStorePublicationsQueue) {
  // fiber to process publications from KvStore
  addFiberTask([q = std::move(kvStoreUpdatesQueue), this]() mutable noexcept {
    XLOG(DBG1) << "Starting kvStore-updates processing task";
    while (true) {
      auto maybePub = q.get(); // perform read

      if (maybePub.hasError()) {
        break;
      }

      // push the KvStore publication into the queues for replication/filtering
      kvStorePublicationsQueue_.push(std::move(maybePub).value());
    }
    XLOG(DBG1) << "[Exit] KvStore-updates processing task finished.";
  });
}

void
Dispatcher::stop() {
  // Invoke stop method of super class
  OpenrEventBase::stop();
  XLOG(DBG1) << "[Exit] Successfully stopped Dispatcher eventbase.";
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

// Dispatcher API
folly::SemiFuture<std::unique_ptr<std::vector<std::vector<std::string>>>>
Dispatcher::getDispatcherFilters() {
  folly::Promise<std::unique_ptr<std::vector<std::vector<std::string>>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable noexcept {
    p.setValue(kvStorePublicationsQueue_.getFilters());
  });
  return sf;
}

} // namespace openr
