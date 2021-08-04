/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>

#include <folly/Function.h>
#include <folly/SocketAddress.h>

#include <openr/common/AsyncThrottle.h>
#include <openr/common/Constants.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrClient.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/if/gen-cpp2/Types_constants.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStore.h>

namespace openr {

using namespace std::chrono_literals;

/**
 * This class abstracts out many client side operations of KvStore into very
 * simple APIs to use.
 * 1. Advertise key/value into local KvStore authoritatively :)
 * 2. Access content of KvStore (key-vals, peers)
 *
 * This client also allows you to do complex stuff with KvStore with simple
 * `setKey`, `getKey` operations. With `subscribeKey` you can write your
 * logic in asynchronous fashion.
 *
 */
class KvStoreClientInternal {
 public:
  using KeyCallback = folly::Function<void(
      std::string const&, std::optional<thrift::Value>) noexcept>;

  /**
   * Creates and initializes all necessary sockets for communicating with
   * KvStore.
   */
  KvStoreClientInternal(
      OpenrEventBase* eventBase,
      std::string const& nodeId,
      KvStore* kvStore,
      bool createKvStoreUpdatesReader = true,
      bool useThrottle = false,
      std::optional<std::chrono::milliseconds> checkPersistKeyPeriod = 60000ms);

  ~KvStoreClientInternal();

  /**
   * Stop methods provides a clean way for termination when OpenrEventBase.
   */
  void stop();

  /**
   * Set specified key-value into KvStore. This is an authoratitive call. It
   * means that if someone else advertise the same key we try to win over it
   * by re-advertising KV with higher version.
   * Key will expire and be removed in ttl time after client stops updating,
   * e.g., client process terminates. Client will update TTL every ttl/3 when
   * running.
   * By default key is published to default area kvstore instance.
   *
   * returns true if call results in state change for this client, i.e. we
   * change the value or ttl for the persistented key or start persisting a key
   */
  bool persistKey(
      AreaId const& area,
      std::string const& key,
      std::string const& value,
      std::chrono::milliseconds const ttl = Constants::kTtlInfInterval);

  /**
   * Advertise the key-value into KvStore with specified version. If version is
   * not specified than the one greater than the latest known will be used.
   * Key will expire and be removed in ttl time after client stops updating,
   * e.g., client process terminates. Client will update TTL every ttl/3 when
   * running.
   *
   * Second flavour directly forwards the value to KvStore.
   */
  std::optional<folly::Unit> setKey(
      AreaId const& area,
      std::string const& key,
      std::string const& value,
      uint32_t version = 0,
      std::chrono::milliseconds ttl = Constants::kTtlInfInterval);
  std::optional<folly::Unit> setKey(
      AreaId const& area, std::string const& key, thrift::Value const& value);

  /**
   * Unset key from KvStore. It really doesn't delete the key from KvStore,
   * instead it just leave it as it is.
   */
  void unsetKey(AreaId const& area, std::string const& key);

  /**
   * Clear key's value by seeting default value of empty string or value passed
   * by the caller, cancel ttl timers, advertise with higher version.
   */
  void clearKey(
      AreaId const& area,
      std::string const& key,
      std::string value = "",
      std::chrono::milliseconds ttl = Constants::kTtlInfInterval);

  /**
   * Get key from KvStore. It gets from local snapshot KeyVals of the kvstore.
   */
  std::optional<thrift::Value> getKey(
      AreaId const& area, std::string const& key);

  /**
   * Dump the entries of my KV store whose keys match the given prefix
   * If the prefix is empty string, the full KV store is dumped
   */
  std::optional<std::unordered_map<std::string, thrift::Value>>
  dumpAllWithPrefix(AreaId const& area, const std::string& prefix = "");

  /**
   * APIs to subscribe/unsubscribe to value change of a key in KvStore
   * @param key - key for which callback is registered
   * @param callback - callback API to invoke when key update is received
   * @param fetchInitValue - returns key value from KvStore if set to 'true'
   */
  std::optional<thrift::Value> subscribeKey(
      AreaId const& area,
      std::string const& key,
      KeyCallback callback,
      bool fetchInitValue = false);
  void unsubscribeKey(AreaId const& area, std::string const& key);

  /**
   * API to register callback for given key filter. Subscribing again
   * will overwrite the existing filter
   */
  void subscribeKeyFilter(KvStoreFilters kvFilters, KeyCallback callback);
  void unsubscribeKeyFilter();

  OpenrEventBase*
  getOpenrEventBase() const noexcept {
    return eventBase_;
  }

  /*
   * Counter-exposing related methods
   */

  // templated method to get stats for total number of keys from two-level map:
  //
  //  - T will be containers with key-val store-like structures. E.g.
  //  std::unordered_map/std::map/etc.
  //  - t will be containers which are able to be checked for size. E.g.
  //    - std::unordered_set, std::unordered_map
  //    - std::set, std::map
  //    - std::vector
  //    - etc.
  template <typename T>
  int64_t
  getCount(const T& mp) {
    int64_t count{0};
    for (auto const& [_, t] : mp) {
      count += t.size();
    }
    return count;
  }

  int64_t
  getPersistedKeyCount() {
    return getCount(persistedKeyVals_);
  }

  int64_t
  getCachedKeysToAdvertiseCount() {
    return getCount(keysToAdvertise_);
  }

  int64_t
  getCachedKeysToDeleteCount() {
    return getCount(clearedKeyVals_);
  }

  int64_t
  getKeyCallbackCount() {
    return getCount(keyCallbacks_);
  }

  int64_t
  getBackoffCount() {
    return getCount(backoffs_);
  }

  int64_t
  getKeyTtlBackoffCount() {
    return getCount(keyTtlBackoffs_);
  }

  /**
   * Function to process received publication over SUB channel which are
   * changes of KvStore. It re-advertises the keys with higher version number
   * if need be for persisted keys.
   */
  void processPublication(thrift::Publication const& publication);

 private:
  /**
   * Function to process received expired keys
   */
  void processExpiredKeys(thrift::Publication const& publication);

  /*
   * Utility function to build thrift::Value in KvStoreClientInternal
   * This method will:
   *  1. create ThriftValue based on input param;
   *  2. check if version is specified:
   *    1) YES - return ThriftValue just created;
   *    2) NO - bump up version number to <lastKnownVersion> + 1
   *            <lastKnownVersion> will be checked against KvStore
   */
  thrift::Value buildThriftValue(
      AreaId const& area,
      std::string const& key,
      std::string const& value,
      uint32_t version = 0,
      std::chrono::milliseconds ttl = Constants::kTtlInfInterval);

  /**
   * Utility function to SET keys in KvStore.
   */
  std::optional<folly::Unit> setKeysHelper(
      AreaId const& area,
      std::unordered_map<std::string, thrift::Value> keyVals);

  /**
   * Helper function to advertise the pending keys considering the exponential
   * backoff with one more than the latest version to KvStore. It also
   * schedules the timeout.
   */
  void advertisePendingKeys(
      std::optional<std::unordered_map<AreaId, std::unordered_set<std::string>>>
          pendingKeysToAdvertise = std::nullopt);

  /**
   * Helper function to clear keys with throttled fashion
   * ATTN: should ONLY be triggered by `AsyncThrottle`
   */
  void clearPendingKeys();

  /**
   * Helper function to schedule TTL update advertisement
   */
  void scheduleTtlUpdates(
      AreaId const& area,
      std::string const& key,
      uint32_t version,
      uint32_t ttlVersion,
      int64_t ttl,
      bool advertiseImmediately);

  /**
   * Helper function to advertise TTL update
   */
  void advertiseTtlUpdates();

  void checkPersistKeyInStore();

  /*
   * Wrapper function to initialize timer
   */
  void initTimers();

  //
  // Immutable state
  //

  // Our local node identifier
  const std::string nodeId_{};

  // Knob to enable batched processing of keys
  const bool useThrottle_{false};

  // OpenrEventBase pointer for scheduling async events and socket callback
  // registration
  OpenrEventBase* const eventBase_{nullptr};

  // Pointers to KvStore module
  KvStore* kvStore_{nullptr};

  // periodic timer to check existence of persist key in kv store
  std::optional<std::chrono::milliseconds> checkPersistKeyPeriod_{std::nullopt};

  // check persist key timer event
  std::unique_ptr<folly::AsyncTimeout> checkPersistKeyTimer_;

  // throttled version of `advertisePendingKeys`
  std::unique_ptr<AsyncThrottle> advertisePendingKeysThrottled_;

  // throttled version of `advertisedTtlUpdates`
  std::unique_ptr<AsyncThrottle> advertiseTtlUpdatesThrottled_;

  // throttled version of `clearKey`
  std::unique_ptr<AsyncThrottle> clearPendingKeysThrottled_;

  //
  // Mutable state
  //

  // Locally advertised authorative key-vals using `persistKey`
  std::unordered_map<
      AreaId,
      std::unordered_map<std::string /* key */, thrift::Value>>
      persistedKeyVals_;

  // Locally withdrawn key-vals using `clearKey`
  std::unordered_map<
      AreaId,
      std::unordered_map<std::string /* key */, thrift::Value>>
      clearedKeyVals_;

  // Subscribed keys to their callback functions
  std::unordered_map<
      AreaId,
      std::unordered_map<std::string /* key */, KeyCallback>>
      keyCallbacks_;

  // callback for updates from keys filtered with provided filter
  KeyCallback keyPrefixFilterCallback_{nullptr};

  // backoff associated with each key for re-advertisements
  std::unordered_map<
      AreaId,
      std::unordered_map<
          std::string /* key */,
          ExponentialBackoff<std::chrono::milliseconds>>>
      backoffs_;

  // backoff associated with each key for freshing TTL
  std::unordered_map<
      AreaId,
      std::unordered_map<
          std::string /* key */,
          std::pair<
              thrift::Value /* value */,
              ExponentialBackoff<std::chrono::milliseconds>>>>
      keyTtlBackoffs_;

  // Set of local keys to be re-advertised.
  std::unordered_map<AreaId, std::unordered_set<std::string /* key */>>
      keysToAdvertise_;

  // Timer to advertised pending key-vals
  std::unique_ptr<folly::AsyncTimeout> advertiseKeyValsTimer_;

  // Timer to advertise ttl updates for key-vals
  std::unique_ptr<folly::AsyncTimeout> ttlTimer_;

  // Timer to periodically advertise counters
  std::unique_ptr<folly::AsyncTimeout> counterUpdateTimer_;

  // prefix key filter to apply for key updates
  KvStoreFilters keyPrefixFilter_{{}, {}};

  // fiber task future hold
  folly::Future<folly::Unit> taskFuture_;
};

} // namespace openr
