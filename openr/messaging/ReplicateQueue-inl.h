/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace openr {
namespace messaging {

template <typename ValueType>
ReplicateQueue<ValueType>::ReplicateQueue() {}

template <typename ValueType>
ReplicateQueue<ValueType>::~ReplicateQueue() {
  close();
}

template <typename ValueType>
template <typename ValueTypeT>
bool
ReplicateQueue<ValueType>::push(ValueTypeT&& value) {
  std::vector<std::shared_ptr<RWQueue<ValueType>>> readers;

  // Copy reader information - and cleans up stale reader
  {
    auto lockedReaders = readers_.wlock();
    if (closed_) {
      return false;
    }
    for (auto it = lockedReaders->begin(); it != lockedReaders->end();) {
      if (it->use_count() == 1) {
        (*it)->close(); // Close before erasing
        it = lockedReaders->erase(it);
      } else {
        readers.emplace_back(*it); // NOTE: intentionally copying shared_ptr
        ++it;
      }
    }
  }

  // Replicate messages
  if (readers.size()) {
    for (size_t i = 0; i < readers.size() - 1; i++) {
      readers.at(i)->push(ValueType(value)); // Intended copy
    }
    // Perfect forwarding for last reader
    readers.back()->push(std::forward<ValueTypeT>(value));
  }

  return true;
}

/**
 * Get new reader stream of this queue. Stream will get closed automatically
 * when reader is destructed.
 */
template <typename ValueType>
RQueue<ValueType>
ReplicateQueue<ValueType>::getReader() {
  auto lockedReaders = readers_.wlock();
  if (closed_) {
    throw std::runtime_error("queue is closed");
  }
  lockedReaders->emplace_back(std::make_shared<RWQueue<ValueType>>());
  return RQueue<ValueType>(lockedReaders->back());
}

template <typename ValueType>
size_t
ReplicateQueue<ValueType>::getNumReaders() {
  auto lockedReaders = readers_.wlock();
  for (auto it = lockedReaders->begin(); it != lockedReaders->end();) {
    if (it->use_count() == 1) {
      (*it)->close(); // Close before erasing
      it = lockedReaders->erase(it);
    } else {
      ++it;
    }
  }
  return lockedReaders->size();
}

template <typename ValueType>
void
ReplicateQueue<ValueType>::close() {
  auto lockedReaders = readers_.wlock();
  closed_ = true;
  for (auto& queue : *lockedReaders) {
    queue->close();
  }
  lockedReaders->clear();
}

} // namespace messaging
} // namespace openr
