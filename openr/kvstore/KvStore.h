/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>

#include <boost/heap/priority_queue.hpp>
#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Optional.h>
#include <folly/TokenBucket.h>
#include <folly/io/IOBuf.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrEventLoop.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/dual/Dual.h>
#include <openr/if/gen-cpp2/Dual_types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

namespace openr {

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

using TtlCountdownQueue = boost::heap::priority_queue<
    TtlCountdownQueueEntry,
    // Always returns smallest first
    boost::heap::compare<std::greater<TtlCountdownQueueEntry>>,
    boost::heap::stable<true>>;

// Kvstore flooding rate <messages/sec, burst size>
using KvStoreFloodRate = folly::Optional<std::pair<const size_t, const size_t>>;

class KvStoreFilters {
 public:
  // takes the list of comma separated key prefixes to match,
  // and the list of originator IDs to match in the value
  explicit KvStoreFilters(
      std::vector<std::string> const& keyPrefix,
      std::set<std::string> const& originatorIds);

  // Check if key matches the filters
  bool keyMatch(std::string const& key, thrift::Value const& value) const;

  // return comma separeated string prefix
  std::vector<std::string> getKeyPrefixes() const;

  // return set of origninator IDs
  std::set<std::string> getOrigniatorIdList() const;

 private:
  // list of string prefixes, empty list matches all keys
  std::vector<std::string> keyPrefixList_{};

  // set of node IDs to match, empty set matches all nodes
  std::set<std::string> originatorIds_{};

  // keyPrefix class to create RE2 set and to match keys
  std::unique_ptr<KeyPrefix> keyPrefixObjList_{};
};

// The class represent a server that stores KV pairs in internal map.
// it listens for submission on REP socket, subscribes to peers via
// SUB socket, and publishes to peers via PUB socket. The configuration
// is passed via constructor arguments.

class KvStore final : public OpenrEventLoop, public DualNode {
 public:
  KvStore(
      // the zmq context to use for IO
      fbzmq::Context& zmqContext,
      // the name of this node (unique in domain)
      std::string nodeId,
      // the url we use to publish our updates to
      // local subscribers
      KvStoreLocalPubUrl localPubUrl,
      // the url we use to publish our updates to
      // any subscriber (often encrypted)
      KvStoreGlobalPubUrl globalPubUrl,
      // the url to receive command from local and
      // non local clients (often encrypted channel)
      KvStoreGlobalCmdUrl globalCmdUrl,
      // the url to submit to monitor
      MonitorSubmitUrl monitorSubmitUrl,
      // IP TOS value to set on sockets using TCP
      folly::Optional<int> ipTos,
      // how often to request full db sync from peers
      std::chrono::seconds dbSyncInterval,
      // how often to submit to monitor
      std::chrono::seconds monitorSubmitInterval,
      // initial list of peers to connect to
      std::unordered_map<std::string, thrift::PeerSpec> peers,
      // Enable legacy flooding
      bool legacyFlooding = true,
      // KvStore key filters
      folly::Optional<KvStoreFilters> filters = folly::none,
      // ZMQ high water mark
      int zmqHwm = Constants::kHighWaterMark,
      // Kvstore flooding rate
      KvStoreFloodRate floodRate = folly::none,
      // TTL decrement factor
      std::chrono::milliseconds ttlDecr = Constants::kTtlDecrement,
      bool enableFloodOptimization = false,
      bool isFloodRoot = false,
      bool useFloodOptimization = false);

  // process the key-values publication, and attempt to
  // merge it in existing map (first argument)
  // Return a publication made out of the updated values
  static std::unordered_map<std::string, thrift::Value> mergeKeyValues(
      std::unordered_map<std::string, thrift::Value>& kvStore,
      std::unordered_map<std::string, thrift::Value> const& update,
      folly::Optional<KvStoreFilters> const& filters = folly::none);

  // compare two thrift::Values to figure out which value is better to
  // use, it will compare following attributes in order
  // <version>, <orginatorId>, <value>, <ttl-version>
  // return 1 if v1 is better,
  //       -1 if v2 is better,
  //        0 if equal,
  //       -2 if unknown
  // unknown can happen if value is missing (only hash is provided)
  static int compareValues(const thrift::Value& v1, const thrift::Value& v2);

 private:
  // disable copying
  KvStore(KvStore const&) = delete;
  KvStore& operator=(KvStore const&) = delete;

  //
  // Private methods
  //

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
      const folly::Optional<std::string>& oldNh,
      const folly::Optional<std::string>& newNh) noexcept override;

  // get flooding peers for a given spt-root-id
  // if rootId is none => flood to all physical peers
  // else only flood to formed SPT-peers for rootId
  std::unordered_set<std::string> getFloodPeers(
      const folly::Optional<std::string>& rootId);

  // collect router-client send failure statistics in following form
  // "kvstore.send_failure.dst-peer-id.error-code"
  // error: fbzmq-Error
  // dstSockId: destination socket identity
  void collectSendFailureStats(
      const fbzmq::Error& error, const std::string& dstSockId);

  // consume a publication pending on sub_ socket
  // (i.e. announced by some of our peers)
  // relays the original publication if needed
  void processPublication();

  // get multiple keys at once
  thrift::Publication getKeyVals(std::vector<std::string> const& keys);

  // dump the entries of my KV store whose keys match the given prefix
  // if prefix is the empty sting, the full KV store is dumped
  thrift::Publication dumpAllWithFilters(KvStoreFilters const& kvFilters) const;

  // dump the hashes of my KV store whose keys match the given prefix
  // if prefix is the empty sting, the full hash store is dumped
  thrift::Publication dumpHashWithFilters(
      KvStoreFilters const& kvFilters) const;

  // dump the keys on which hashes differ from given keyVals
  thrift::Publication dumpDifference(
      std::unordered_map<std::string, thrift::Value> const& myKeyVal,
      std::unordered_map<std::string, thrift::Value> const& reqKeyVal) const;

  // add new peers to sync with
  void addPeers(std::unordered_map<std::string, thrift::PeerSpec> const& peers);

  // delete some peers we are subscribed to
  void delPeers(std::vector<std::string> const& peers);

  // request full-sync (KEY_DUMP) with peersToSyncWith_
  void requestFullSyncFromPeers();

  // dump all peers we are subscribed to
  thrift::PeerCmdReply dumpPeers();

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

  // update Time to expire filed in Publication
  // removeAboutToExpire: knob to remove keys which are about to expire
  // and hence do not want to include them. Constants::kTtlThreshold
  void updatePublicationTtl(
      thrift::Publication& thriftPub, bool removeAboutToExpire = false);

  // perform last step as a 3-way full-sync request
  // full-sync initiator sends back key-val to senderId (where we made
  // full-sync request to) who need to update those keys
  void finalizeFullSync(
      const std::vector<std::string>& keys, const std::string& senderId);

  // Merge received publication with local store and publish out the delta.
  // If senderId is set, will build <key:value> map from kvStore_ and
  // rcvdPublication.tobeUpdatedKeys and send back to senderId to update it
  // @return: Number of KV updates applied
  size_t mergePublication(
      thrift::Publication const& rcvdPublication,
      folly::Optional<std::string> senderId = folly::none);

  // process a request pending on cmdSock socket
  void processRequest(
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& cmdSock) noexcept;

  folly::Expected<fbzmq::Message, fbzmq::Error> processRequestMsg(
      fbzmq::Message&& msg) override;

  // process spanning-tree-set command to set/unset a child for a given root
  void processFloodTopoSet(
      const thrift::FloodTopoSetParams& setParams) noexcept;

  // get current snapshot of SPT(s) information
  thrift::SptInfos processFloodTopoGet() noexcept;

  // process received KV_DUMP from one of our neighbor
  void processSyncResponse() noexcept;

  // randomly request sync from one connected neighbor
  void requestSync();

  // this will poll the sockets listening to the requests
  void attachCallbacks();

  // count number of prefixes in kvstore
  int getPrefixCount() const;

  // Extracts the counters and submit them to monitor
  fbzmq::thrift::CounterMap getCounters();
  void submitCounters();

  // Submit events to monitor
  void logKvEvent(const std::string& event, const std::string& key);

  // buffer publications blocked by the rate limiter
  void bufferPublication(thrift::Publication&& publication);

  // flood pending update blocked by rate limiter
  void floodBufferedUpdates(void);
  //
  // Private variables
  //

  //
  // Immutable state
  //

  // zmq context
  fbzmq::Context& zmqContext_;

  // unique among all nodes, identifies this particular node
  const std::string nodeId_;

  // we only encrypt inter-node traffic and don't encrypt intra-node traffic
  // tcp*_ sockets are used to communicate with external node
  // inproc*_ sockets within a node

  // The ZMQ URL we'll be using for publications
  const std::string localPubUrl_;
  const std::string globalPubUrl_;

  // The ZMQ URL we'll be listening for commands on
  const std::string globalCmdUrl_;

  // base interval to run syncs with (jitter will be added)
  const std::chrono::seconds dbSyncInterval_;

  // Interval to submit to monitor. Default value is high
  // to avoid submission of counters in testing.
  const std::chrono::seconds monitorSubmitInterval_;

  // Enable legacy flooding using ZMQ PUB-SUB. If set to false then flooding
  // will be done via KvStore's one way SET_KEY requests and publications will
  // be sent out old way for internal clients for functioning of Open/R and
  // external clients for debugging purpose only
  const bool legacyFlooding_{true};

  // ZMQ high water mark for PUB sockets
  const int hwm_{openr::Constants::kHighWaterMark};

  // TTL decrement at flooding publications
  const std::chrono::milliseconds ttlDecr_{1};

  // Dual parameters
  const bool enableFloodOptimization_{false};
  const bool isFloodRoot_{false};
  const bool useFloodOptimization_{false};

  //
  // Mutable state
  //

  // The peers we will be talking to: both PUB and CMD URLs for each. We use
  // peerAddCounter_ to uniquely identify a peering session's socket-id.
  uint64_t peerAddCounter_{0};
  std::unordered_map<
      std::string /* node-name */,
      std::pair<thrift::PeerSpec, std::string /* socket-id */>>
      peers_;

  // key/value filters
  folly::Optional<KvStoreFilters> filters_;

  // the socket to publish changes to kv-store
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> localPubSock_;
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> globalPubSock_;

  // DEPRECATED: Use of sub socket will be removed soon for flooding between
  // KvStores.
  // the socket we use to subscribe to other KvStores
  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> peerSubSock_;

  // zmq ROUTER socket for requesting full dumps from peers
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_CLIENT> peerSyncSock_;

  // set of peers to perform full sync from. We use exponential backoff to try
  // repetitively untill we succeeed (without overwhelming anyone with too
  // many requests).
  std::unordered_map<std::string, ExponentialBackoff<std::chrono::milliseconds>>
      peersToSyncWith_{};

  // Callback timer to get full KEY_DUMP from peersToSyncWith_
  std::unique_ptr<fbzmq::ZmqTimeout> fullSyncTimer_;

  // the serializer/deserializer helper we'll be using
  apache::thrift::CompactSerializer serializer_;

  // store keys mapped to (version, originatoId, value)
  std::unordered_map<std::string, thrift::Value> kvStore_;

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_;

  // TTL count down queue
  TtlCountdownQueue ttlCountdownQueue_;

  // TTL count down timer
  std::unique_ptr<fbzmq::ZmqTimeout> ttlCountdownTimer_;

  // Data-struct for maintaining stats/counters
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

  // Map of latest peer sync up request send to each peer
  // this is used to measure full-dump sync time between this node and each of
  // its peers
  std::unordered_map<
      std::string,
      std::chrono::time_point<std::chrono::steady_clock>>
      latestSentPeerSync_;

  // Kvstore rate limiter
  std::unique_ptr<folly::BasicTokenBucket<>> floodLimiter_{nullptr};

  // Kvstore flooding rate
  KvStoreFloodRate floodRate_ = folly::none;

  // timer to send pending kvstore publication
  std::unique_ptr<fbzmq::ZmqTimeout> pendingPublicationTimer_{nullptr};

  // pending keys to flood publication
  // map<flood-root-id: set<keys>>
  std::unordered_map<
      folly::Optional<std::string>,
      std::unordered_set<std::string>>
      publicationBuffer_{};
};

} // namespace openr
