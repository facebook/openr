/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/dispatcher/DispatcherQueue.h>

#include <folly/Overload.h>

#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <memory>

namespace openr {

DispatcherQueue::DispatcherQueue() = default;

DispatcherQueue::~DispatcherQueue() {
  close();
}

bool
DispatcherQueue::push(KvStorePublication&& value) {
  std::vector<std::shared_ptr<std::pair<
      std::shared_ptr<messaging::RWQueue<KvStorePublication>>,
      std::unique_ptr<std::vector<std::string>>>>>
      readers;
  readers.reserve(0);

  auto closed = readers_.withWLock([&](auto& lockedReaders) {
    if (closed_) {
      return true;
    }
    // Copy reader information - and cleans up stale reader
    for (auto it = lockedReaders.begin(); it != lockedReaders.end();) {
      if ((*it)->first.use_count() == 1) {
        (*it)->first->close(); // Close before erasing
        it = lockedReaders.erase(it);
      } else {
        readers.emplace_back(*it); // NOTE: intentionally copying shared_ptr
        ++it;
      }
    }

    return false;
  });

  if (closed) {
    return false;
  }

  // Replicate messages
  if (readers.size()) {
    for (size_t i = 0; i < readers.size(); i++) {
      auto publication = filterKeys(value, *(readers.at(i)->second));

      if (publication) {
        readers.at(i)->first->push(std::move(*publication));
      }
    }
  }
  ++writes_;

  return true;
}

size_t
DispatcherQueue::getNumReaders() {
  auto numReaders = readers_.withWLock([&](auto& lockedReaders) {
    for (auto it = lockedReaders.begin(); it != lockedReaders.end();) {
      if ((*it)->first.use_count() == 1) {
        (*it)->first->close(); // Close before erasing
        it = lockedReaders.erase(it);
      } else {
        ++it;
      }
    }
    return lockedReaders.size();
  });

  return numReaders;
}

/**
 * Get new reader stream of this queue. Stream will get closed automatically
 * when reader is destructed.
 */
messaging::RQueue<KvStorePublication>
DispatcherQueue::getReader(const std::vector<std::string>& filters) {
  auto lockedReaders = readers_.wlock();
  if (closed_) {
    throw std::runtime_error("queue is closed");
  }

  lockedReaders->emplace_back(
      std::make_shared<std::pair<
          std::shared_ptr<messaging::RWQueue<KvStorePublication>>,
          std::unique_ptr<std::vector<std::string>>>>(
          std::make_pair(
              std::make_shared<messaging::RWQueue<KvStorePublication>>(),
              std::make_unique<std::vector<std::string>>(filters))));

  return messaging::RQueue<KvStorePublication>(lockedReaders->back()->first);
}

void
DispatcherQueue::close() {
  auto lockedReaders = readers_.wlock();
  closed_ = true;
  for (auto& pair : *lockedReaders) {
    pair->first->close();
  }
  lockedReaders->clear();
}

size_t
DispatcherQueue::getNumWrites() {
  auto lockedReaders = readers_.wlock();
  return writes_;
}

std::vector<messaging::RWQueueStats>
DispatcherQueue::getReplicationStats() {
  std::vector<messaging::RWQueueStats> stats;
  uint32_t queueCount = 0;
  auto lockedReaders = readers_.wlock();
  for (auto it = lockedReaders->begin(); it != lockedReaders->end();) {
    if ((*it)->first.use_count() == 1) {
      (*it)->first->close(); // Close before erasing
      it = lockedReaders->erase(it);
    } else {
      messaging::RWQueueStats stat = (*it)->first->getStats();
      if (stat.queueId.empty()) {
        stat.queueId = std::to_string(queueCount++);
      }
      stats.push_back(stat);
      ++it;
    }
  }
  return stats;
}

std::optional<KvStorePublication>
DispatcherQueue::filterKeys(
    KvStorePublication& publication, std::vector<std::string>& filters) {
  // avoid filtering for cases where there are no prefixes
  // an empty vector of prefixes means provide all keys to the reader
  if (filters.empty()) {
    return KvStorePublication(publication);
  }

  auto result = folly::variant_match(
      publication,
      [&filters](
          thrift::Publication& pub) -> std::optional<KvStorePublication> {
        // create a empty thrift publication
        auto filteredPublication = thrift::Publication();

        auto& filteredKeyVals = *filteredPublication.keyVals();
        auto& filteredExpiredKeys = *filteredPublication.expiredKeys();

        auto& keyVals = *pub.keyVals();
        for (auto it = keyVals.begin(); it != keyVals.end(); ++it) {
          if (matchPrefix(it->first, filters) && it->second.value()) {
            // add keys that start with the any of the prefixes
            // and keys that have values
            filteredKeyVals.emplace(it->first, it->second);
          }
        }

        auto& expiredKeys = *pub.expiredKeys();
        for (auto it = expiredKeys.begin(); it != expiredKeys.end(); ++it) {
          // remove keys that don't start with the any of the prefixes
          if (matchPrefix(*it, filters)) {
            filteredExpiredKeys.emplace_back(*it);
          }
        }

        // only return the KvStorePublication if filteredExpiredKeys or
        // filteredKeyVals are non-empty
        if (!filteredExpiredKeys.empty() || !filteredKeyVals.empty()) {
          // set the all of the fields if publication should be replicated to
          // reader
          filteredPublication.nodeIds().copy_from(pub.nodeIds());
          filteredPublication.tobeUpdatedKeys().copy_from(
              pub.tobeUpdatedKeys());
          filteredPublication.area().copy_from(pub.area());
          filteredPublication.timestamp_ms().copy_from(pub.timestamp_ms());

          return filteredPublication;
        }
        return std::nullopt;
      },
      [](thrift::InitializationEvent& event)
          -> std::optional<KvStorePublication> {
        // no need to filter keys in InitializationEvent
        return KvStorePublication(event);
      });

  return result;
}

std::unique_ptr<std::vector<std::vector<std::string>>>
DispatcherQueue::getFilters() {
  auto filters = readers_.withWLock([&](auto& lockedReaders) {
    std::vector<std::vector<std::string>> filtersList;
    for (auto it = lockedReaders.begin(); it != lockedReaders.end();) {
      // check for stale readers
      if ((*it)->first.use_count() == 1) {
        (*it)->first->close(); // Close before erasing
        it = lockedReaders.erase(it);
      } else {
        // copy vector of filters for each RW queue
        filtersList.emplace_back(*((*it)->second));
        ++it;
      }
    }

    return filtersList;
  });

  return std::make_unique<std::vector<std::vector<std::string>>>(filters);
}

} // namespace openr
