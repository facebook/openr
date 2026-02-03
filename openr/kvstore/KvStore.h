/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/TokenBucket.h>
#include <folly/container/F14Set.h>
#include <folly/gen/Base.h>
#include <folly/io/async/AsyncTimeout.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/AsyncThrottle.h>
#include <openr/common/Constants.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrClient.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreParams.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {

/*
 * The KvStoreDb class represents a KV Store database and stores KV pairs in
 * an internal map. KV store DB instance is created for each area.
 *
 * This class processes messages received from KvStore peer. The configuration
 * is passed via KvStoreParams from constructor.
 */
template <class ClientType>
class KvStoreDb {
 public:
  KvStoreDb(
      OpenrEventBase* evb,
      KvStoreParams& kvParams,
      const std::string& area,
      const std::string& nodeId,
      std::function<void()> initialKvStoreSyncedCallback,
      std::function<void()> initialSelfOriginatedKeysSyncedCallback);

  ~KvStoreDb() = default;

  // shutdown fiber/timer/etc.
  void stop();

  inline std::string const&
  getAreaId() const {
    return area_;
  }

  inline std::string const&
  AreaTag() const {
    return areaTag_;
  }

  inline int32_t
  getPeerCnt() const {
    return thriftPeers_.size();
  }

  inline bool
  getInitialSyncedWithPeers() const {
    return initialSyncCompleted_;
  }

  inline bool
  getInitialSelfOriginatedKeysSyncCompleted() const {
    return initialSelfOriginatedKeysSyncCompleted_;
  }

  inline bool
  getIsStopped() const {
    return isStopped_;
  }

  // get all active (ttl-refreshable) self-originated key-vals
  SelfOriginatedKeyVals const&
  getSelfOriginatedKeyVals() const {
    return selfOriginatedKeyVals_;
  }

  thrift::KeyVals const&
  getKeyValueMap() const {
    return kvStore_;
  }

  inline TtlCountdownQueue const&
  getTtlCountdownQueue() const {
    return ttlCountdownQueue_;
  }

  /*
   * [Util]
   *
   * This section contains the util function used by KvStoreDb to
   * set/update/merge key-value pairs.
   */

  // Extracts the counters
  std::map<std::string, int64_t> getCounters() const;

  // Calculate size of KvStoreDB (just the key/val pairs)
  size_t getKeyValsSize() const;

  // get multiple keys at once
  thrift::Publication getKeyVals(std::vector<std::string> const& keys);

  // add new key-vals to kvstore_'s key-vals
  thrift::SetKeyValsResult setKeyVals(
      thrift::KeySetParams&& setParams, bool isSelfOriginatedUpdate = true);

  /*
   * This is the util function to do the following:
   *  1) Merge received publication with local store object, aka, "kvStore_" and
   *     flood the delta to the peers.
   *  2) Respond to `senderId` with requested `tobeUpdatedKeys`.
   *
   * @param: rcvdPublication => the thrift::Publication object received to
   *                            merge with local store
   * @param: isSelfOriginatedUpdate => mark if this publication is coming from
   *                                   internal, aka, self-originated k-v pair.
   * @param: senderId => if senderId is set, will send back to `senderId` with
   *                     the k-v paris inside rcvdPublication.tobeUpdatedKeys
   * @return: thrift::KvStoreMergeResult
   */
  thrift::KvStoreMergeResult mergePublication(
      thrift::Publication const& rcvdPublication,
      bool isSelfOriginatedUpdate,
      std::optional<std::string> senderId = std::nullopt);

  /*
   * [Peer Management]
   *
   * KvStoreDb keeps synchronizing with peers via TCP session through thrift
   * client connection. It provides multiple util functions to interact with
   * peers including:
   *    1) addThriftPeers(Thrift)
   *    2) delThriftPeers(Thrift)
   *    3) dumpPeers(Thrift)
   */
  void addThriftPeers(
      std::unordered_map<std::string, thrift::PeerSpec> const& peers);
  void delThriftPeers(std::vector<std::string> const& peers);
  thrift::PeersMap dumpPeers();

  /*
   * [KvStore Peer State]
   *
   * KvStore maintains Finite State Machine(FSM) to manage peer state.
   * It exposes multiple helper functions.
   */

  // util function to fetch peer by its state
  std::vector<std::string> getPeersByState(thrift::KvStorePeerState state);

  // util funtion to fetch KvStorePeerState
  std::optional<thrift::KvStorePeerState> getCurrentState(
      std::string const& peerName);

  // util function for state transition
  static thrift::KvStorePeerState getNextState(
      std::optional<thrift::KvStorePeerState> const& currState,
      KvStorePeerEvent const& event);

  // util funtion to fetch KvStorePeerStateEpochTimeMs
  int64_t getCurrentStateEpochTimeMs(std::string const& peerName);

  // util funtion to fetch KvStorePeerFlaps
  int32_t getCurrentFlaps(std::string const& peerName);

  /*
   * [Open/R Initialization]
   *
   * Process KvStore sync event in OpenR initialization procedure.
   * A syncing completion signal will be marked in following cases:
   *    1) No peers in the area thus syncing is not needed, or
   *    2) achieving INITIALIZED state, or
   *    3) running into THRIFT_API_ERROR
   */
  void processInitializationEvent();

  /*
   * [Self Originated Key Management]
   *
   * Public API used for self-originated key management including:
   *    1) persistSelfOriginatedKey
   *      Set specified key-value in KvStore. This is an authoratitive call,
   *      meaning if someone else advertises the same key we try to win over
   *      it by setting key-value with higher version. By default key is
   *      published to default area.
   *    2) setSelfOriginatedKey
   *      Set key-value in KvStore with specified version. If version is 0,
   *      the one greater than the latest known will be used.
   *    3) unsetSelfOriginatedKey
   *      Set new value for self-originated key and stop ttl-refreshing by
   *      clearing from local cache.
   *    4) eraseSelfOriginatedKey
   *      Erase key from local cache(DO NOT SET NEW VALUE), thus stopping
   *      ttl-refreshing.
   */
  void persistSelfOriginatedKey(
      std::string const& key, std::string const& value);
  void setSelfOriginatedKey(
      std::string const& key, std::string const& value, uint32_t version);
  void unsetSelfOriginatedKey(std::string const& key, std::string const& value);
  void eraseSelfOriginatedKey(std::string const& key);

  /*
   * [Initial Sync]
   *
   * util method to process thrift sync response in:
   *    1) Success
   *    2) Failure
   */
  void processThriftSuccess(
      std::string const& peerName,
      thrift::Publication&& pub,
      std::chrono::milliseconds timeDelta);

  void processThriftFailure(
      std::string const& peerName,
      std::string const& type,
      folly::fbstring const& exceptionStr,
      std::chrono::steady_clock::time_point startTime);

 private:
  // disable copying
  KvStoreDb(KvStoreDb const&) = delete;
  KvStoreDb& operator=(KvStoreDb const&) = delete;
  KvStoreDb(KvStoreDb&&) = delete;
  KvStoreDb& operator=(KvStoreDb&&) = delete;

  /*
   * Private methods
   */

  // util wrapper function which calls both logStateTransition and
  // publishPeerStateCounters functions
  void logStateTransitionWithCounterPublication(
      std::string const& peerName,
      thrift::KvStorePeerState oldState,
      thrift::KvStorePeerState newState);

  // util function to log state transition
  void logStateTransition(
      std::string const& peerName,
      thrift::KvStorePeerState oldState,
      thrift::KvStorePeerState newState);

  // a util function to publish number of peers by state as fb303 counters
  void publishPeerStateCounters();

  /*
   * [Initial Sync]
   *
   * util method to scan over thrift peers in IDLE state.
   *
   * perform initial step of a 3-way full-sync request
   */
  void requestThriftPeerSync();

  /*
   * [Initial Sync]
   *
   * perform last step as a 3-way full-sync request:
   * the full-sync initiator sends back key-val to senderId (where we made
   * full-sync request to) who need to update those keys due to:
   *    1) it doesn't have the keys;
   *    2) it has outdated version of keys;
   */
  void finalizeFullSync(
      const folly::F14FastSet<std::string>& keys, const std::string& senderId);

  /*
   * [Version Inconsistency Mitigation]
   */

  // forward declare
  struct KvStorePeer;
  void disconnectPeer(KvStorePeer& peer, KvStorePeerEvent const& event);

  /*
   * [Incremental flooding]
   *
   * util method to flood publication to neighbors
   *
   * @param: publication => data element to flood
   * @param: rateLimit => if 'false', publication will not be rate limited
   */
  void floodPublication(
      thrift::Publication&& publication, bool rateLimit = true);

  /*
   * [Incremental flooding]
   *
   * util method to get flooding peers for a given spt-root-id.
   */
  folly::F14FastSet<std::string> getFloodPeers();

  /*
   * [Incremental flooding]
   *
   * buffer publications blocked by the rate limiter
   * flood pending update blocked by rate limiter
   */
  void bufferPublication(thrift::Publication&& publication);
  void floodBufferedUpdates();

  /*
   * [Ttl Management]
   *
   * add new query entries into ttlCountdownQueue from publication
   * and reschedule ttl expiry timer if needed
   */
  void updateTtlCountdownQueue(
      const thrift::Publication& publication, bool isSelfOriginatedUpdate);

  /*
   * [Ttl Management]
   *
   * periodically count down and purge expired keys from CountdownQueue
   */
  void cleanupTtlCountdownQueue();

  /*
   * [Logging]
   *
   * Submit events to monitor
   */
  void logSyncEvent(
      const std::string& peerNodeName,
      const std::chrono::milliseconds syncDuration);
  void logKvEvent(const std::string& event, const std::string& key);

  /*
   * [Self Originated Key Management with ttl refreshing]
   *
   * KvStoreDb will manage ttl-refreshing for self-originated key-vals sent
   * via queue.
   *
   * It provides set of util methods to manage self-originiated keys with:
   *    1) key persistence
   *    2) key ttl-refreshing
   * update ttls for all self-originated key-vals
   * schedule ttl updates for self-originated key-vals
   */
  void advertiseTtlUpdates();
  void scheduleTtlUpdates(std::string const& key, bool advertiseImmediately);

  /*
   * [Self Originated Key Management with throttling]
   *
   * KvStoreDb uses throttling to advertise key-value changes to KvStore in
   * batches. It provides the following util methods to:
   *     1) advertise persisted self-originated key-vals in batches
   *     2) unset self-originated key-vals in batches
   */
  void advertiseSelfOriginatedKeys();
  void unsetPendingSelfOriginatedKeys();

  /*
   * [Self Originated Key Management with publication]
   *
   * Self-originiated keys needs sync with latest KvStore contents.
   * To handle potential discrepancy like:
   *
   *  - t0: advertise self-orinigated key with version 1 since local KvStoreDb
   *    is empty;
   *  - t1: KvStoreDb FULL_SYNC finished and see keys advertised by its
   * previous incarnaton with higher version(>1);
   */
  void processPublicationForSelfOriginatedKey(
      thrift::Publication const& publication);

  /*
   * [Monitoring]
   *
   * fiber task and util function to periodically dump flooding topology.
   *
   * Signaling part consists of:
   *  - Promise retained in state variable of KvStoreDb. Fiber awaits on it.
   *    The promise is fulfilled in destructor of KvStoreDb.
   *  - SemiFuture is passed to fiber for awaiting.
   */
  void floodTopoDump() noexcept;
  void floodTopoDumpTask() noexcept;

  /*
   * [Monitoring]
   *
   * fiber task and util function to periodically check key ttl
   *
   * ATTN:
   *  - Adjacency key can be very important for LSDB protocol to run;
   *  - Adjacency key should NEVER be under certain threshold if KvStore
   *    has the adj key originator in its peer collection.
   */
  void checkKeyTtl() noexcept;
  void checkKeyTtlTask() noexcept;

  /*
   * Private variables
   */

  // Kv store parameters
  KvStoreParams& kvParams_;

  // area identified of this KvStoreDb instance
  const std::string area_{};

  // area id tag for logging purpose
  const std::string areaTag_{};

  // KvStore peer struct to convey peer information
  struct KvStorePeer {
    KvStorePeer(
        const std::string& nodeName,
        const std::string& areaTag,
        const thrift::PeerSpec& ps,
        const ExponentialBackoff<std::chrono::milliseconds>& expBackoff,
        const KvStoreParams& kvParams);

    // util function to create new or get existing thrift client
    bool getOrCreateThriftClient(
        OpenrEventBase* evb, std::optional<int> maybeIpTos);

#pragma region ApiWrapper
    // KvStorePeer API wrappers
    folly::SemiFuture<folly::Unit> setKvStoreKeyValsWrapper(
        const std::string& area, const thrift::KeySetParams& keySetParams);

    folly::SemiFuture<thrift::Publication> getKvStoreKeyValsFilteredAreaWrapper(
        const thrift::KeyDumpParams& filter, const std::string& area);
#pragma endregion ApiWrapper

    // node name
    const std::string nodeName{};

    // area tag
    const std::string areaTag{};

    // peer spec(peerSpec can be modified as peerAddr can change)
    thrift::PeerSpec peerSpec;

    // exponetial backoff in case of retry after sync failure
    ExponentialBackoff<std::chrono::milliseconds> expBackoff;

    // KvStorePeer now supports 2 types of clients:
    // 1. thrift::OpenrCtrlCppAsyncClient -> KvStore runs with Open/R;
    // 2. thrift::KvStoreServiceAsyncClient -> KvStore runs independently;
    std::unique_ptr<ClientType> plainTextClient{nullptr};

    // only if TLS is enabled
    std::unique_ptr<ClientType> secureClient{nullptr};

    // Stores set of keys that may have changed during initialization of this
    // peer. Will flood to them in finalizeFullSync(), the last step of
    // initial sync.
    folly::F14FastSet<std::string> pendingKeysDuringInitialization{};

    // Number of occured Thrift API errors in the process of syncing with
    // peer.
    int64_t numThriftApiErrors{0};

    // Kv store parameters
    const KvStoreParams& kvParams_;
  };

  // Set of peers with all info over thrift channel
  std::unordered_map<std::string, KvStorePeer> thriftPeers_{};

  // Boolean flag indicating whether initial KvStoreDb sync with all peers
  // completed in OpenR initialization procedure.
  bool initialSyncCompleted_{false};
  bool initialSelfOriginatedKeysSyncCompleted_{false};

  // store keys mapped to (version, originatoId, value)
  thrift::KeyVals kvStore_{};

  // TTL count down queue
  TtlCountdownQueue ttlCountdownQueue_;

  // Map holding the handles to the elements in ttlCountdownQueue_
  // Key is struct TtlCountdownHandleKey(key, originatorId) and value is the
  // handle to the element.
  // Note: F14FastMap is used here instead of F14NodeMap
  // because of the automatic sizing when we call erase(). This should not cause
  // referance stability with current code as there are no long standing
  // references. Current UTs do resize the map multiple times and are able to
  // pass.
  folly::F14FastMap<TtlCountdownHandleKey, TtlCountdownQueue::handle_type>
      ttlCountdownHandleMap_;

  // TTL count down timer
  std::unique_ptr<folly::AsyncTimeout> ttlCountdownTimer_{nullptr};

  // Kvstore rate limiter
  std::unique_ptr<folly::BasicTokenBucket<>> floodLimiter_{nullptr};

  // timer to send pending kvstore publication
  std::unique_ptr<folly::AsyncTimeout> pendingPublicationTimer_{nullptr};

  // timer to promote idle peers for initial syncing
  std::unique_ptr<folly::AsyncTimeout> thriftSyncTimer_{nullptr};

  // timer to advertise ttl updates for self-originated key-vals
  std::unique_ptr<folly::AsyncTimeout> selfOriginatedKeyTtlTimer_{nullptr};

  // timer to advertise key-vals for self-originated keys
  std::unique_ptr<folly::AsyncTimeout> advertiseKeyValsTimer_{nullptr};

  // all self originated key-vals and their backoffs
  // persistKey and setKey will add, clearKey will remove
  std::unordered_map<std::string /* key */, SelfOriginatedValue>
      selfOriginatedKeyVals_{};

  // Map of keys to unset to new values to set. Used for batch processing of
  // unset ClearKeyValueRequests.
  std::unordered_map<std::string /* key */, thrift::Value> keysToUnset_{};

  // Set of local keys to be re-advertised.
  folly::F14FastSet<std::string /* key */> keysToAdvertise_{};

  // Throttle advertisement of self-originated persisted keys.
  // Calls `advertiseSelfOriginatedKeys()`.
  std::unique_ptr<AsyncThrottle> advertiseSelfOriginatedKeysThrottled_{nullptr};

  // Throttle advertisement of TTL updates for self-originated keys.
  // Calls `advertiseTtlUpdates()`.
  std::unique_ptr<AsyncThrottle> selfOriginatedTtlUpdatesThrottled_{nullptr};

  // Throttle unsetting of self-originated keys.
  // Calls `unsetPendingSelfOriginatedKeys()`.
  std::unique_ptr<AsyncThrottle> unsetSelfOriginatedKeysThrottled_{nullptr};

  // pending keys to flood publication
  // map<flood-root-id: set<keys>>
  std::unordered_map<std::optional<std::string>, folly::F14FastSet<std::string>>
      publicationBuffer_{};

  // Callback function to signal KvStore that KvStoreDb sync with all peers
  // are completed.
  std::function<void()> initialKvStoreSyncedCallback_;
  std::function<void()> initialSelfOriginatedKeysSyncedCallback_;

  // max parallel syncs allowed. It's initialized with '2' and doubles
  // up to a max value of kMaxFullSyncPendingCountThresholdfor each full sync
  // response received
  size_t parallelSyncLimitOverThrift_{2};

  // Stop signal for fiber to periodically dump flood topology
  folly::fibers::Baton floodTopoStopSignal_;

  // Stop signal for fiber to periodically check adj key ttl
  folly::fibers::Baton ttlCheckStopSignal_;

  // event loop
  OpenrEventBase* evb_{nullptr};

  // Boolean flag to mark KvStoreDb terminated and not processing callbacks
  std::atomic<bool> isStopped_{false};

  // vector to store all child fiber tasks
  std::vector<folly::Future<folly::Unit>> kvStoreDbWorkers_;
};

/*
 * The class represents a server on which the requests are listened via
 * thrift channel. The configuration is passed via constructor arguments.
 * This class instantiates individual KvStoreDb per area. Area config is
 * passed in the constructor.
 */
template <class ClientType>
class KvStore final : public OpenrEventBase {
 public:
  KvStore(
      // Queue for publishing kvstore updates
      messaging::ReplicateQueue<KvStorePublication>& kvStoreUpdatesQueue,
      // Queue for receiving peer updates
      messaging::RQueue<PeerEvent> peerUpdatesQueue,
      // Queue for receiving key-value update requests
      messaging::RQueue<KeyValueRequest> kvRequestQueue,
      // Queue for publishing the event log
      messaging::ReplicateQueue<LogSample>& logSampleQueue,
      // AreaId collection
      const folly::F14FastSet<std::string>& areaIds,
      // KvStoreConfig to drive the instance
      const thrift::KvStoreConfig& kvStoreConfig);

  ~KvStore() override = default;

  void stop() override;

  inline bool
  getIsStopped(std::string const& area) {
    if (!kvStoreDb_.count(area)) {
      throw std::runtime_error(std::string{"area not found"});
    }
    return kvStoreDb_.at(area).getIsStopped();
  }

  /*
   * Check if initial self originated keys timer currently
   * scheduled on or off
   */
  inline bool
  isInitialSelfOriginatedKeysTimerScheduled() {
    if (initialSelfOriginatedKeysTimer_->isScheduled()) {
      return true;
    }

    return false;
  }

  /*
   * Check if kvStoreSync timer currently scheduled on or off
   */
  inline bool
  isKvStoreSyncTimerScheduled() {
    if (kvStoreSyncTimer_ && kvStoreSyncTimer_->isScheduled()) {
      return true;
    }

    return false;
  }

  /*
   * [Open/R Initialization]
   *
   * This is the callback function used by KvStoreDb to mark initial
   * KVSTORE_SYNC stage done during Open/R initialization sequence.
   */
  void initialKvStoreDbSynced();

  /*
   * [Open/R Initialization]
   *
   * This is the callback function used by KvStoreDb to mark initial
   * SELF_ADJ_SYNC stage done during Open/R initialization sequence.
   */
  void initialSelfOriginatedKeysSynced();

  /*
   * [Public APIs]
   *
   * KvStore exposes multiple public APIs for external caller to be able to
   *  1) dump/get/set keys;
   *  2) dump hashes;
   *  3) dump self-originated keys;
   */
  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreKeyVals(
      std::string area, thrift::KeyGetParams keyGetParams);

  folly::SemiFuture<folly::Unit> semifuture_setKvStoreKeyVals(
      std::string area, thrift::KeySetParams keySetParams);

  folly::SemiFuture<std::unique_ptr<thrift::SetKeyValsResult>>
  semifuture_setKvStoreKeyValues(
      std::string area, thrift::KeySetParams keySetParams);

  folly::SemiFuture<std::unique_ptr<bool>> semifuture_injectThriftFailure(
      std::string area, std::string peerName);

  folly::SemiFuture<bool>
  semifuture_checkInitialSelfOriginatedKeysTimerScheduled();

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::Publication>>>
  semifuture_dumpKvStoreKeys(
      thrift::KeyDumpParams keyDumpParams,
      std::set<std::string> selectAreas = {});

  folly::SemiFuture<std::unique_ptr<SelfOriginatedKeyVals>>
  semifuture_dumpKvStoreSelfOriginatedKeys(std::string area);

  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_dumpKvStoreHashes(
      std::string area, thrift::KeyDumpParams keyDumpParams);

  /*
   * [Public APIs]
   *
   * API to persist a self-originated key in KvStore.
   * This is an authoritative call - if someone else advertises the same key,
   * KvStore will try to win over it by setting key-value with higher version.
   */
  folly::SemiFuture<folly::Unit> semifuture_persistSelfOriginatedKey(
      std::string&& area, thrift::KeySetParams&& keySetParams);

  /*
   * [Public APIs]
   *
   * API to unset self-originated keys in KvStore.
   * This call sets a final value (deletion marker) with incremented version
   * and stops ttl-refreshing by clearing from local cache.
   */
  folly::SemiFuture<folly::Unit> semifuture_unsetSelfOriginatedKey(
      std::string&& area, thrift::KeySetParams&& keySetParams);

  /*
   * [Public APIs]
   *
   * Set of APIs to interact with KvStore peers
   */
  folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
  semifuture_getKvStorePeers(std::string area);

  folly::SemiFuture<folly::Unit> semifuture_addUpdateKvStorePeers(
      std::string area, thrift::PeersMap peersToAdd);

  folly::SemiFuture<folly::Unit> semifuture_deleteKvStorePeers(
      std::string area, std::vector<std::string> peersToDel);

  /*
   * [Public APIs]
   *
   * Set of APIs to retrieve internal state including:
   * state/counter/reader/etc.
   */
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::KvStoreAreaSummary>>>
  semifuture_getKvStoreAreaSummaryInternal(
      std::set<std::string> selectAreas = {});

  folly::SemiFuture<std::map<std::string, int64_t>> semifuture_getCounters();

  // API to get reader for kvStoreUpdatesQueue
  messaging::RQueue<KvStorePublication> getKvStoreUpdatesReader();

  // API to fetch state of peerNode, used for unit-testing
  folly::SemiFuture<std::optional<thrift::KvStorePeerState>>
  semifuture_getKvStorePeerState(
      std::string const& area, std::string const& peerName);

  // API to fetch state epoch time of peerNode, used for unit-testing
  folly::SemiFuture<int64_t> semifuture_getKvStorePeerStateEpochTimeMs(
      std::string const& area, std::string const& peerName);

  // API to fetch peerNode flaps, used for unit-testing
  folly::SemiFuture<int32_t> semifuture_getKvStorePeerFlaps(
      std::string const& area, std::string const& peerName);

// [Public APIs]
// Coroutine versions
#if FOLLY_HAS_COROUTINES
  folly::coro::Task<folly::Unit> co_setKvStoreKeyVals(
      std::string area, thrift::KeySetParams keySetParams);

  // same as co_setKvStoreKeyVals, but returns result instead of void.
  folly::coro::Task<std::unique_ptr<thrift::SetKeyValsResult>>
  co_setKvStoreKeyValues(std::string area, thrift::KeySetParams keySetParams);

  folly::coro::Task<std::unique_ptr<thrift::Publication>> co_getKvStoreKeyVals(
      std::string area, thrift::KeyGetParams keyGetParams);

  folly::coro::Task<std::unique_ptr<std::vector<thrift::Publication>>>
  co_dumpKvStoreKeys(
      thrift::KeyDumpParams keyDumpParams,
      std::set<std::string> selectAreas = {});

  folly::coro::Task<std::unique_ptr<thrift::Publication>> co_dumpKvStoreHashes(
      std::string area, thrift::KeyDumpParams keyDumpParams);

  folly::coro::Task<std::unique_ptr<std::vector<thrift::KvStoreAreaSummary>>>
  co_getKvStoreAreaSummaryInternal(std::set<std::string> selectAreas = {});

  folly::coro::Task<std::unique_ptr<thrift::PeersMap>> co_getKvStorePeers(
      std::string area);

  folly::coro::Task<folly::Unit> co_persistSelfOriginatedKey(
      std::string&& area, thrift::KeySetParams&& keySetParams);

  folly::coro::Task<folly::Unit> co_unsetSelfOriginatedKey(
      std::string&& area, thrift::KeySetParams&& keySetParams);

  // [private APIs]
 private:
  folly::coro::Task<thrift::Publication> co_getKvStoreKeyValsInternal(
      std::string area, thrift::KeyGetParams keyGetParams);

  folly::coro::Task<thrift::SetKeyValsResult> co_setKvStoreKeyValsInternal(
      std::string area, thrift::KeySetParams keySetParams);

  folly::coro::Task<std::unique_ptr<std::vector<thrift::Publication>>>
  co_dumpKvStoreKeysImpl(
      thrift::KeyDumpParams keyDumpParams, std::set<std::string> selectAreas);

  folly::coro::Task<thrift::Publication> co_dumpKvStoreHashesImpl(
      std::string area, thrift::KeyDumpParams keyDumpParams);

  folly::coro::Task<std::vector<thrift::KvStoreAreaSummary>>
  co_getKvStoreAreaSummaryImpl(std::set<std::string> selectAreas);

  folly::coro::Task<folly::Unit> co_persistSelfOriginatedKeyInternal(
      std::string&& area, thrift::KeySetParams&& keySetParams);

  folly::coro::Task<folly::Unit> co_unsetSelfOriginatedKeyInternal(
      std::string&& area, thrift::KeySetParams&& keySetParams);
#endif // FOLLY_HAS_COROUTINES

 private:
  // disable copying
  KvStore(KvStore const&) = delete;
  KvStore& operator=(KvStore const&) = delete;
  KvStore(KvStore&&) = delete;
  KvStore& operator=(KvStore&&) = delete;

  /*
   * Private methods
   */

  // util function to process peer updates
  void processPeerUpdates(PeerEvent&& event);

  /*
   * [Self Originated Key Management]
   *
   * Wrapper function to redirect request to update specific kvStoreDb
   */
  void processKeyValueRequest(KeyValueRequest&& kvRequest);

  /*
   * [Counter]
   *
   * util methods called by getCounters() public API
   */
  std::map<std::string, int64_t> getGlobalCounters() const;
  void initGlobalCounters();

  /*
   * Initialize all kvstore instances with zero peers
   */
  void initZeroPeersKvStores();

  /*
   * This is a helper function which returns a reference to the relevant
   * KvStoreDb or throws an instance of KvStoreError for backward compaytibilty.
   *
   * Backward compatibility:
   * It allows getting single configured area if default area is requested or
   * is the only one configured areaId for areaId migration purpose.
   */
  KvStoreDb<ClientType>& getAreaDbOrThrow(
      std::string const& areaId, std::string const& caller);

  std::unique_ptr<std::vector<thrift::Publication>> dumpKvStoreKeysImpl(
      thrift::KeyDumpParams keyDumpParams, std::set<std::string> selectAreas);

  thrift::Publication dumpKvStoreHashesImpl(
      std::string area, thrift::KeyDumpParams keyDumpParams);

  std::vector<thrift::KvStoreAreaSummary> getKvStoreAreaSummaryImpl(
      std::set<std::string> selectAreas);

  /*
   * Private variables
   */

  // Timer for updating and submitting counters periodically
  std::unique_ptr<folly::AsyncTimeout> counterUpdateTimer_{nullptr};

  std::unique_ptr<folly::AsyncTimeout> initialSelfOriginatedKeysTimer_{nullptr};

  // Timer to await for learning at least one peer
  std::unique_ptr<folly::AsyncTimeout> kvStoreSyncTimer_{nullptr};

  // kvstore parameters common to all kvstoreDB
  KvStoreParams kvParams_;

  // map of area IDs and instance of KvStoreDb
  std::unordered_map<std::string /* area ID */, KvStoreDb<ClientType>>
      kvStoreDb_{};

  // Boolean flag to indicate if kvStoreSynced signal is published in OpenR
  // initialization process.
  bool initialSyncSignalSent_{false};
  bool initialSelfAdjSyncSignalSent_{false};

  // vector to store all child fiber tasks
  std::vector<folly::Future<folly::Unit>> kvStoreWorkers_;
};

} // namespace openr

#include <openr/kvstore/KvStore-inl.h>
