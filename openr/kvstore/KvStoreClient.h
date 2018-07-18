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

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

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
class KvStoreClient {
 public:
  using KeyCallback =
      folly::Function<void(std::string const&,
                folly::Optional<thrift::Value>) noexcept>;

  /**
   * Creates and initializes all necessary sockets for communicating with
   * KvStore.
   */
  KvStoreClient(
      fbzmq::Context& context,
      fbzmq::ZmqEventLoop* eventLoop,
      std::string const& nodeId,
      std::string const& kvStoreLocalCmdUrl,
      std::string const& kvStoreLocalPubUrl,
      folly::Optional<std::chrono::milliseconds>
                      checkPersistKeyPeriod = 60000ms,
      folly::Optional<std::chrono::milliseconds> recvTimeout = 3000ms);

  ~KvStoreClient();

  /**
   * Set specified key-value into KvStore. This is an authoratitive call. It
   * means that if someone else advertise the same key we try to win over it
   * by re-advertising KV with higher version.
   * Key will expire and be removed in ttl time after client stops updating,
   * e.g., client process terminates. Client will update TTL every ttl/3 when
   * running.
   */
  void persistKey(
      std::string const& key,
      std::string const& value,
      std::chrono::milliseconds ttl = Constants::kTtlInfInterval);

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
      std::chrono::milliseconds ttl = Constants::kTtlInfInterval);
  folly::Expected<folly::Unit, fbzmq::Error> setKey(
      std::string const& key, thrift::Value const& value);

  /**
   * Unset key from KvStore. It really doesn't delete the key from KvStore,
   * instead it just leave it as it is.
   */
  void unsetKey(std::string const& key);

  /**
   * Get key from KvStore. It gets from local snapshot KeyVals of the kvstore.
   * Return error type:
   *    1. zmq socket error
   *    2. "key not found" error upon non-existing key.
   */
  folly::Expected<thrift::Value, fbzmq::Error> getKey(std::string const& key);

  /**
   * Dump the entries of my KV store whose keys match the given prefix
   * If the prefix is empty string, the full KV store is dumped
   */
  folly::Expected<
      std::unordered_map<std::string /* key */, thrift::Value /* value */>,
      fbzmq::Error>
  dumpAllWithPrefix(const std::string& prefix = "");

  // helper for deserialization
  template <typename ThriftType>
  static ThriftType parseThriftValue(thrift::Value const& value);

  /**
   * Given map of thrift::Value object parse them into map of ThriftType
   * objects,
   * while retaining the versioning information
   */
  template <typename ThriftType>
  static std::unordered_map<std::string, ThriftType> parseThriftValues(
      std::unordered_map<std::string, thrift::Value> const& keyVals);

  /**
   * This is a one-shot connect and dump method. It allows for simple way
   * of obtaining full dump in "stateless" fashion - without maintaining any
   * client. This version connects to multiple stores, and merges values
   * from all of them, performing conflict resolution. The client will see
   * the finall merged thrift::Values
   *
   * @param context - usual ZMQ context stuff
   * @param kvStoreCmdUrls - the URL for the KvStore's CMD socket
   * @param prefix - the key prefix to use for key dumping; empty dumps all
   * @param recvTimeout - how to wait on receive operations
   *
   * @return first member of the pair is key-value map obtained by merging data
   * from all stores. Null value if failed connecting and obtaining snapshot
   * from ALL stores. If at least one store responds this will be non-empty.
   * Second member is a list of unreached kvstore urls
   */

  static std::pair<
      folly::Optional<std::unordered_map<std::string /* key */, thrift::Value>>,
      std::vector<fbzmq::SocketUrl> /* unreached url */>
  dumpAllWithPrefixMultiple(
      fbzmq::Context& context,
      const std::vector<fbzmq::SocketUrl>& kvStoreCmdUrls,
      const std::string& prefix,
      folly::Optional<std::chrono::milliseconds> recvTimeout = folly::none,
      folly::Optional<int> maybeIpTos = folly::none);

  /**
   * Similar to the above but parses the values according to the ThrifType
   * passed. This will hide the version/originator & other details
   *
   * @param ThriftType - decode values as this thrift type. this is handy when
   *   you dump keys with the same prefix (which we do)
   *
   * @param context - usual ZMQ context stuff
   * @param kvStoreCmdUrls - the URL for the KvStore's CMD socket
   * @param prefix - the key prefix to use for key dumping; empty dumps all
   * @param recvTimeout - how to wait on receive operations
   *
   * @return first member of the pair is key-value map obtained by merging data
   * from all stores. Null value if failed connecting and obtaining snapshot
   * from ALL stores. If at least one store responds this will be non-empty.
   * Second member is a list of unreached kvstore urls
   *
   */
  template <typename ThriftType>
  static std::pair<
      folly::Optional<std::unordered_map<std::string /* key */, ThriftType>>,
      std::vector<fbzmq::SocketUrl> /* unreached url */>
  dumpAllWithPrefixMultipleAndParse(
      fbzmq::Context& context,
      const std::vector<fbzmq::SocketUrl>& kvStoreCmdUrls,
      const std::string& prefix,
      folly::Optional<std::chrono::milliseconds> recvTimeout = folly::none,
      folly::Optional<int> maybeIpTos = folly::none);

  /**
   * APIs to subscribe/unsubscribe to value change of a key in KvStore
   * @param key - key for which callback is registered
   * @param callback - callback API to invoke when key update is received
   * @param fetchInitValue - returns key value from KvStore if set to 'true'
   */
  folly::Optional<thrift::Value> subscribeKey(std::string const& key,
          KeyCallback callback, bool fetchInitValue = false);
  void unsubscribeKey(std::string const& key);

  // Set callback for all kv publications
  void setKvCallback(KeyCallback callback);

  /**
   * APIs to send Add/Del peer command to KvStore.
   * Return error type:
   *     1. zmq socket error
   *     2. server error returned by KvStore
   */
  folly::Expected<folly::Unit, fbzmq::Error> addPeers(
      std::unordered_map<std::string, thrift::PeerSpec> peers);
  folly::Expected<folly::Unit, fbzmq::Error> delPeer(
      std::string const& peerName);
  folly::Expected<folly::Unit, fbzmq::Error> delPeers(
      const std::vector<std::string>& peerNames);

  /**
   * APIs to send PEER_DUMP command to KvStore.
   * return error type:
   *      1. zme socket error
   *      2. server error returned by KvStore
   */
  folly::
      Expected<std::unordered_map<std::string, thrift::PeerSpec>, fbzmq::Error>
      getPeers();

  fbzmq::ZmqEventLoop*
  getEventLoop() const noexcept {
    return eventLoop_;
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

  /**
   * Utility function to SET keys in KvStore. Will throw an exception if things
   * goes wrong.
   */
  folly::Expected<folly::Unit, fbzmq::Error> setKeysHelper(
      std::unordered_map<std::string, thrift::Value> keyVals);

  /**
   * Utility function to del peers in KvStore
   * return error type:
   *    1. zmq socket error
   *    2. server error returned by KvStore
   */
  folly::Expected<folly::Unit, fbzmq::Error> delPeersHelper(
      const std::vector<std::string>& peerNames);

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
      int64_t ttl);

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
      folly::Optional<std::chrono::milliseconds> recvTimeout);

  /**
   * Create KvStoreCmd socket on the fly. REQ-REP sockets needs reset if
   * previously issued request fails and response hasn't been received as
   * REQ socket will enter into exceptional state.
   */
  void prepareKvStoreCmdSock() noexcept;

  void checkPersistKeyInStore();

  //
  // Immutable state
  //

  // Our local node identifier
  const std::string nodeId_;

  // ZmqEventLoop pointer for scheduling async events and socket callback
  // registration
  fbzmq::ZmqEventLoop* const eventLoop_{nullptr};

  // ZMQ context for IO processing
  fbzmq::Context& context_;

  // Socket Urls (we assume local, unencrypted connection)
  const std::string kvStoreLocalCmdUrl_;
  const std::string kvStoreLocalPubUrl_;

  // periodic timer to check existence of persist key in kv store
  folly::Optional<std::chrono::milliseconds> checkPersistKeyPeriod_;

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
  std::unordered_map<std::string, thrift::Value> persistedKeyVals_;

  // Subscribed keys to their callback functions
  std::unordered_map<std::string, KeyCallback> keyCallbacks_;

  // callback for every key published
  KeyCallback kvCallback_{nullptr};

  // backoff associated with each key for re-advertisements
  std::unordered_map<
      std::string /* key */,
      ExponentialBackoff<std::chrono::milliseconds>>
      backoffs_;

  // backoff associated with each key for freshing TTL
  std::unordered_map<
      std::string /* key */,
      std::pair<
          thrift::Value /* value */,
          ExponentialBackoff<std::chrono::milliseconds>>>
      keyTtlBackoffs_;

  // Set of local keys to be re-advertised.
  std::unordered_set<std::string> keysToAdvertise_;

  // Serializer object for thrift-obj <-> string conversion
  apache::thrift::CompactSerializer serializer_;

  // Timer to advertised pending key-vals
  std::unique_ptr<fbzmq::ZmqTimeout> advertiseKeyValsTimer_;

  // Timer to advertise ttl updates for key-vals
  std::unique_ptr<fbzmq::ZmqTimeout> ttlTimer_;
};

} // namespace openr

#include <openr/kvstore/KvStoreClient-inl.h>
