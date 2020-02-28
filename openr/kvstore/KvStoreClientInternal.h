/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

#include <folly/Function.h>
#include <folly/Optional.h>
#include <folly/SocketAddress.h>

#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrClient.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStore.h>

namespace openr {

using namespace std::chrono_literals;

/**
 * This class abstracts out many client side operations of KvStore into very
 * simple APIs to use.
 * 1. Advertise key/value into local KvStore authoratitively :)
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
      std::string const&, folly::Optional<thrift::Value>) noexcept>;

  /**
   * Creates and initializes all necessary sockets for communicating with
   * KvStore.
   */
  KvStoreClientInternal(
      fbzmq::Context& context,
      OpenrEventBase* eventBase,
      std::string const& nodeId,
      std::string const& kvStoreLocalCmdUrl,
      std::string const& kvStoreLocalPubUrl,
      KvStore* kvStore,
      folly::Optional<std::chrono::milliseconds> checkPersistKeyPeriod =
          60000ms,
      folly::Optional<std::chrono::milliseconds> recvTimeout = 3000ms);

  /*
   * Second flavor of KvStoreClientInternal to talk to KvStore through Open/R
   * ctrl Thrift port.
   */
  KvStoreClientInternal(
      fbzmq::Context& context,
      OpenrEventBase* eventBase,
      std::string const& nodeId,
      folly::SocketAddress const& socketAddr);

  ~KvStoreClientInternal();

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
      std::string const& key,
      std::string const& value,
      std::chrono::milliseconds const ttl = Constants::kTtlInfInterval,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * Advertise the key-value into KvStore with specified version. If version is
   * not specified than the one greater than the latest known will be used.
   * Key will expire and be removed in ttl time after client stops updating,
   * e.g., client process terminates. Client will update TTL every ttl/3 when
   * running.
   *
   * Second flavour directly forwards the value to KvStore.
   *
   * Return error type:
   *     1. zmq socket error
   *     2. server error returned by KvStore
   */
  folly::Expected<folly::Unit, fbzmq::Error> setKey(
      std::string const& key,
      std::string const& value,
      uint32_t version = 0,
      std::chrono::milliseconds ttl = Constants::kTtlInfInterval,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());
  folly::Expected<folly::Unit, fbzmq::Error> setKey(
      std::string const& key,
      thrift::Value const& value,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * Unset key from KvStore. It really doesn't delete the key from KvStore,
   * instead it just leave it as it is.
   */
  void unsetKey(
      std::string const& key,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * Clear key's value by seeting default value of empty string or value passed
   * by the caller, cancel ttl timers, advertise with higher version.
   */
  void clearKey(
      std::string const& key,
      std::string value = "",
      std::chrono::milliseconds ttl = Constants::kTtlInfInterval,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * Get key from KvStore. It gets from local snapshot KeyVals of the kvstore.
   * Return error type:
   *    1. zmq socket error
   *    2. "key not found" error upon non-existing key.
   */
  folly::Expected<thrift::Value, fbzmq::Error> getKey(
      std::string const& key,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * Dump the entries of my KV store whose keys match the given prefix
   * If the prefix is empty string, the full KV store is dumped
   */
  folly::Expected<
      std::unordered_map<std::string /* key */, thrift::Value /* value */>,
      fbzmq::Error>
  dumpAllWithPrefix(
      const std::string& prefix = "",
      const std::string& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * APIs to subscribe/unsubscribe to value change of a key in KvStore
   * @param key - key for which callback is registered
   * @param callback - callback API to invoke when key update is received
   * @param fetchInitValue - returns key value from KvStore if set to 'true'
   */
  folly::Optional<thrift::Value> subscribeKey(
      std::string const& key,
      KeyCallback callback,
      bool fetchInitValue = false,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());
  void unsubscribeKey(std::string const& key);

  // Set callback for all kv publications
  void setKvCallback(KeyCallback callback);

  /**
   * API to register callback for given key filter. Subscribing again
   * will overwrite the existing filter
   */
  void subscribeKeyFilter(KvStoreFilters kvFilters, KeyCallback callback);
  void unSubscribeKeyFilter();

  /**
   * APIs to send Add/Del peer command to KvStore.
   * Return error type:
   *     1. zmq socket error
   *     2. server error returned by KvStore
   */
  folly::Expected<folly::Unit, fbzmq::Error> addPeers(
      std::unordered_map<std::string, thrift::PeerSpec> peers,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());
  folly::Expected<folly::Unit, fbzmq::Error> delPeer(
      std::string const& peerName,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());
  folly::Expected<folly::Unit, fbzmq::Error> delPeers(
      const std::vector<std::string>& peerNames,
      const std::string& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * APIs to send PEER_DUMP command to KvStore.
   * return error type:
   *      1. zme socket error
   *      2. server error returned by KvStore
   */
  folly::Expected<
      std::unordered_map<std::string, thrift::PeerSpec>,
      fbzmq::Error>
  getPeers(const std::string& area = thrift::KvStore_constants::kDefaultArea());

  OpenrEventBase*
  getOpenrEventBase() const noexcept {
    return eventBase_;
  }

 private:
  /**
   * Process timeout is called when timeout expires.
   */
  void processTimeout();

  /**
   * Function to process received publication over SUB channel which are
   * changes of KvStore. It re-advertises the keys with higher version number
   * if need be for persisted keys.
   */
  void processPublication(thrift::Publication const& publication);

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
      std::string const& key,
      std::string const& value,
      uint32_t version = 0,
      std::chrono::milliseconds ttl = Constants::kTtlInfInterval,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * Utility function to SET keys in KvStore. Will throw an exception if things
   * goes wrong.
   */
  folly::Expected<folly::Unit, fbzmq::Error> setKeysHelper(
      std::unordered_map<std::string, thrift::Value> keyVals,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * Utility function to del peers in KvStore
   * return error type:
   *    1. zmq socket error
   *    2. server error returned by KvStore
   */
  folly::Expected<folly::Unit, fbzmq::Error> delPeersHelper(
      const std::vector<std::string>& peerNames,
      const std::string& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * Helper function to advertise the pending keys considering the exponential
   * backoff with one more than the latest version to KvStore. It also
   * schedules the timeout.
   */
  void advertisePendingKeys();

  /**
   * Helper function to schedule TTL update advertisement
   */
  void scheduleTtlUpdates(
      std::string const& key,
      uint32_t version,
      uint32_t ttlVersion,
      int64_t ttl,
      bool advertiseImmediately,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * Helper function to advertise TTL update
   */
  void advertiseTtlUpdates();

  /**
   * Helper to do full dumps
   */
  static folly::Expected<thrift::Publication, fbzmq::Error> dumpImpl(
      fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>& sock,
      apache::thrift::CompactSerializer& serializer,
      std::string const& prefix,
      folly::Optional<std::chrono::milliseconds> recvTimeout,
      std::string const& area = thrift::KvStore_constants::kDefaultArea());

  /**
   * Create KvStoreCmd socket on the fly. REQ-REP sockets needs reset if
   * previously issued request fails and response hasn't been received as
   * REQ socket will enter into exceptional state.
   */
  void prepareKvStoreCmdSock() noexcept;

  void checkPersistKeyInStore();

  /*
   * Util function to instantiate openrCtrlClient
   */
  void initOpenrCtrlClient();

  /*
   * Wrapper function to initialize timer
   */
  void initTimers();

  //
  // Immutable state
  //

  // boolean var indicating use thrift or NOT
  const bool useThriftClient_{false};

  // SocketAddress used to connect to openrCtrlClient
  const folly::SocketAddress sockAddr_{};

  // EventBase to create openrCtrl client
  folly::EventBase evb_;

  // cached OpenrCtrlClient to talk to Open/R instance
  std::unique_ptr<thrift::OpenrCtrlCppAsyncClient> openrCtrlClient_{nullptr};

  // Our local node identifier
  const std::string nodeId_;

  // OpenrEventBase pointer for scheduling async events and socket callback
  // registration
  OpenrEventBase* const eventBase_{nullptr};

  // ZMQ context for IO processing
  fbzmq::Context& context_;

  // Socket Urls (we assume local, unencrypted connection)
  const std::string kvStoreLocalCmdUrl_{""};
  const std::string kvStoreLocalPubUrl_{""};

  // Pointers to KvStore module
  KvStore* kvStore_{nullptr};

  // periodic timer to check existence of persist key in kv store
  folly::Optional<std::chrono::milliseconds> checkPersistKeyPeriod_{
      folly::none};

  // check persiste key timer event
  std::unique_ptr<fbzmq::ZmqTimeout> checkPersistKeyTimer_;

  // Recv Timeout to be used from from KvStore
  folly::Optional<std::chrono::milliseconds> recvTimeout_;

  //
  // Mutable state
  //

  // REQ socket for KvStore API calls
  std::unique_ptr<fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>> kvStoreCmdSock_;

  // SUB socket for listing to kvstore client.
  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> kvStoreSubSock_;

  // Locally advertised authorative key-vals using `persistKey`
  std::unordered_map<
      std::string /* area */,
      std::unordered_map<std::string /* key */, thrift::Value>>
      persistedKeyVals_;

  // Subscribed keys to their callback functions
  std::unordered_map<std::string, KeyCallback> keyCallbacks_;

  // callback for every key published
  KeyCallback kvCallback_{nullptr};

  // callback for updates from keys filtered with provided filter
  KeyCallback keyPrefixFilterCallback_{nullptr};

  // backoff associated with each key for re-advertisements
  std::unordered_map<
      std::string /* key */,
      ExponentialBackoff<std::chrono::milliseconds>>
      backoffs_;

  // backoff associated with each key for freshing TTL
  std::unordered_map<
      std::string /* area */,
      std::unordered_map<
          std::string /* key */,
          std::pair<
              thrift::Value /* value */,
              ExponentialBackoff<std::chrono::milliseconds>>>>
      keyTtlBackoffs_;

  // Set of local keys to be re-advertised.
  std::unordered_map<
      std::string /* area */,
      std::unordered_set<std::string /* key */>>
      keysToAdvertise_;

  // Serializer object for thrift-obj <-> string conversion
  apache::thrift::CompactSerializer serializer_;

  // Timer to advertised pending key-vals
  std::unique_ptr<fbzmq::ZmqTimeout> advertiseKeyValsTimer_;

  // Timer to advertise ttl updates for key-vals
  std::unique_ptr<fbzmq::ZmqTimeout> ttlTimer_;

  // prefix key filter to apply for key updates
  KvStoreFilters keyPrefixFilter_{{}, {}};
};

} // namespace openr
