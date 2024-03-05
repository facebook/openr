/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/LsdbUtil.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/OpenrCtrlCppAsyncClient.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreServiceHandler.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/LogSample.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

namespace openr {

/**
 * A utility class to wrap and interact with KvStore. It exposes the APIs to
 * send commands to and receive publications from KvStore.
 * Mainly used for testing.
 *
 * Not thread-safe, use from the same thread only.
 */
template <class ClientType>
class KvStoreWrapper {
 public:
  KvStoreWrapper(
      // areaId collection
      const std::unordered_set<std::string>& areaIds,
      // KvStoreConfig to drive the instance
      const thrift::KvStoreConfig& kvStoreConfig,
      // Queue for receiving peer updates
      std::optional<messaging::RQueue<PeerEvent>> peerUpdatesQueue =
          std::nullopt,
      // Queue for receiving key-value update requests
      std::optional<messaging::RQueue<KeyValueRequest>> kvRequestQueue =
          std::nullopt);

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
   * Get reader for KvStore updates queue
   */
  messaging::RQueue<KvStorePublication>
  getReader() {
    return kvStoreUpdatesQueue_.getReader();
  }

  /**
   * Get writer reference for KvStoreSyncQueue
   */
  messaging::ReplicateQueue<KvStorePublication>&
  getKvStoreUpdatesQueueWriter() {
    return kvStoreUpdatesQueue_;
  }

  void
  openQueue() {
    kvStoreUpdatesQueue_.open();
  }

  void
  closeQueue() {
    kvStoreUpdatesQueue_.close();
  }

  void
  stopThriftServer() {
    // ATTN: it is user's responsibility to close the queue passed
    //       to thrift server before calling stop()
    thriftServerThread_.stop();
    thriftServerThread_.join();

    CHECK(kvStoreServiceHandler_.unique())
        << "Unexpected ownership of kvStoreServiceHandler";
    kvStoreServiceHandler_.reset();
  }

  /**
   * APIs to set key-value into the KvStore. Returns true on success else
   * returns false.
   */
  bool setKey(
      AreaId const& area,
      std::string key,
      thrift::Value value,
      std::optional<std::vector<std::string>> nodeIds = std::nullopt);

  /**
   * API to retrieve an existing key-value from KvStore. Returns empty if none
   * exists.
   */
  std::optional<thrift::Value> getKey(AreaId const& area, std::string key);

  /**
   * APIs to set key-values into the KvStore. Returns true on success else
   * returns false.
   */
  bool setKeys(
      AreaId const& area,
      const std::vector<std::pair<std::string, thrift::Value>>& keyVals,
      std::optional<std::vector<std::string>> nodeIds = std::nullopt);

  bool injectThriftFailure(AreaId const& area, std::string const& peerName);

  void
  publishKvStoreSynced() {
    kvStoreUpdatesQueue_.push(thrift::InitializationEvent::KVSTORE_SYNCED);
  }

  void pushToKvStoreUpdatesQueue(
      const AreaId& area,
      const std::unordered_map<std::string /* key */, thrift::Value>& keyVals);

  /**
   * API to get dump from KvStore.
   * if we pass a prefix, only return keys that match it
   */
  std::unordered_map<std::string /* key */, thrift::Value> dumpAll(
      AreaId const& area, std::optional<KvStoreFilters> filters = std::nullopt);

  /**
   * API to get dump hashes from KvStore.
   * if we pass a prefix, only return keys that match it
   */
  std::unordered_map<std::string /* key */, thrift::Value> dumpHashes(
      AreaId const& area, std::string const& prefix = "");

  /**
   * API to get dump of self originated key-vals from KvStore.
   */
  SelfOriginatedKeyVals dumpAllSelfOriginated(AreaId const& area);

  /**
   * API to get key vals on which hash differs from provided keyValHashes.
   */
  std::unordered_map<std::string /* key */, thrift::Value> syncKeyVals(
      AreaId const& area, thrift::KeyVals const& keyValHashes);

  /**
   * API to listen for a publication on PUB queue.
   */
  thrift::Publication recvPublication();

  /**
   * API to listen for KvStore sync signal on PUB queue.
   */
  void recvKvStoreSyncedSignal();

  /**
   * APIs to manage (add/remove) KvStore peers. Returns true on success else
   * returns false.
   */
  bool addPeer(AreaId const& area, std::string peerName, thrift::PeerSpec spec);
  bool addPeers(AreaId const& area, thrift::PeersMap& peers);
  bool delPeer(AreaId const& area, std::string peerName);

  std::optional<thrift::KvStorePeerState> getPeerState(
      AreaId const& area, std::string const& peerName);
  int64_t getPeerStateEpochTimeMs(
      AreaId const& area, std::string const& peerName);
  int64_t getPeerStateElapsedTimeMs(
      AreaId const& area, std::string const& peerName);
  int32_t getPeerFlaps(AreaId const& area, std::string const& peerName);

  /**
   * APIs to get existing peers of a KvStore.
   */
  std::unordered_map<std::string /* peerName */, thrift::PeerSpec> getPeers(
      AreaId const& area);

  /**
   * API to get summary of each KvStore area provided as input.
   */
  std::vector<thrift::KvStoreAreaSummary> getSummary(
      std::set<std::string> selectAreas);

  /**
   * Utility function to get peer-spec for owned KvStore
   */
  thrift::PeerSpec
  getPeerSpec(thrift::KvStorePeerState state = thrift::KvStorePeerState::IDLE) {
    return createPeerSpec(
        Constants::kPlatformHost.toString(), /* peerAddr for thrift */
        getThriftPort(),
        state);
  }

  /**
   * Get counters from KvStore
   */
  std::map<std::string, int64_t>
  getCounters() {
    return kvStore_->semifuture_getCounters().get();
  }

  KvStore<ClientType>*
  getKvStore() {
    return kvStore_.get();
  }

  inline uint16_t
  getThriftPort() {
    CHECK(kvStoreServiceHandler_)
        << "thrift server must be initialized in advance";
    return thriftServerThread_.getAddress()->getPort();
  }

  const std::string
  getNodeId() {
    return this->nodeId_;
  }

  std::unordered_set<std::string>
  getAreaIds() {
    return areaIds_;
  }

 private:
  const std::string nodeId_;

  // AreaId collection to indicate # of KvStoreDb spawn for different areas
  const std::unordered_set<std::string> areaIds_;

  // thrift::KvStoreConfig to feed to the KvStore instance
  const thrift::KvStoreConfig kvStoreConfig_;

  // Queue for streaming KvStore updates
  messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue_;
  messaging::RQueue<KvStorePublication> kvStoreUpdatesQueueReader_{
      kvStoreUpdatesQueue_.getReader()};

  // Queue for publishing the event log
  messaging::ReplicateQueue<LogSample> logSampleQueue_;

  // Queue for streaming peer updates from LM
  messaging::ReplicateQueue<PeerEvent> dummyPeerUpdatesQueue_;

  // Emtpy queue for streaming key events from sources which persist keys into
  // KvStore Will be removed once KvStoreClientInternal is deprecated
  messaging::ReplicateQueue<KeyValueRequest> dummyKvRequestQueue_;

  // KvStore instance owned by this wrapper
  std::unique_ptr<KvStore<ClientType>> kvStore_;

  // KvStoreServiceHandler instance used for thrift server usage
  std::shared_ptr<KvStoreServiceHandler<ClientType>> kvStoreServiceHandler_;

  // Thread in which KvStore will be running
  std::thread kvStoreThread_;

  // Thread in which thrift server will be running
  apache::thrift::util::ScopedServerThread thriftServerThread_;
};

} // namespace openr
