/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Function.h>

#include <openr/common/AsyncThrottle.h>
#include <openr/common/Constants.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/kvstore/KvStore.h>

namespace openr {

/**
 * This KvStoreClientInternal class provides the client-side APIs to allow
 * user to interact with KvStore within the eventbase. This includes:
 *
 *  - GET/SET/UNSET/DUMP keys;
 *  - SUBSCRIBE/UNSUBSCRIBE keys with your own logic in ASYNC fashion;
 */
class KvStoreClientInternal {
 public:
  using KeyCallback = folly::Function<void(
      std::string const&, std::optional<thrift::Value>) noexcept>;

  KvStoreClientInternal(
      OpenrEventBase* eventBase, std::string const& nodeId, KvStore* kvStore);

  ~KvStoreClientInternal();

  // Provide a clean way of termination when OpenrEventBase exits.
  void stop();

  /**
   * [Key Management - Basic]
   *
   * [SET KEY]:
   *
   * Advertise the key-value into KvStore with specified version and key will be
   * refreshed with ttl-updating interval.
   *
   * There are two flavors of setKey() API:
   *
   *  1) Version and ttl are explicitly provided. If the version if NOT
   *     specified, then the one greater than the latest known will be used.
   *  2) thrift::Value is explicitly provided;
   *
   * [UNSET KEY]:
   *
   * Stop key ttl-refreshing.
   * ATTN: KvStore doesn't support explicit key deletion.
   *
   * [GET KEY]:
   *
   * Get the corresponding value for one key from KvStore.
   *
   * [DUMP KEY]:
   *
   * Dump the entries from KvStore whose keys match the given prefix.
   * ATTN: If the prefix is empty string, the full KV store is dumped.
   */
  std::optional<folly::Unit> setKey(
      AreaId const& area,
      std::string const& key,
      std::string const& value,
      uint32_t version = 0,
      std::chrono::milliseconds ttl = Constants::kTtlInfInterval);

  std::optional<folly::Unit> setKey(
      AreaId const& area, std::string const& key, thrift::Value const& value);

  void unsetKey(AreaId const& area, std::string const& key);

  std::optional<thrift::Value> getKey(
      AreaId const& area, std::string const& key);

  std::optional<std::unordered_map<std::string, thrift::Value>>
  dumpAllWithPrefix(AreaId const& area, const std::string& prefix = "");

  /**
   * [Key Management - Advanced]
   *
   * [SUBSCRIBE/UNSUBSCRIBE KEY]:
   *
   * APIs to subscribe/unsubscribe to value change of a key in KvStore.
   * ATTN: the callback function is attached for a per-key basis.
   *
   * [SUBSCRIBE/UNSUBSCRIBE KEY FILTER]:
   *
   * APIs to subscribe/unsubscribe to value change for a given key filter.
   * ATTN: the callback function can be ONLY used once. Subscribing again
   * will overwrite the existing filter.
   */
  std::optional<thrift::Value> subscribeKey(
      AreaId const& area,
      std::string const& key,
      KeyCallback callback,
      bool fetchInitValue = false);
  void unsubscribeKey(AreaId const& area, std::string const& key);

  void subscribeKeyFilter(KvStoreFilters kvFilters, KeyCallback callback);
  void unsubscribeKeyFilter();

  OpenrEventBase*
  getOpenrEventBase() const noexcept {
    return eventBase_;
  }

 private:
  /**
   * Function to process received expired keys
   */
  void processExpiredKeys(thrift::Publication const& publication);

  /**
   * Function to process received publications for changes of KvStore.
   */
  void processPublication(thrift::Publication const& publication);

  /**
   * Utility function to SET keys in KvStore.
   */
  std::optional<folly::Unit> setKeysHelper(
      AreaId const& area,
      std::unordered_map<std::string, thrift::Value> keyVals);

  /**
   * [TTL Management]
   *
   *  - helper function to schedule TTL update advertisement
   *  - helper function to advertise TTL updates
   */
  void scheduleTtlUpdates(
      AreaId const& area,
      std::string const& key,
      uint32_t version,
      uint32_t ttlVersion,
      int64_t ttl,
      bool advertiseImmediately);

  void advertiseTtlUpdates();

  /**
   * Immutable state
   */

  // Our local node identifier
  const std::string nodeId_{};

  // OpenrEventBase pointer for scheduling async events and socket callback
  // registration
  OpenrEventBase* const eventBase_{nullptr};

  // Pointers to KvStore module
  KvStore* kvStore_{nullptr};

  /**
   * Mutable state
   */

  // throttled version of `advertisedTtlUpdates`
  std::unique_ptr<AsyncThrottle> advertiseTtlUpdatesThrottled_;

  // backoff associated with each key for freshing TTL
  std::unordered_map<
      AreaId,
      std::unordered_map<
          std::string /* key */,
          std::pair<
              thrift::Value /* value */,
              ExponentialBackoff<std::chrono::milliseconds>>>>
      keyTtlBackoffs_;

  // Timer to advertise ttl updates for key-vals
  std::unique_ptr<folly::AsyncTimeout> ttlTimer_;

  // Subscribed keys to their callback functions
  std::unordered_map<
      AreaId,
      std::unordered_map<std::string /* key */, KeyCallback>>
      keyCallbacks_;

  // callback for updates from keys filtered with provided filter
  KeyCallback keyPrefixFilterCallback_{nullptr};

  // prefix key filter to apply for key updates
  KvStoreFilters keyPrefixFilter_{{}, {}};

  // fiber task future hold
  folly::Future<folly::Unit> taskFuture_;
};

} // namespace openr
