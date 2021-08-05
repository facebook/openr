/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/heap/priority_queue.hpp>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Optional.h>
#include <folly/TokenBucket.h>
#include <folly/futures/Future.h>
#include <folly/gen/Base.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/AsyncTimeout.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrClient.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/dual/Dual.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Types_constants.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/LogSample.h>

namespace openr {

//
// Define KvStorePeerEvent which triggers the peer state
// transition.
//
enum class KvStorePeerEvent {
  PEER_ADD = 0,
  PEER_DEL = 1,
  SYNC_RESP_RCVD = 2,
  THRIFT_API_ERROR = 3,
};

struct TtlCountdownQueueEntry {
  std::chrono::steady_clock::time_point expiryTime;
  std::string key;
  int64_t version{0};
  int64_t ttlVersion{0};
  std::string originatorId;
  bool
  operator>(TtlCountdownQueueEntry other) const {
    return expiryTime > other.expiryTime;
  }
};

// Structure for values and their backoffs for self-originated key-vals
struct SelfOriginatedValue {
  // Value associated with the self-originated key
  thrift::Value value;
  // Backoff for advertising key-val to kvstore_. Only for persisted key-vals.
  std::optional<ExponentialBackoff<std::chrono::milliseconds>> keyBackoff;
  // Backoff for advertising ttl updates for this key-val
  ExponentialBackoff<std::chrono::milliseconds> ttlBackoff;

  SelfOriginatedValue() {}
  explicit SelfOriginatedValue(const thrift::Value& val) : value(val) {}
};

using SelfOriginatedKeyVals =
    std::unordered_map<std::string, SelfOriginatedValue>;

// TODO: migrate to std::priority_queue
using TtlCountdownQueue = boost::heap::priority_queue<
    TtlCountdownQueueEntry,
    // Always returns smallest first
    boost::heap::compare<std::greater<TtlCountdownQueueEntry>>,
    boost::heap::stable<true>>;

// structure for common params across all instances of KvStoreDb
struct KvStoreParams {
  // the name of this node (unique in domain)
  std::string nodeId;

  // Queue for publishing KvStore updates to other modules within a process
  messaging::ReplicateQueue<Publication>& kvStoreUpdatesQueue;

  // Queue for publishing kvstore peer initial sync events
  messaging::ReplicateQueue<KvStoreSyncEvent>& kvStoreSyncEventsQueue;

  // Queue to publish the event log
  messaging::ReplicateQueue<LogSample>& logSampleQueue;

  // [TO BE DEPRECATED]
  // socket for remote & local commands
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> globalCmdSock;

  // [TO BE DEPRECATED]
  // ZMQ high water
  int zmqHwm;
  // IP ToS
  std::optional<int> maybeIpTos;
  // how often to request full db sync from peers
  std::chrono::seconds dbSyncInterval;
  // KvStore key filters
  std::optional<KvStoreFilters> filters;
  // Kvstore flooding rate
  std::optional<thrift::KvstoreFloodRate> floodRate;
  // TTL decrement factor
  std::chrono::milliseconds ttlDecr{Constants::kTtlDecrement};
  // TTL for self-originated keys
  std::chrono::milliseconds keyTtl{0};
  // DUAL related config knob
  bool enableFloodOptimization{false};
  bool isFloodRoot{false};
  bool enableThriftDualMsg{false};
  // consume requests from local modules (e.g. PrefixManager, LinkMonitor)
  // via queue
  bool enableKvStoreRequestQueue{false};

  KvStoreParams(
      std::string nodeId,
      messaging::ReplicateQueue<Publication>& kvStoreUpdatesQueue,
      messaging::ReplicateQueue<KvStoreSyncEvent>& kvStoreSyncEventsQueue,
      messaging::ReplicateQueue<LogSample>& logSampleQueue,
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> globalCmdSock,
      // ZMQ high water mark
      int zmqhwm,
      // how often to request full db sync from peers
      std::chrono::seconds dbsyncInterval,
      std::optional<KvStoreFilters> filter,
      // Kvstore flooding rate
      std::optional<thrift::KvstoreFloodRate> floodrate,
      // TTL decrement factor
      std::chrono::milliseconds ttldecr,
      // TTL for self-originated keys
      std::chrono::milliseconds keyTtl,
      // DUAL related config knob
      bool enableFloodOptimization,
      bool isFloodRoot,
      bool enableThriftDualMsg,
      bool enableKvStoreRequestQueue)
      : nodeId(nodeId),
        kvStoreUpdatesQueue(kvStoreUpdatesQueue),
        kvStoreSyncEventsQueue(kvStoreSyncEventsQueue),
        logSampleQueue(logSampleQueue),
        globalCmdSock(std::move(globalCmdSock)),
        zmqHwm(zmqhwm),
        dbSyncInterval(dbsyncInterval),
        filters(std::move(filter)),
        floodRate(std::move(floodrate)),
        ttlDecr(ttldecr),
        keyTtl(keyTtl),
        enableFloodOptimization(enableFloodOptimization),
        isFloodRoot(isFloodRoot),
        enableThriftDualMsg(enableThriftDualMsg),
        enableKvStoreRequestQueue(enableKvStoreRequestQueue) {}
};

// The class represents a KV Store DB and stores KV pairs in internal map.
// KV store DB instance is created for each area.
// This class processes messages received from KvStore server. The configuration
// is passed via constructor arguments.

class KvStoreDb : public DualNode {
 public:
  KvStoreDb(
      OpenrEventBase* evb,
      KvStoreParams& kvParams,
      const std::string& area,
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_CLIENT> peersyncSock,
      bool isFloodRoot,
      const std::string& nodeId,
      std::function<void()> initialKvStoreSyncedCallback);

  ~KvStoreDb() override;

  inline std::string const&
  getAreaId() const {
    return area_;
  }

  inline std::string const&
  AreaTag() const {
    return areaTag_;
  }

  // get all active (ttl-refreshable) self-originated key-vals
  SelfOriginatedKeyVals const&
  getSelfOriginatedKeyVals() const {
    return selfOriginatedKeyVals_;
  }

  folly::Expected<fbzmq::Message, fbzmq::Error> processRequestMsgHelper(
      const std::string& requestId, thrift::KvStoreRequest& thriftReq);

  // Extracts the counters
  std::map<std::string, int64_t> getCounters() const;

  // Calculate size of KvStoreDB (just the key/val pairs)
  size_t getKeyValsSize() const;

  // get multiple keys at once
  thrift::Publication getKeyVals(std::vector<std::string> const& keys);

  // dump the entries of my KV store whose keys match the filter
  thrift::Publication dumpAllWithFilters(
      KvStoreFilters const& kvFilters,
      thrift::FilterOperator oper = thrift::FilterOperator::OR,
      bool doNotPublishValue = false) const;

  // dump the hashes of my KV store whose keys match the given prefix
  // if prefix is the empty sting, the full hash store is dumped
  thrift::Publication dumpHashWithFilters(
      KvStoreFilters const& kvFilters) const;

  // dump the keys on which hashes differ from given keyVals
  thrift::Publication dumpDifference(
      std::unordered_map<std::string, thrift::Value> const& myKeyVal,
      std::unordered_map<std::string, thrift::Value> const& reqKeyVal) const;

  // Merge received publication with local store and publish out the delta.
  // If senderId is set, will build <key:value> map from kvStore_ and
  // rcvdPublication.tobeUpdatedKeys and send back to senderId to update it
  // @return: Number of KV updates applied
  size_t mergePublication(
      thrift::Publication const& rcvdPublication,
      std::optional<std::string> senderId = std::nullopt);

  // update Time to expire filed in Publication
  // removeAboutToExpire: knob to remove keys which are about to expire
  // and hence do not want to include them. Constants::kTtlThreshold
  void updatePublicationTtl(
      thrift::Publication& thriftPub, bool removeAboutToExpire = false);

  // add new peers to sync with
  void addPeers(std::unordered_map<std::string, thrift::PeerSpec> const& peers);

  // delete some peers we are subscribed to
  void delPeers(std::vector<std::string> const& peers);

  // dump all peers we are subscribed to
  thrift::PeersMap dumpPeers();

  // thrift flavor of peer adding
  void addThriftPeers(
      std::unordered_map<std::string, thrift::PeerSpec> const& peers);

  // thrift flavor of peer deletion
  void delThriftPeers(std::vector<std::string> const& peers);

  // util funtion to fetch KvStorePeerState
  std::optional<thrift::KvStorePeerState> getCurrentState(
      std::string const& peerName);

  // process spanning-tree-set command to set/unset a child for a given root
  void processFloodTopoSet(
      const thrift::FloodTopoSetParams& setParams) noexcept;

  // get current snapshot of SPT(s) information
  thrift::SptInfos processFloodTopoGet() noexcept;

  // util function to fetch peer by its state
  std::vector<std::string> getPeersByState(thrift::KvStorePeerState state);

  inline bool
  getInitialSyncedWithPeers() const {
    return initialSyncCompleted_;
  }

  // util function for state transition
  static thrift::KvStorePeerState getNextState(
      std::optional<thrift::KvStorePeerState> const& currState,
      KvStorePeerEvent const& event);

  // add new key-vals to kvstore_'s key-vals
  void setKeyVals(thrift::KeySetParams&& setParams);

  // Set key-value in KvStore with specified version. If version is 0, the one
  // greater than the latest known will be used. KvStore will manage
  // ttl-refreshing for self-originated key-vals, aka, key-vals sent via queue.
  void setSelfOriginatedKey(
      std::string const& key, std::string const& value, uint32_t version);

  // Set specified key-value in KvStore. This is an authoratitive call, meaning
  // if someone else advertises the same key we try to win over it by setting
  // key-value with higher version. By default key is published to default area.
  // KvStore will manage ttl-refreshing for self-originated key-vals, aka,
  // key-vals sent via queue.
  void persistSelfOriginatedKey(
      std::string const& key, std::string const& value);

  // Set new value for self-originated key and stop ttl-refreshing by clearing
  // from local cache.
  void unsetSelfOriginatedKey(std::string const& key, std::string const& value);

  // Erase key from local cache, thus stopping ttl-refreshing.
  void eraseSelfOriginatedKey(std::string const& key);

 private:
  // disable copying
  KvStoreDb(KvStoreDb const&) = delete;
  KvStoreDb& operator=(KvStoreDb const&) = delete;

  // util function to log state transition
  void logStateTransition(
      std::string const& peerName,
      thrift::KvStorePeerState oldState,
      thrift::KvStorePeerState newState);

  // Process KvStore sync event in OpenR initialization procedure. A syncing
  // completion signal will be marked for either:
  // 1) achieving INITIALIZED state or
  // 2) running into THRIFT_API_ERROR
  void processInitializationEvent();

  // method to scan over thriftPeers to send full-dump request
  void requestThriftPeerSync();

  // util function to process when sync response received
  void processThriftSuccess(
      std::string const& peerName,
      thrift::Publication&& pub,
      std::chrono::milliseconds timeDelta);

  // util function to process when exception encountered
  void processThriftFailure(
      std::string const& peerName,
      folly::fbstring const& exceptionStr,
      std::chrono::milliseconds timeDelta);

  // send dual messages over syncSock
  bool sendDualMessages(
      const std::string& neighbor,
      const thrift::DualMessages& msgs) noexcept override;

  // send topology-set command to peer, peer will set/unset me as child
  // rootId: action will applied on given rootId
  // peerName: peer name
  // setChild: true if set, false if unset
  // allRoots: if true, rootId will be ignored, action will be applied to all
  //           roots. (currently used for initial unsetChildAll() cmd)
  void sendTopoSetCmd(
      const std::string& rootId,
      const std::string& peerName,
      bool setChild,
      bool allRoots) noexcept;

  // set child on given rootId
  void setChild(
      const std::string& rootId, const std::string& peerName) noexcept;

  // unset child on given rootId
  void unsetChild(
      const std::string& rootId, const std::string& peerName) noexcept;

  // unset child on all rootIds
  void unsetChildAll(const std::string& peerName) noexcept;

  // callbacks when nexthop changed for a given root-id
  void processNexthopChange(
      const std::string& rootId,
      const std::optional<std::string>& oldNh,
      const std::optional<std::string>& newNh) noexcept override;

  // get flooding peers for a given spt-root-id
  // if rootId is none => flood to all physical peers
  // else only flood to formed SPT-peers for rootId
  std::unordered_set<std::string> getFloodPeers(
      const std::optional<std::string>& rootId);

  // collect router-client send failure statistics in following form
  // "kvstore.send_failure.dst-peer-id.error-code"
  // error: fbzmq-Error
  // dstSockId: destination socket identity
  void collectSendFailureStats(
      const fbzmq::Error& error, const std::string& dstSockId);

  // Process the messages on peerSyncSock_
  void drainPeerSyncSock();

  // request full-sync (KEY_DUMP) with peersToSyncWith_
  void requestFullSyncFromPeers();

  // add new query entries into ttlCountdownQueue from publication
  // and Reschedule ttl expiry timer if needed
  void updateTtlCountdownQueue(const thrift::Publication& publication);

  // periodically count down and purge expired keys from CountdownQueue
  void cleanupTtlCountdownQueue();

  // Function to flood publication to neighbors
  // publication => data element to flood
  // rateLimit => if 'false', publication will not be rate limited
  // setFloodRoot => if 'false', floodRootId will not be set
  void floodPublication(
      thrift::Publication&& publication,
      bool rateLimit = true,
      bool setFloodRoot = true);

  // perform last step as a 3-way full-sync request
  // full-sync initiator sends back key-val to senderId (where we made
  // full-sync request to) who need to update those keys
  void finalizeFullSync(
      const std::unordered_set<std::string>& keys, const std::string& senderId);

  // [TO BE DEPRECATED]
  // process received KV_DUMP from one of our neighbor
  void processSyncResponse(
      const std::string& requestId, fbzmq::Message&& syncPubMsg) noexcept;

  // [TO BE DEPRECATED]
  // this will poll the sockets listening to the requests
  void attachCallbacks();

  // Submit full-sync event to monitor
  void logSyncEvent(
      const std::string& peerNodeName,
      const std::chrono::milliseconds syncDuration);

  // Submit events to monitor
  void logKvEvent(const std::string& event, const std::string& key);

  // buffer publications blocked by the rate limiter
  void bufferPublication(thrift::Publication&& publication);

  // flood pending update blocked by rate limiter
  void floodBufferedUpdates(void);

  // Send message via socket
  folly::Expected<size_t, fbzmq::Error> sendMessageToPeer(
      const std::string& peerSocketId, const thrift::KvStoreRequest& request);

  // update ttls for all self-originated key-vals
  void advertiseTtlUpdates();

  // schedule ttl updates for self-originated key-vals
  void scheduleTtlUpdates(std::string const& key, bool advertiseImmediately);

  void advertiseSelfOriginatedKeys(
      const std::unordered_set<std::string>& pendingKeysToAdvertise);
  //
  // Private variables
  //

  // Kv store parameters
  KvStoreParams& kvParams_;

  // area identified of this KvStoreDb instance
  const std::string area_;

  // area id tag for logging purpose
  const std::string areaTag_;

  // [TO BE DEPRECATED]
  // zmq ROUTER socket for requesting full dumps from peers
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_CLIENT> peerSyncSock_;

  //
  // Mutable state
  //

  // KvStore peer struct to convey peer information
  struct KvStorePeer {
    KvStorePeer(
        const std::string& nodeName,
        const std::string& areaTag,
        const thrift::PeerSpec& ps,
        const ExponentialBackoff<std::chrono::milliseconds>& expBackoff);

    // util function to create thrift client
    bool getOrCreateThriftClient(
        OpenrEventBase* evb, std::optional<int> maybeIpTos);

    // node name
    const std::string nodeName;

    // area tag
    const std::string areaTag;

    // peer spec(peerSpec can be modified as peerAddr can change)
    thrift::PeerSpec peerSpec;

    // exponetial backoff in case of retry after sync failure
    ExponentialBackoff<std::chrono::milliseconds> expBackoff;

    // thrift client for this peer
    std::unique_ptr<thrift::OpenrCtrlCppAsyncClient> client{nullptr};

    // timer to periodically send keep-alive status
    // ATTN: this mechanism serves the purpose of avoiding channel being
    //       closed from thrift server due to IDLE timeout(i.e. 60s by default)
    std::unique_ptr<folly::AsyncTimeout> keepAliveTimer{nullptr};

    // Stores set of keys that may have changed during initialization of this
    // peer. Will flood to them in finalizeFullSync(), the last step of initial
    // sync.
    std::unordered_set<std::string> pendingKeysDuringInitialization;

    // Number of occured Thrift API errors in the process of syncing with peer.
    int64_t numThriftApiErrors{0};
  };

  // Set of peers with all info over thrift channel
  std::unordered_map<std::string, KvStorePeer> thriftPeers_{};

  // [TO BE DEPRECATED]
  // The peers we will be talking to: both PUB and CMD URLs for each. We use
  // peerAddCounter_ to uniquely identify a peering session's socket-id.
  uint64_t peerAddCounter_{0};
  std::unordered_map<
      std::string /* node-name */,
      std::pair<thrift::PeerSpec, std::string /* socket-id */>>
      peers_;

  // [TO BE DEPRECATED]
  // set of peers to perform full sync from. We use exponential backoff to try
  // repetitively untill we succeeed (without overwhelming anyone with too
  // many requests).
  std::unordered_map<std::string, ExponentialBackoff<std::chrono::milliseconds>>
      peersToSyncWith_{};

  // [TO BE DEPRECATED]
  // Callback timer to get full KEY_DUMP from peersToSyncWith_
  std::unique_ptr<folly::AsyncTimeout> fullSyncTimer_;

  // [TO BE DEPRECATED]
  // the serializer/deserializer helper we'll be using
  apache::thrift::CompactSerializer serializer_;

  // Boolean flag indicating whether initial KvStoreDb sync with all peers
  // completed in OpenR initialization procedure.
  bool initialSyncCompleted_{false};

  // store keys mapped to (version, originatoId, value)
  std::unordered_map<std::string, thrift::Value> kvStore_;

  // TTL count down queue
  TtlCountdownQueue ttlCountdownQueue_;

  // TTL count down timer
  std::unique_ptr<folly::AsyncTimeout> ttlCountdownTimer_;

  // [TO BE DEPRECATED]
  // Map of latest peer sync up request send to each peer
  // this is used to measure full-dump sync time between this node and each of
  // its peers
  std::unordered_map<
      std::string,
      std::chrono::time_point<std::chrono::steady_clock>>
      latestSentPeerSync_;

  // Kvstore rate limiter
  std::unique_ptr<folly::BasicTokenBucket<>> floodLimiter_{nullptr};

  // timer to send pending kvstore publication
  std::unique_ptr<folly::AsyncTimeout> pendingPublicationTimer_{nullptr};

  // timer for requesting full-sync
  std::unique_ptr<folly::AsyncTimeout> requestSyncTimer_{nullptr};

  // timer to promote idle peers for initial syncing
  std::unique_ptr<folly::AsyncTimeout> thriftSyncTimer_{nullptr};

  // timer to advertise ttl updates for self-originated key-vals
  std::unique_ptr<folly::AsyncTimeout> selfOriginatedKeyTtlTimer_;

  // all self originated key-vals and their backoffs
  // persistKey and setKey will add, clearKey will remove
  std::unordered_map<std::string /* key */, SelfOriginatedValue>
      selfOriginatedKeyVals_;

  // pending keys to flood publication
  // map<flood-root-id: set<keys>>
  std::
      unordered_map<std::optional<std::string>, std::unordered_set<std::string>>
          publicationBuffer_{};

  // Callback function to signal KvStore that KvStoreDb sync with all peers are
  // completed.
  std::function<void()> initialKvStoreSyncedCallback_;

  // [TO BE DEPRECATED]
  // max parallel syncs allowed. It's initialized with '2' and doubles
  // up to a max value of kMaxFullSyncPendingCountThresholdfor each full sync
  // response received
  size_t parallelSyncLimit_{2};

  // max parallel syncs allowed
  size_t parallelSyncLimitOverThrift_{2};

  // event loop
  OpenrEventBase* evb_{nullptr};
};

// The class represent a server on either the thrift server port or the
// it listens for submission on REP socket. The configuration
// is passed via constructor arguments. This class instantiates KV Store DB
// for each area. The list of areas is passed in the constructor.
// The messages received are either sent to a specific instance of
// KvStore DB or to all instances.

class KvStore final : public OpenrEventBase {
 public:
  KvStore(
      // the zmq context to use for IO
      fbzmq::Context& zmqContext,
      // Queue for publishing kvstore updates
      messaging::ReplicateQueue<Publication>& kvStoreUpdatesQueue,
      // Queue for publishing kvstore peer initial sync events
      messaging::ReplicateQueue<KvStoreSyncEvent>& kvStoreSyncEventsQueue,
      // Queue for receiving peer updates
      messaging::RQueue<PeerEvent> peerUpdatesQueue,
      // Queue for receiving key-value update requests
      messaging::RQueue<KeyValueRequest> kvRequestQueue,
      // Queue for publishing the event log
      messaging::ReplicateQueue<LogSample>& logSampleQueue,
      // the url to receive command from peer instances
      KvStoreGlobalCmdUrl globalCmdUrl,
      // openr config
      std::shared_ptr<const Config> config);

  ~KvStore() override = default;

  // override stop() method of OpenrEventBase
  void stop() override;

  // Public APIs

  folly::SemiFuture<std::unique_ptr<thrift::Publication>> getKvStoreKeyVals(
      std::string area, thrift::KeyGetParams keyGetParams);

  folly::SemiFuture<folly::Unit> setKvStoreKeyVals(
      std::string area, thrift::KeySetParams keySetParams);

  // return publication for each area in selectAreas or all areas if select
  // areas is empty
  folly::SemiFuture<std::unique_ptr<std::vector<thrift::Publication>>>
  dumpKvStoreKeys(
      thrift::KeyDumpParams keyDumpParams,
      std::set<std::string> selectAreas = {});

  // return self-originated key-vals for given area
  folly::SemiFuture<std::unique_ptr<SelfOriginatedKeyVals>>
  dumpKvStoreSelfOriginatedKeys(std::string area);

  folly::SemiFuture<std::unique_ptr<thrift::Publication>> dumpKvStoreHashes(
      std::string area, thrift::KeyDumpParams keyDumpParams);

  folly::SemiFuture<std::unique_ptr<thrift::PeersMap>> getKvStorePeers(
      std::string area);

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::KvStoreAreaSummary>>>
  getKvStoreAreaSummaryInternal(std::set<std::string> selectAreas = {});

  folly::SemiFuture<folly::Unit> addUpdateKvStorePeers(
      std::string area, thrift::PeersMap peersToAdd);

  folly::SemiFuture<folly::Unit> deleteKvStorePeers(
      std::string area, std::vector<std::string> peersToDel);

  folly::SemiFuture<std::unique_ptr<thrift::SptInfos>> getSpanningTreeInfos(
      std::string area);

  folly::SemiFuture<folly::Unit> updateFloodTopologyChild(
      std::string area, thrift::FloodTopoSetParams floodTopoSetParams);

  folly::SemiFuture<folly::Unit> processKvStoreDualMessage(
      std::string area, thrift::DualMessages dualMessages);

  folly::SemiFuture<std::map<std::string, int64_t>> getCounters();

  // API to get reader for kvStoreUpdatesQueue
  messaging::RQueue<Publication> getKvStoreUpdatesReader();

  // API to fetch state of peerNode, used for unit-testing
  folly::SemiFuture<std::optional<thrift::KvStorePeerState>>
  getKvStorePeerState(std::string const& area, std::string const& peerName);

  // Callback function for KvStoreDb to indicate initial KvStore sync is
  // is done within configured area during Open/R initialization procedure.
  void initialKvStoreDbSynced();

 private:
  // disable copying
  KvStore(KvStore const&) = delete;
  KvStore& operator=(KvStore const&) = delete;

  //
  // Private methods
  //

  // [TO BE DEPRECATED]
  void prepareSocket(
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& socket,
      std::string const& url,
      std::optional<int> maybeIpTos = std::nullopt);

  // [TO BE DEPRECATED]
  void processCmdSocketRequest(std::vector<fbzmq::Message>&& req) noexcept;

  // [TO BE DEPRECATED]
  // This function wraps `processRequestMsgHelper` and updates send/received
  // bytes counters.
  folly::Expected<fbzmq::Message, fbzmq::Error> processRequestMsg(
      const std::string& requestId, fbzmq::Message&& msg);

  void processPeerUpdates(PeerEvent&& event);

  // Wrapper function that calls functions to update the kv store based on
  // type of update
  void processKeyValueRequest(KeyValueRequest&& kvRequest);

  std::map<std::string, int64_t> getGlobalCounters() const;

  void initGlobalCounters();

  // helper for public semifuture API. Returns a reference to the relevant
  // KvStoreDb (to be captured and operated on in this event loop) or throws an
  // instance of OpenrError
  // for backward compaytibilty, will allow getting single configured area if
  // default area is requested or is the only one configured
  // pass in id for caller for counting backward compatibility requests
  KvStoreDb& getAreaDbOrThrow(
      std::string const& areaId, std::string const& caller);

  //
  // Private variables
  //

  // Timer for updating and submitting counters periodically
  std::unique_ptr<folly::AsyncTimeout> counterUpdateTimer_{nullptr};

  // kvstore parameters common to all kvstoreDB
  KvStoreParams kvParams_;

  // map of area IDs and instance of KvStoreDb
  std::unordered_map<std::string /* area ID */, KvStoreDb> kvStoreDb_{};

  // the serializer/deserializer helper we'll be using
  apache::thrift::CompactSerializer serializer_;

  // Boolean flag to indicate if kvStoreSynced signal is published in OpenR
  // initialization process.
  bool initialSyncSignalSent_{false};
};
} // namespace openr
