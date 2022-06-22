/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/dispatcher/DispatcherQueue.h>

#include <folly/Overload.h>

#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

namespace openr {
namespace dispatcher {

DispatcherQueue::DispatcherQueue() {}

DispatcherQueue::~DispatcherQueue() {
  close();
}

bool
DispatcherQueue::push(KvStorePublication&& value) {
  std::vector<std::shared_ptr<std::pair<
      std::shared_ptr<messaging::RWQueue<KvStorePublication>>,
      std::unique_ptr<re2::RE2>>>>
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
    for (size_t i = 0; i < readers.size() - 1; i++) {
      auto publication = KvStorePublication(value); // Intended copy

      if (filterKeys(publication, *(readers.at(i)->second))) {
        readers.at(i)->first->push(std::move(publication));
      }
    }
    // Perfect forwarding for last reader
    if (filterKeys(value, *(readers.back()->second))) {
      readers.back()->first->push(std::forward<KvStorePublication>(value));
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
DispatcherQueue::getReader(const std::string& filter) {
  auto lockedReaders = readers_.wlock();
  if (closed_) {
    throw std::runtime_error("queue is closed");
  }

  lockedReaders->emplace_back(
      std::make_shared<std::pair<
          std::shared_ptr<messaging::RWQueue<KvStorePublication>>,
          std::unique_ptr<re2::RE2>>>(
          std::make_pair(
              std::make_shared<messaging::RWQueue<KvStorePublication>>(),
              std::make_unique<re2::RE2>(filter))));

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

bool
DispatcherQueue::filterKeys(KvStorePublication& publication, re2::RE2& filter) {
  // avoid filtering for cases where regex is .*
  if (filter.pattern() == ".*") {
    return true;
  }

  auto result = folly::variant_match(
      publication,
      [&filter](thrift::Publication& pub) {
        auto& keyVals = *pub.keyVals();
        for (auto it = keyVals.begin(); it != keyVals.end();) {
          if (not re2::RE2::FullMatch(it->first, filter) or
              (not it->second.value())) {
            // remove keys that don't match the regex
            // or keys that don't have values
            it = keyVals.erase(it);
          } else {
            ++it;
          }
        }
        return not keyVals.empty();
      },
      [](thrift::InitializationEvent&) {
        // no need to filter keys in InitializationEvent
        return true;
      });

  return result;
}

} // namespace dispatcher

} // namespace openr
