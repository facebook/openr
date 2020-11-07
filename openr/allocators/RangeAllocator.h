/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <random>
#include <string>

#include <folly/Format.h>
#include <folly/Optional.h>
#include <folly/Random.h>
#include <folly/gen/Base.h>

#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreClientInternal.h>

namespace openr {

template <typename T = uint32_t>
class RangeAllocator {
 public:
  static_assert(std::is_integral<T>::value, "T is not an integral type");

  /**
   * RangeAllocator is an abstract class to elect a unique value from within
   * the range in a distributed application using KvStore as a communication
   * bus.
   *
   * Idea:
   * - Generate a random value to be claimed
   * - Try electing it via KvStore. Higher originatorId wins.
   * - If we fail we should try again with another random number
   * - To ease up re-tries we use ExponentialBackoff
   *
   * callback: tells you of new allocated value.
   * overrideOwner:  allow a higher originator ID to grab a key from an existing
   * owner with a lower ID knowingly. In some applications like Terragraph, we
   * don't want this to occur so existing allocated values are not stolen by
   * higher priority allocator instances joining later
   */
  RangeAllocator(
      const std::string& nodeName,
      const std::string& keyPrefix,
      KvStoreClientInternal* const kvStoreClient,
      std::function<void(std::optional<T>)> callback,
      const std::chrono::milliseconds minBackoffDur =
          std::chrono::milliseconds(50),
      const std::chrono::milliseconds maxBackoffDur = std::chrono::seconds(2),
      const bool overrideOwner = true,
      const std::function<bool(T)> checkValueInUseCb = nullptr,
      const std::chrono::milliseconds rangeAllocTtl = Constants::kRangeAllocTtl,
      const std::string& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * user must call this to start allocation
   * range and initial value may be unknown during construction
   * allocRange: the range from which to allocate values (range is inclusive)
   * initValue: must be in allocRange
   */
  void startAllocator(
      const std::pair<T /* min */, T /* max */> allocRange,
      const std::optional<T> maybeInitValue);

  /**
   * Default destructor.
   */
  ~RangeAllocator();

  /**
   * Allocated value stored locally if any.
   */
  std::optional<T>
  getValue() const {
    return myValue_;
  }

  // Allocated value stored in kvstore if any
  std::optional<T> getValueFromKvStore() const;

  // check if the whole range has been allocated
  bool isRangeConsumed() const;

 private:
  /**
   * Non-copyable and non-movable
   */
  RangeAllocator(RangeAllocator const&) = delete;
  RangeAllocator& operator=(RangeAllocator const&) = delete;

  // start allocation
  void start(const std::optional<T> maybeInitValue);

  /**
   * Invoked asynchronously to allocate a new value. On success callback
   * will be executed.
   */
  void tryAllocate(const T newVal) noexcept;

  /**
   * Schedule allocation of a new value. A new random value will be chosen
   * based on the seed value.
   */
  void scheduleAllocate(const T seedVal) noexcept;

  /* Invoked whenever there is an update for our currently allocated value
   */
  void keyValUpdated(
      const std::string& key, const thrift::Value& thriftVal) noexcept;

  /**
   * Utility function to create KvStore key for the value.
   */
  std::string createKey(const T val) const noexcept;

  //
  // Immutable state
  //

  const std::string nodeName_;
  const std::string keyPrefix_;

  // KvStoreClientInternal instance used for communicating with KvStore
  KvStoreClientInternal* const kvStoreClient_{nullptr};

  // EventLoop in which KvStoreClientInternal is looping. Used for scheduling
  // asynchronous events.
  OpenrEventBase* const eventBase_{nullptr};

  // Callback function to let user know of newly allocated value
  const std::function<void(std::optional<T>)> callback_{nullptr};

  // allow a higher originator ID to grab a key from an existing owner with a
  // lower ID knowingly
  // Note: even if this is set false, a higher originator can still take over a
  // key accidentally if a lower originator submit the key and it has not
  // propogated to the former yet
  const bool overrideOwner_{true};

  //
  // Mutable state
  //

  // Range from which a value need to be allocated.
  std::pair<T /* min */, T /* max */> allocRange_;

  // Size of range
  T allocRangeSize_;

  // Currently allocated value
  std::optional<T> myValue_;

  // Currently requested value
  std::optional<T> myRequestedValue_;

  // Exponential backoff to avoid frequent allocation retries
  ExponentialBackoff<std::chrono::milliseconds> backoff_;

  // Scheduled timeout token
  std::optional<int64_t> allocateValue_{std::nullopt};
  std::unique_ptr<folly::AsyncTimeout> timeout_;

  // if allocator has started
  bool hasStarted_{false};

  // callback to check if value already exists
  const std::function<bool(T)> checkValueInUseCb_{nullptr};

  // KvStore TTL for value
  const std::chrono::milliseconds rangeAllocTtl_;

  // area ID
  const std::string area_{};
};

} // namespace openr

#define RANGE_ALLOCATOR_H_
#include "RangeAllocator-inl.h"
#undef RANGE_ALLOCATOR_H_
