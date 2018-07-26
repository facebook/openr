/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Memory.h>
#include <folly/Optional.h>

#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStore.h>

namespace openr {

/**
 * A utility class to wrap and interact with KvStore. It exposes the APIs to
 * send commands to and receive publications from KvStore.
 * Mainly used for testing.
 *
 * Not thread-safe, use from the same thread only.
 */
class KvStoreWrapper {
 public:
  KvStoreWrapper(
      fbzmq::Context& zmqContext,
      std::string nodeId,
      std::chrono::seconds dbSyncInterval,
      std::chrono::seconds monitorSubmitInterval,
      std::unordered_map<std::string, thrift::PeerSpec> peers,
      folly::Optional<KvStoreFilters> filters = folly::none);

  ~KvStoreWrapper() {
    stop();
  }

  /**
   * Synchronous APIs to run and stop KvStore. This creates a thread
   * and stop it on destruction.
   *
   * Synchronous => function call with return only after thread is
   *                running/stopped completely.
   */
  void run() noexcept;
  void stop();

  /**
   * APIs to set key-value into the KvStore. Returns true on success else
   * returns false.
   */
  bool setKey(std::string key, thrift::Value value);

  /**
   * API to retrieve an existing key-value from KvStore. Returns empty if none
   * exists.
   */
  folly::Optional<thrift::Value> getKey(std::string key);

  /**
   * API to get dump from KvStore.
   * if we pass a prefix, only return keys that match it
   */
  std::unordered_map<std::string /* key */, thrift::Value> dumpAll(
      folly::Optional<KvStoreFilters> filters = folly::none);

  /**
   * API to get dump hashes from KvStore.
   * if we pass a prefix, only return keys that match it
   */
  std::unordered_map<std::string /* key */, thrift::Value> dumpHashes(
      std::string const& prefix = "");

  /**
   * API to get key vals on which hash differs from provided keyValHashes.
   */
  std::unordered_map<std::string /* key */, thrift::Value> syncKeyVals(
      thrift::KeyVals const& keyValHashes);

  /**
   * API to listen for a publication on PUB socket. This blocks until a
   * publication is received on the socket from KvStore.
   */
  thrift::Publication recvPublication(std::chrono::milliseconds timeout);

  /**
   * API to get counters from KvStore directly
   */
  fbzmq::thrift::CounterMap getCounters();

  /**
   * APIs to manage (add/remove) KvStore peers. Returns true on success else
   * returns false.
   */
  bool addPeer(std::string peerName, thrift::PeerSpec spec);
  bool delPeer(std::string peerName);

  /**
   * APIs to get existing peers of a KvStore.
   */
  std::unordered_map<std::string /* peerName */, thrift::PeerSpec> getPeers();

  /**
   * Utility function to get peer-spec for owned KvStore
   */
  thrift::PeerSpec
  getPeerSpec() const {
    return thrift::PeerSpec(
        apache::thrift::FRAGILE, globalPubUrl, globalCmdUrl);
  }

 public:
  /**
   * Socket URLs and other constants which can be used for connecting to KvStore
   * sockets.
   */
  const std::string nodeId;
  const std::string localCmdUrl;
  const std::string localPubUrl;

  /**
   * Global URLs could be created outside of kvstore, mainly for testing
   */
  const std::string globalCmdUrl;
  const std::string globalPubUrl;

  /**
   * Socket URL for zmq Monitoring
   */
  const std::string monitorSubmitUrl;

 private:
  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  apache::thrift::CompactSerializer serializer_;

  // ZMQ request socket for interacting with KvStore's command socket
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> reqSock_;

  // ZMQ sub socket for listening realtime updates from KvStore
  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> subSock_;

  // KvStore owned by this wrapper.
  std::unique_ptr<KvStore> kvStore_;

  // Thread in which KvStore will be running.
  std::thread kvStoreThread_;
};

} // namespace openr
