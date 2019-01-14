/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KvStore.h"

#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/Random.h>
#include <folly/String.h>

#include <openr/common/Constants.h>
#include <openr/common/Util.h>

using namespace std::chrono_literals;
using namespace std::chrono;

namespace openr {

KvStoreFilters::KvStoreFilters(
    std::vector<std::string> const& keyPrefix,
    std::set<std::string> const& nodeIds)
    : keyPrefixList_(keyPrefix),
      originatorIds_(nodeIds) {
  // create re2 set
  keyPrefixObjList_ = std::make_unique<KeyPrefix>(keyPrefixList_);
}

bool KvStoreFilters::keyMatch(
    std::string const& key,
    thrift::Value const& value
  ) const {
  if (keyPrefixList_.empty() && originatorIds_.empty()) {
    return true;
  }
  if (!keyPrefixList_.empty() && keyPrefixObjList_->keyMatch(key)) {
    return true;
  }
  if (!originatorIds_.empty() &&
      originatorIds_.count(value.originatorId)) {
    return true;
  }
  return false;
}

std::vector<std::string> KvStoreFilters::getKeyPrefixes() const {
  return keyPrefixList_;
}

std::set<std::string> KvStoreFilters::getOrigniatorIdList() const {
  return originatorIds_;
}

KvStore::KvStore(
    // initializers for immutable state
    fbzmq::Context& zmqContext,
    std::string nodeId,
    KvStoreLocalPubUrl localPubUrl,
    KvStoreGlobalPubUrl globalPubUrl,
    KvStoreGlobalCmdUrl globalCmdUrl,
    MonitorSubmitUrl monitorSubmitUrl,
    folly::Optional<int> maybeIpTos,
    std::chrono::seconds dbSyncInterval,
    std::chrono::seconds monitorSubmitInterval,
    // initializer for mutable state
    std::unordered_map<std::string, thrift::PeerSpec> peers,
    bool legacyFlooding,
    folly::Optional<KvStoreFilters> filters,
    int zmqHwm,
    KvStoreFloodRate floodRate,
    std::chrono::milliseconds ttlDecr)
    : OpenrEventLoop(nodeId, thrift::OpenrModuleType::KVSTORE, zmqContext,
          std::string{globalCmdUrl}, folly::none /* ipcUrl */, maybeIpTos,
          zmqHwm),
      zmqContext_(zmqContext),
      nodeId_(std::move(nodeId)),
      localPubUrl_(std::move(localPubUrl)),
      globalPubUrl_(std::move(globalPubUrl)),
      dbSyncInterval_(dbSyncInterval),
      monitorSubmitInterval_(monitorSubmitInterval),
      legacyFlooding_(legacyFlooding),
      hwm_(zmqHwm),
      ttlDecr_(ttlDecr),
      filters_(std::move(filters)),
      // initialize zmq sockets
      localPubSock_{zmqContext},
      peerSyncSock_(
          zmqContext,
          fbzmq::IdentityString{folly::sformat(
              Constants::kPeerSyncIdTemplate.toString(), nodeId_)},
          folly::none,
          fbzmq::NonblockingFlag{true}),
      floodRate_(floodRate)  {
  CHECK(not nodeId_.empty());
  CHECK(not localPubUrl_.empty());
  CHECK(not globalPubUrl_.empty());

  // allocate new global pub socket if not provided
  globalPubSock_ = fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER>(
      zmqContext,
      fbzmq::IdentityString{
          folly::sformat(Constants::kGlobalPubIdTemplate.toString(), nodeId_)},
      folly::none,
      fbzmq::NonblockingFlag{true});

  if (legacyFlooding_) {
    peerSubSock_ = fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT>(
        zmqContext,
        fbzmq::IdentityString{folly::sformat(
            Constants::kGlobalSubIdTemplate.toString(), nodeId_)},
        folly::none,
        fbzmq::NonblockingFlag{true});
  }

  if (floodRate_.hasValue()) {
    floodLimiter_ = std::make_unique<folly::BasicTokenBucket<>>(
        floodRate_.value().first,    // messages per sec
        floodRate_.value().second);   // burst size
    pendingPublicationTimer_ =
        fbzmq::ZmqTimeout::make(this, [this]() noexcept {
            if(!floodLimiter_->consume(1)) {
              pendingPublicationTimer_->scheduleTimeout(
                  Constants::kFloodPendingPublication,
                  false);
                return;
            }
            floodBufferedUpdates();
    });
  }

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  // Schedule periodic timer for counters submission
  const bool isPeriodic = true;
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(monitorSubmitInterval_, isPeriodic);

  //
  // Set various socket options
  //

  // HWM for pub and peer sub sockets
  const auto localPubHwm =
      localPubSock_.setSockOpt(ZMQ_SNDHWM, &hwm_, sizeof(hwm_));
  if (localPubHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm_ << " "
               << localPubHwm.error();
  }
  const auto globalPubHwm =
      globalPubSock_.setSockOpt(ZMQ_SNDHWM, &hwm_, sizeof(hwm_));
  if (globalPubHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm_ << " "
               << globalPubHwm.error();
  }

  const auto peersSyncSndHwm =
      peerSyncSock_.setSockOpt(ZMQ_SNDHWM, &hwm_, sizeof(hwm_));
  if (peersSyncSndHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm_ << " "
               << peersSyncSndHwm.error();
  }
  const auto peerSyncRcvHwm =
      peerSyncSock_.setSockOpt(ZMQ_RCVHWM, &hwm_, sizeof(hwm_));
  if (peerSyncRcvHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm_ << " "
               << peerSyncRcvHwm.error();
  }

  // enable handover for inter process router socket
  const int handover = 1;
  const auto peerSyncHandover =
      peerSyncSock_.setSockOpt(ZMQ_ROUTER_HANDOVER, &handover, sizeof(int));
  if (peerSyncHandover.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_ROUTER_HANDOVER to " << handover << " "
               << peerSyncHandover.error();
  }

  // set keep-alive to retire old flows
  const auto peerSyncKeepAlive = peerSyncSock_.setKeepAlive(
      Constants::kKeepAliveEnable,
      Constants::kKeepAliveTime.count(),
      Constants::kKeepAliveCnt,
      Constants::kKeepAliveIntvl.count());
  if (peerSyncKeepAlive.hasError()) {
    LOG(FATAL) << "Error setting KeepAlive " << peerSyncKeepAlive.error();
  }

  if (maybeIpTos) {
    const int ipTos = *maybeIpTos;
    const auto globalPubTos =
        globalPubSock_.setSockOpt(ZMQ_TOS, &ipTos, sizeof(int));
    if (globalPubTos.hasError()) {
      LOG(FATAL) << "Error setting ZMQ_TOS to " << ipTos << " "
                 << globalPubTos.error();
    }
    const auto peerSyncTos =
        peerSyncSock_.setSockOpt(ZMQ_TOS, &ipTos, sizeof(int));
    if (peerSyncTos.hasError()) {
      LOG(FATAL) << "Error setting ZMQ_TOS to " << ipTos << " "
                 << peerSyncTos.error();
    }
    if (legacyFlooding_) {
      const auto peerSubTos =
          peerSubSock_.setSockOpt(ZMQ_TOS, &ipTos, sizeof(int));
      if (peerSubTos.hasError()) {
        LOG(FATAL) << "Error setting ZMQ_TOS to " << ipTos << " "
                   << peerSubTos.error();
      }
    }
  }

  //
  // Bind the sockets
  //
  VLOG(2) << "KvStore: Binding publisher and replier sockets.";

  // the following will throw exception if something is wrong
  VLOG(2) << "KvStore: Binding localPubUrl '" << localPubUrl_ << "'";
  const auto localPubBind = localPubSock_.bind(fbzmq::SocketUrl{localPubUrl_});
  if (localPubBind.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << localPubUrl_ << "' "
               << localPubBind.error();
  }

  VLOG(2) << "KvStore: Binding globalPubUrl '" << globalPubUrl_ << "'";
  const auto globalPubBind =
      globalPubSock_.bind(fbzmq::SocketUrl{globalPubUrl_});
  if (globalPubBind.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << globalPubUrl_ << "' "
               << globalPubBind.error();
  }


  // Subscribe to all messages
  if (legacyFlooding_) {
    auto const peerSyncSub = peerSubSock_.setSockOpt(ZMQ_SUBSCRIBE, "", 0);
    if (peerSyncSub.hasError()) {
      LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
                 << ""
                 << " " << peerSyncSub.error();
    }
  }

  // Attach socket callbacks/schedule events
  attachCallbacks();

  VLOG(2) << "Subscribing/connecting to all peers...";

  // Add all existing peers again. This will also ensure querying full dump
  // from each peer.
  addPeers(peers);

  // Hook up timer with cleanupTtlCountdownQueue(). The actual scheduling
  // happens within updateTtlCountdownQueue()
  ttlCountdownTimer_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
    cleanupTtlCountdownQueue();
  });
}

// static, public
std::unordered_map<std::string, thrift::Value>
KvStore::mergeKeyValues(
    std::unordered_map<std::string, thrift::Value>& kvStore,
    std::unordered_map<std::string, thrift::Value> const& keyVals,
    folly::Optional<KvStoreFilters> const& filters) {
  // the publication to build if we update our KV store
  std::unordered_map<std::string, thrift::Value> kvUpdates;

  // Counters for logging
  uint32_t ttlUpdateCnt{0}, valUpdateCnt{0};

  for (const auto& kv : keyVals) {
    auto const& key = kv.first;
    auto const& value = kv.second;

    if (
      filters.hasValue() &&
      not filters->keyMatch(kv.first, kv.second)) {
      VLOG(4) << "key: " << key << " not adding from "
          << value.originatorId;
      continue;
    }

    // versions must start at 1; setting this to zero here means
    // we would be beaten by any version supplied by the setter
    int64_t myVersion{0};
    int64_t newVersion = value.version;

    // Check if TTL is valid. It must be infinite or positive number
    // Skip if invalid!
    if (value.ttl != Constants::kTtlInfinity && value.ttl <= 0) {
      continue;
    }

    // if key exist, compare values first
    // if they are the same, no need to propagate changes
    auto kvStoreIt = kvStore.find(key);
    if (kvStoreIt != kvStore.end()) {
      myVersion = kvStoreIt->second.version;
    } else {
      VLOG(4) << "(mergeKeyValues) key: '" << key << "' not found, adding";
    }

    // If we get an old value just skip it
    if (newVersion < myVersion) {
      continue;
    }

    VLOG(4) << "(mergeKeyValues) key: '" << key << "' value: '"
            << (value.value.hasValue() ? "valid" : "null")
            << "' new version: " << newVersion << " old version: " << myVersion
            << " new TTL: " << value.ttl
            << " new originator: '" << value.originatorId
            << "' new TTL version: " << value.ttlVersion;
    VLOG_IF(5, value.value.hasValue())
        << "(mergeKeyValues) value: '" << key
        << folly::backslashify(value.value.value());

    bool updateAllNeeded{false};
    bool updateTtlNeeded{false};

    //
    // Check updateAll and updateTtl
    //
    if (value.value.hasValue()) {
      if (newVersion > myVersion) {
        // Version is newer or
        // kvStoreIt is NULL(myVersion is set to 0)
        updateAllNeeded = true;
      } else if (value.originatorId > kvStoreIt->second.originatorId) {
        // versions are the same but originatorId is higher
        updateAllNeeded = true;
      } else if (value.originatorId == kvStoreIt->second.originatorId) {
        // This can occur after kvstore restarts or simply reconnects after
        // disconnection. We let one of the two values win if they differ(higher
        // in this case but can be lower as long as it's deterministic).
        // Otherwise, local store can have new value while other stores have old
        // value and they never sync.
        int rc = (*value.value).compare(*kvStoreIt->second.value);
        if (rc > 0) {
          // versions and orginatorIds are same but value is higher
          VLOG(3) << "Previous incarnation reflected back for key " << key;
          updateAllNeeded = true;
        } else if (rc == 0) {
          // versions, orginatorIds, value are all same
          // retain higher ttlVersion
          if (value.ttlVersion > kvStoreIt->second.ttlVersion) {
            updateTtlNeeded = true;
          }
        }
      }
    }

    //
    // Check updateTtl
    //
    if (not value.value.hasValue() and
        kvStoreIt != kvStore.end() and
        value.version == kvStoreIt->second.version and
        value.originatorId == kvStoreIt->second.originatorId and
        value.ttlVersion > kvStoreIt->second.ttlVersion) {
      updateTtlNeeded = true;
    }

    if (!updateAllNeeded and !updateTtlNeeded) {
      VLOG(4) << "(mergeKeyValues) no need to update anything for key: '" << key
              << "'";
      continue;
    }

    // grab the new value (this will copy, intended)
    thrift::Value newValue = value;

    VLOG(4) << "(mergeKeyValues) Inserting/Updating key: '" << key << "'";

    if (updateAllNeeded) {
      ++valUpdateCnt;
      //
      // update everything for such key
      //
      CHECK(value.value.hasValue());
      if (kvStoreIt == kvStore.end()) {
        // create new entry
        std::tie(kvStoreIt, std::ignore) = kvStore.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key),
            std::forward_as_tuple(std::move(newValue)));
      } else {
        // update the entry in place, the old value will be destructed
        kvStoreIt->second = std::move(newValue);
      }
      // update hash if it's not there
      if (not kvStoreIt->second.hash.hasValue()) {
        kvStoreIt->second.hash = generateHash(
            value.version, value.originatorId, value.value);
      }
    } else if (updateTtlNeeded) {
      ++ttlUpdateCnt;
      //
      // update ttl,ttlVersion only
      //
      CHECK(kvStoreIt != kvStore.end());

      // update TTL only, nothing else
      kvStoreIt->second.ttl = value.ttl;
      kvStoreIt->second.ttlVersion = value.ttlVersion;
    }

    // announce the update
    kvUpdates.emplace(key, value);
  }

  VLOG(4) << "(mergeKeyValues) updating " << kvUpdates.size()
          << " keyvals. ValueUpdates: " << valUpdateCnt
          << ", TtlUpdates: " << ttlUpdateCnt;
  return kvUpdates;
}

void
KvStore::updateTtlCountdownQueue(
    const thrift::Publication& publication) {
  for (const auto& kv : publication.keyVals) {
    const auto& key = kv.first;
    const auto& value = kv.second;

    if (value.ttl != Constants::kTtlInfinity) {
      TtlCountdownQueueEntry queueEntry;
      queueEntry.expiryTime = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(value.ttl);
      queueEntry.key = key;
      queueEntry.version = value.version;
      queueEntry.ttlVersion = value.ttlVersion;
      queueEntry.originatorId = value.originatorId;

      if ((ttlCountdownQueue_.empty() or
           (queueEntry.expiryTime <= ttlCountdownQueue_.top().expiryTime)) and
          ttlCountdownTimer_) {
        // Reschedule the shorter timeout
        ttlCountdownTimer_->scheduleTimeout(
            std::chrono::milliseconds(value.ttl));
      }

      ttlCountdownQueue_.push(std::move(queueEntry));
    }
  }
}

// consume a publication pending on peerSubSock_ socket
// (i.e. announced by some of our peers) and relays the changes only
void
KvStore::processPublication() {
  CHECK(legacyFlooding_);

  auto thriftPubMsg = peerSubSock_.recvOne();
  if (thriftPubMsg.hasError()) {
    LOG(ERROR) << "processPublication: failed receiving publication "
               << thriftPubMsg.error();
    return;
  }

  auto maybeThriftPub =
      thriftPubMsg.value().readThriftObj<thrift::Publication>(serializer_);
  if (maybeThriftPub.hasError()) {
    LOG(ERROR) << "processPublication: failed reading thrift message "
               << maybeThriftPub.error();
    return;
  }
  mergePublication(maybeThriftPub.value());
}

// build publication out of the requested keys (per request)
// if not keys provided, will return publication with empty keyVals
thrift::Publication
KvStore::getKeyVals(std::vector<std::string> const& keys) {
  thrift::Publication thriftPub;

  for (auto const& key : keys) {
    // if requested key if found, respond with version and value
    auto it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      // copy here
      thriftPub.keyVals[key] = it->second;
    }
  }
  return thriftPub;
}

// dump the entries of my KV store whose keys match the given prefix
// if prefix is the empty string, the full KV store is dumped
thrift::Publication
KvStore::dumpAllWithFilters(
         KvStoreFilters const& kvFilters) const {
  thrift::Publication thriftPub;

  for (auto const& kv : kvStore_) {
    if (not kvFilters.keyMatch(kv.first, kv.second)) {
      continue;
    }
    thriftPub.keyVals[kv.first] = kv.second;
  }
  return thriftPub;
}

// dump the hashes of my KV store whose keys match the given prefix
// if prefix is the empty string, the full hash store is dumped
thrift::Publication
KvStore::dumpHashWithFilters(KvStoreFilters const& kvFilters) const {
  thrift::Publication thriftPub;
  for (auto const& kv : kvStore_) {
    if (not kvFilters.keyMatch(kv.first, kv.second)) {
      continue;
    }
    DCHECK(kv.second.hash.hasValue());
    auto& value = thriftPub.keyVals[kv.first];
    value.version = kv.second.version;
    value.originatorId = kv.second.originatorId;
    value.hash = kv.second.hash;
    value.ttl = kv.second.ttl;
    value.ttlVersion = kv.second.ttlVersion;
  }
  return thriftPub;
}

/**
 * Compare two values to find out which value is better
 * TODO: this function can be leveraged in mergeKeyValues to perform same
 * logic of which value if better to use
 */
int
KvStore::compareValues(const thrift::Value& v1, const thrift::Value& v2) {
  // compare version
  if (v1.version != v2.version) {
    return v1.version > v2.version ? 1 : -1;
  }

  // compare orginatorId
  if (v1.originatorId != v2.originatorId) {
    return v1.originatorId > v2.originatorId ? 1 : -1;
  }

  // compare value
  if (v1.hash.hasValue() and v2.hash.hasValue() and *v1.hash == *v2.hash) {
    // hashes are same => (version, orginatorId, value are same)
    // compare ttl-version
    if (v1.ttlVersion != v2.ttlVersion) {
      return v1.ttlVersion > v2.ttlVersion ? 1 : -1;
    } else {
      return 0;
    }
  }

  // can't use hash, either it's missing or they are different
  // compare values
  if (v1.value.hasValue() and v2.value.hasValue()) {
    return (*v1.value).compare(*v2.value);
  } else {
    // some value is missing
    return -2; // unknown
  }
}


// dump the keys on which hashes differ from given keyVals
// thriftPub.keyVals: better keys or keys exist only in MY-KEY-VAL
// thriftPub.tobeUpdatedKeys: better keys or keys exist only in REQ-KEY-VAL
// this way, full-sync initiator knows what keys need to send back to finish
// 3-way full-sync
thrift::Publication
KvStore::dumpDifference(
    std::unordered_map<std::string, thrift::Value> const& myKeyVal,
    std::unordered_map<std::string, thrift::Value> const& reqKeyVal) const {
  thrift::Publication thriftPub;

  thriftPub.tobeUpdatedKeys = std::vector<std::string>{};
  std::unordered_set<std::string> allKeys;
  for (const auto& kv : myKeyVal) {
    allKeys.insert(kv.first);
  }
  for (const auto& kv : reqKeyVal) {
    allKeys.insert(kv.first);
  }

  for (const auto& key : allKeys) {
    const auto& myKv = myKeyVal.find(key);
    const auto& reqKv = reqKeyVal.find(key);
    if (myKv == myKeyVal.end()) {
      // not exist in myKeyVal
      thriftPub.tobeUpdatedKeys->emplace_back(key);
      continue;
    }
    if (reqKv == reqKeyVal.end()) {
      // not exist in reqKeyVal
      thriftPub.keyVals.emplace(key, myKv->second);
      continue;
    }
    // common key
    const auto& myVal = myKv->second;
    const auto& reqVal = reqKv->second;
    int rc = compareValues(myVal, reqVal);
    if (rc == 1 or rc == -2) {
      // myVal is better or unknown
      thriftPub.keyVals.emplace(key, myVal);
    }
    if (rc == -1 or rc == -2) {
      // reqVal is better or unknown
      thriftPub.tobeUpdatedKeys->emplace_back(key);
    }
  }

  return thriftPub;
}

// add new peers to subscribe to
void
KvStore::addPeers(
    std::unordered_map<std::string, thrift::PeerSpec> const& peers) {
  ++peerAddCounter_;
  for (auto const& kv : peers) {
    auto const& peerName = kv.first;
    auto const& newPeerSpec = kv.second;
    auto const& newPeerId = folly::sformat(
        Constants::kGlobalCmdLocalIdTemplate.toString(),
        peerName,
        peerAddCounter_);

    try {
      auto it = peers_.find(peerName);
      bool pubUrlUpdated{false};
      bool cmdUrlUpdated{false};

      if (it != peers_.end()) {
        LOG(INFO) << "Updating existing peer " << peerName;
        auto& peerSpec = it->second.first;

        if (legacyFlooding_ && peerSpec.pubUrl != newPeerSpec.pubUrl) {
          pubUrlUpdated = true;
          LOG(INFO) << "Unsubscribing from " << peerSpec.pubUrl;
          auto const ret =
              peerSubSock_.disconnect(fbzmq::SocketUrl{peerSpec.pubUrl});
          if (ret.hasError()) {
            LOG(FATAL) << "Error Disconnecting to URL '" << peerSpec.pubUrl
                       << "' " << ret.error();
          }
        }

        if (peerSpec.cmdUrl != newPeerSpec.cmdUrl) {
          cmdUrlUpdated = true;
          LOG(INFO) << "Disconnecting from " << peerSpec.cmdUrl;
          const auto ret =
              peerSyncSock_.disconnect(fbzmq::SocketUrl{peerSpec.cmdUrl});
          if (ret.hasError()) {
            LOG(FATAL) << "Error Disconnecting to URL '" << peerSpec.cmdUrl
                       << "' " << ret.error();
          }
        }

        // Update entry with new data
        it->second.first = newPeerSpec;
        it->second.second = newPeerId;
      } else {
        LOG(INFO) << "Adding new peer " << peerName;
        pubUrlUpdated = true;
        cmdUrlUpdated = true;
        std::tie(it, std::ignore) = peers_.emplace(
            peerName, std::make_pair(newPeerSpec, newPeerId));
      }

      if (legacyFlooding_ && pubUrlUpdated) {
        LOG(INFO) << "Subscribing to " << newPeerSpec.pubUrl;
        if (peerSubSock_.connect(fbzmq::SocketUrl{newPeerSpec.pubUrl})
                .hasError()) {
          LOG(FATAL) << "Error connecting to URL '"
                     << newPeerSpec.pubUrl << "'";
        }
      }

      if (cmdUrlUpdated) {
        LOG(INFO) << "Connecting sync channel to " << newPeerSpec.cmdUrl;
        auto const optStatus = peerSyncSock_.setSockOpt(
            ZMQ_CONNECT_RID,
            newPeerId.data(),
            newPeerId.size());
        if (optStatus.hasError()) {
          LOG(FATAL) << "Error setting ZMQ_CONNECT_RID with value "
                     << newPeerId;
        }
        if (peerSyncSock_.connect(fbzmq::SocketUrl{newPeerSpec.cmdUrl})
                .hasError()) {
          LOG(FATAL) << "Error connecting to URL '"
                     << newPeerSpec.cmdUrl << "'";
        }
      }

      // Enqueue for full dump requests
      LOG(INFO) << "Enqueuing full dump request for peer " << peerName;
      peersToSyncWith_.emplace(
          peerName,
          ExponentialBackoff<std::chrono::milliseconds>(
            Constants::kInitialBackoff, Constants::kMaxBackoff));
    } catch (std::exception const& e) {
      LOG(ERROR) << "Error connecting to: `" << peerName
                 << "` reason: " << folly::exceptionStr(e);
    }
  }
  fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
}

// delete some peers we are subscribed to
void
KvStore::delPeers(std::vector<std::string> const& peers) {
  for (auto const& peerName : peers) {
    // not currently subscribed
    auto it = peers_.find(peerName);
    if (it == peers_.end()) {
      LOG(ERROR) << "Trying to delete non-existing peer '" << peerName << "'";
      continue;
    }

    auto const& peerSpec = it->second.first;

    if (legacyFlooding_) {
      LOG(INFO) << "Unsubscribing from: " << peerSpec.pubUrl;
      auto subRes = peerSubSock_.disconnect(fbzmq::SocketUrl{peerSpec.pubUrl});
      if (subRes.hasError()) {
        LOG(ERROR) << "Failed to unsubscribe. " << subRes.error();
      }
    }

    LOG(INFO) << "Detaching from: " << peerSpec.cmdUrl;
    auto syncRes = peerSyncSock_.disconnect(fbzmq::SocketUrl{peerSpec.cmdUrl});
    if (syncRes.hasError()) {
      LOG(ERROR) << "Failed to detach. " << syncRes.error();
    }

    peersToSyncWith_.erase(peerName);
    peers_.erase(it);
  }
}

// Get full KEY_DUMP from peersToSyncWith_
void
KvStore::requestFullSyncFromPeers() {
  // minimal timeout for next run
  auto timeout = std::chrono::milliseconds(0);

  // Make requests
  for (auto it = peersToSyncWith_.begin(); it != peersToSyncWith_.end();) {
    auto& peerName = it->first;
    auto& expBackoff = it->second;

    if (not expBackoff.canTryNow()) {
      timeout = std::min(timeout, expBackoff.getTimeRemainingUntilRetry());
      ++it;
      continue;
    }

    // Generate and send router-socket id of peer first. If the kvstore of
    // peer is not connected over the router socket then it will error out
    // exception and we will retry again.
    auto const& peerCmdSocketId = peers_.at(peerName).second;

    thrift::Request dumpRequest;
    dumpRequest.cmd = thrift::Command::KEY_DUMP;
    if (filters_.hasValue()) {
      std::string keyPrefix =
        folly::join(",", filters_.value().getKeyPrefixes());
      dumpRequest.keyDumpParams.prefix = keyPrefix;
      dumpRequest.keyDumpParams.originatorIds =
        filters_.value().getOrigniatorIdList();
    }
    std::set<std::string> originator{};
    std::vector<std::string> keyPrefixList{};
    KvStoreFilters kvFilters{keyPrefixList, originator};
    dumpRequest.keyDumpParams.keyValHashes =
      std::move(dumpHashWithFilters(kvFilters).keyVals);
    VLOG(1) << "Sending full sync request to peer " << peerName << " using id "
            << peerCmdSocketId;
    latestSentPeerSync_.emplace(
      peerCmdSocketId, std::chrono::steady_clock::now());

    auto const ret = peerSyncSock_.sendMultiple(
        fbzmq::Message::from(peerCmdSocketId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(dumpRequest, serializer_).value());

    if (ret.hasError()) {
      // this could be pretty common on initial connection setup
      LOG(ERROR) << "Failed to send full sync request to peer " << peerName
                 << " using id " << peerCmdSocketId << " (will try again). "
                 << ret.error();
      expBackoff.reportError(); // Apply exponential backoff
      timeout = std::min(timeout, expBackoff.getTimeRemainingUntilRetry());
      ++it;
    } else {
      // Remove the iterator
      it = peersToSyncWith_.erase(it);
    }
  } // for

  // We should be able to perfom full dump from all peers. Log warning if
  // there
  // are still some peers to sync with.
  if (not peersToSyncWith_.empty()) {
    LOG(WARNING) << peersToSyncWith_.size() << " peers still require sync."
                 << "Scheduling retry after " << timeout.count() << "ms.";
    // schedule next timeout
    fullSyncTimer_->scheduleTimeout(timeout);
  }
}

// dump all peers we are subscribed to
thrift::PeerCmdReply
KvStore::dumpPeers() {
  thrift::PeerCmdReply reply;
  for (auto const& kv : peers_) {
    reply.peers.emplace(kv.first, kv.second.first);
  }
  return reply;
}

// update TTL with remainng time to expire, TTL version remains
// same so existing keys will not be updated with this TTL
void
KvStore::updatePublicationTtl(
    thrift::Publication& thriftPub,
    bool removeAboutToExpire) {
  auto timeNow = std::chrono::steady_clock::now();
  for (const auto& qE : ttlCountdownQueue_) {
    // Find key and ensure we are taking time from right entry from queue
    auto kv = thriftPub.keyVals.find(qE.key);
    if (kv == thriftPub.keyVals.end() or
        kv->second.version != qE.version or
        kv->second.originatorId != qE.originatorId or
        kv->second.ttlVersion != qE.ttlVersion) {
      continue;
    }

    // Compute timeLeft and do sanity check on it
    auto timeLeft = duration_cast<milliseconds>(qE.expiryTime - timeNow);
    if (timeLeft <= ttlDecr_) {
      thriftPub.keyVals.erase(kv);
      continue;
    }

    // filter key from publication if time left is below ttl threshold
    if (removeAboutToExpire and timeLeft < Constants::kTtlThreshold) {
      thriftPub.keyVals.erase(kv);
      continue;
    }

    // Set the time-left and decrement it by one so that ttl decrement
    // deterministically whenever it is exchanged between KvStores. This will
    // avoid looping of updates between stores.
    kv->second.ttl = timeLeft.count() - ttlDecr_.count();
  }
}

// process a request
folly::Expected<fbzmq::Message, fbzmq::Error>
KvStore::processRequestMsg(fbzmq::Message&& request) {

  auto maybeThriftReq =
      request.readThriftObj<thrift::Request>(serializer_);

  if (maybeThriftReq.hasError()) {
    LOG(ERROR) << "processRequest: failed reading thrift::Request "
               << maybeThriftReq.error();
    return folly::makeUnexpected(fbzmq::Error());
  }
  auto& thriftReq = maybeThriftReq.value();

  VLOG(3)
      << "processRequest: command: `"
      << apache::thrift::TEnumTraits<thrift::Command>::findName(thriftReq.cmd)
      << "` received";

  std::vector<std::string> keys;
  switch (thriftReq.cmd) {
  case thrift::Command::KEY_SET: {
    VLOG(3) << "Set key requested";
    tData_.addStatValue("kvstore.cmd_key_set", 1, fbzmq::COUNT);
    if (thriftReq.keySetParams.keyVals.empty()) {
      LOG(ERROR) << "Malformed set request, ignoring";
      return folly::makeUnexpected(fbzmq::Error());
    }

    // Update hash for key-values
    for (auto& kv : thriftReq.keySetParams.keyVals) {
      auto& value = kv.second;
      if (value.value.hasValue()) {
        value.hash = generateHash(
            value.version, value.originatorId, value.value);
      }
    }

    // Create publication and merge it with local KvStore
    thrift::Publication rcvdPublication;
    rcvdPublication.keyVals = std::move(thriftReq.keySetParams.keyVals);
    rcvdPublication.nodeIds = std::move(thriftReq.keySetParams.nodeIds);
    mergePublication(rcvdPublication);

    // respond to the client
    if (thriftReq.keySetParams.solicitResponse) {
      return fbzmq::Message::from(
          Constants::kSuccessResponse.toString());
    }
    return fbzmq::Message();
  }
  case thrift::Command::KEY_GET: {
    VLOG(3) << "Get key-values requested";
    tData_.addStatValue("kvstore.cmd_key_get", 1, fbzmq::COUNT);

    auto thriftPub = getKeyVals(thriftReq.keyGetParams.keys);
    updatePublicationTtl(thriftPub);
    return fbzmq::Message::fromThriftObj(thriftPub, serializer_);
  }
  case thrift::Command::KEY_DUMP: {
    if (thriftReq.keyDumpParams.keyValHashes.hasValue()) {
      VLOG(3) << "Dump keys requested along with "
              << thriftReq.keyDumpParams.keyValHashes.value().size()
              << " keyValHashes item(s) provided from peer";
    } else {
      VLOG(3) << "Dump all keys requested - "
              << "KeyPrefixes:"
              << thriftReq.keyDumpParams.prefix
              << " Originator IDs:"
              << folly::join(",", thriftReq.keyDumpParams.originatorIds);
    }
    // TODO, add per request id counters in thrift server
    tData_.addStatValue("kvstore.cmd_key_dump", 1, fbzmq::COUNT);

    std::vector<std::string> keyPrefixList;
    folly::split(",", thriftReq.keyDumpParams.prefix, keyPrefixList, true);
    const auto keyPrefixMatch = KvStoreFilters(
        keyPrefixList, thriftReq.keyDumpParams.originatorIds);
    auto thriftPub = dumpAllWithFilters(keyPrefixMatch);
    if (thriftReq.keyDumpParams.keyValHashes.hasValue()) {
      thriftPub = dumpDifference(
        thriftPub.keyVals,
        thriftReq.keyDumpParams.keyValHashes.value());
    }
    updatePublicationTtl(thriftPub);
    return fbzmq::Message::fromThriftObj(thriftPub, serializer_);
  }
  case thrift::Command::HASH_DUMP: {
    VLOG(3) << "Dump all hashes requested";
    tData_.addStatValue("kvstore.cmd_hash_dump", 1, fbzmq::COUNT);
    std::set<std::string> originator{};
    std::vector<std::string> keyPrefixList{};
    folly::split(",", thriftReq.keyDumpParams.prefix, keyPrefixList, true);
    KvStoreFilters kvFilters{keyPrefixList, originator};
    auto hashDump = dumpHashWithFilters(kvFilters);
    updatePublicationTtl(hashDump);
    return fbzmq::Message::fromThriftObj(hashDump, serializer_);
  }
  case thrift::Command::COUNTERS_GET: {
    VLOG(3) << "Counters are requested";
    fbzmq::thrift::CounterValuesResponse counters{
      apache::thrift::FRAGILE,
      getCounters()};
    return fbzmq::Message::fromThriftObj(counters, serializer_);
  }
  case thrift::Command::PEER_ADD: {
    VLOG(2) << "Peer addition requested";
    tData_.addStatValue("kvstore.cmd_peer_add", 1, fbzmq::COUNT);

    if (thriftReq.peerAddParams.peers.empty()) {
      LOG(ERROR) << "Malformed peer-add request, ignoring";
      return folly::makeUnexpected(fbzmq::Error());
    }
    addPeers(thriftReq.peerAddParams.peers);
    return fbzmq::Message::fromThriftObj(dumpPeers(), serializer_);
  }
  case thrift::Command::PEER_DEL: {
    VLOG(2) << "Peer deletion requested";
    tData_.addStatValue("kvstore.cmd_per_del", 1, fbzmq::COUNT);

    if (thriftReq.peerDelParams.peerNames.empty()) {
      LOG(ERROR) << "Malformed peer-del request, ignoring";
      return folly::makeUnexpected(fbzmq::Error());
    }
    delPeers(thriftReq.peerDelParams.peerNames);
    return fbzmq::Message::fromThriftObj(dumpPeers(), serializer_);
  }
  case thrift::Command::PEER_DUMP: {
    VLOG(2) << "Peer dump requested";
    tData_.addStatValue("kvstore.cmd_peer_dump", 1, fbzmq::COUNT);
    return fbzmq::Message::fromThriftObj(dumpPeers(), serializer_);
  }
  default: {
    LOG(ERROR) << "Unknown command received";
    return folly::makeUnexpected(fbzmq::Error());
  }
  }
}

void
KvStore::processSyncResponse() noexcept {
  VLOG(4) << "awaiting for sync response message";

  fbzmq::Message requestIdMsg, delimMsg, syncPubMsg;

  auto ret = peerSyncSock_.recvMultiple(requestIdMsg, delimMsg, syncPubMsg);
  if (ret.hasError()) {
    LOG(ERROR) << "processSyncResponse: failed processing syncRespone: "
               << ret.error();
    return;
  }

  // at this point we received all three parts
  if (not delimMsg.empty()) {
    LOG(ERROR) << "processSyncResponse: unexpected delimiter: "
               << delimMsg.read<std::string>().value();
    return;
  }

  auto const requestId = requestIdMsg.read<std::string>().value();

  // syncPubMsg can be of two types
  // 1. ack to SET_KEY ("OK" or "ERR")
  // 2. response of DUMP_KEY (thrift::Publication)
  // We check for first one and then fallback to second one
  if (syncPubMsg.size() < 3) {
    auto syncPubStr = syncPubMsg.read<std::string>().value();
    if (syncPubStr == Constants::kErrorResponse) {
      LOG(ERROR) << "Got error for sent publication from " << requestId;
      return;
    }
    if (syncPubStr == Constants::kSuccessResponse) {
      VLOG(2) << "Got ack for sent publication on " << requestId;
      return;
    }
  }

  // Perform error check
  auto maybeSyncPub =
      syncPubMsg.readThriftObj<thrift::Publication>(serializer_);
  if (maybeSyncPub.hasError()) {
    LOG(ERROR) << "Received bad response on peerSyncSock_";
    return;
  }

  const auto& syncPub = maybeSyncPub.value();
  const size_t kvUpdateCnt = mergePublication(syncPub, requestId);
  LOG(INFO) << "Sync response received from " << requestId << " with "
            << syncPub.keyVals.size() << " key value pairs which incured "
            << kvUpdateCnt << " key-value updates";

  if (latestSentPeerSync_.count(requestId)) {
    auto syncDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - latestSentPeerSync_.at(requestId));
    tData_.addStatValue(
        "kvstore.peer_sync_time_ms", syncDuration.count(), fbzmq::AVG);
    VLOG(1) << "It took " << syncDuration.count()
            << " ms to sync with " << requestId;
    latestSentPeerSync_.erase(requestId);
  }
}

// send sync request from one neighbor randomly
void
KvStore::requestSync() {
  SCOPE_EXIT {
    auto base = dbSyncInterval_.count();
    std::default_random_engine generator;
    // add 20% variance
    std::uniform_int_distribution<int> distribution(-0.2 * base, 0.2 * base);
    auto roll = std::bind(distribution, generator);
    auto period = std::chrono::milliseconds((base + roll()) * 1000);

    // Schedule next sync with peers
    scheduleTimeout(period, [this]() { requestSync(); });
  };

  if (peers_.empty()) {
    return;
  }

  // Randomly select one neighbor to request full-dump from
  int randomIndex = folly::Random::rand32() % peers_.size();
  int index{0};
  std::string randomNeighbor;

  for (auto const& kv : peers_) {
    if (index++ == randomIndex) {
      randomNeighbor = kv.first;
      break;
    }
  }

  // Enqueue neighbor for full-sync (insert only if entry doesn't exists)
  LOG(INFO) << "Requesting periodic sync from " << randomNeighbor;
  peersToSyncWith_.emplace(
      randomNeighbor,
      ExponentialBackoff<std::chrono::milliseconds>(
          Constants::kInitialBackoff, Constants::kMaxBackoff));

  // initial full sync request if peersToSyncWith_ was empty
  if (not fullSyncTimer_->isScheduled()) {
    fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  }
}

// this will poll the sockets listening to the requests
void
KvStore::attachCallbacks() {
  VLOG(2) << "KvStore: Registering events callbacks ...";

  if (legacyFlooding_) {
    addSocket(
      fbzmq::RawZmqSocketPtr{*peerSubSock_}, ZMQ_POLLIN, [this](int) noexcept {
        // we received a publication
        VLOG(3) << "KvStore: Publication received...";
        try {
          processPublication();
        } catch (std::exception const& err) {
          LOG(ERROR) << "KvStore: Error processing publication, "
                     << folly::exceptionStr(err);
        }
      });
  }

  addSocket(
      fbzmq::RawZmqSocketPtr{*peerSyncSock_}, ZMQ_POLLIN, [this](int) noexcept {
        // we received a sync response
        VLOG(3) << "KvStore: sync response received";
        processSyncResponse();
      });

  // Perform full sync if there are peers to sync with.
  fullSyncTimer_ = fbzmq::ZmqTimeout::make(
      this, [this]() noexcept { requestFullSyncFromPeers(); });

  // Schedule periodic call to re-sync with one of our peer
  scheduleTimeout(
      std::chrono::milliseconds(0), [this]() noexcept { requestSync(); });
}

void
KvStore::cleanupTtlCountdownQueue() {
  // record all expired keys
  std::vector<std::string> expiredKeys;
  auto now = std::chrono::steady_clock::now();

  // Iterate through ttlCountdownQueue_ until the top expires in the future
  while (not ttlCountdownQueue_.empty()) {
    auto top = ttlCountdownQueue_.top();
    if (top.expiryTime > now) {
      // Nothing in queue worth evicting
      break;
    }
    auto it = kvStore_.find(top.key);
    if (it != kvStore_.end() and
        it->second.version == top.version and
        it->second.originatorId == top.originatorId and
        it->second.ttlVersion == top.ttlVersion) {
      expiredKeys.emplace_back(top.key);
      LOG(WARNING)
        << "Delete expired (key, version, originatorId, ttlVersion, node) "
        << folly::sformat(
              "({}, {}, {}, {}, {})",
              top.key, it->second.version,
              it->second.originatorId, it->second.ttlVersion, nodeId_);
      logKvEvent("KEY_EXPIRE", top.key);
      kvStore_.erase(it);
    }
    ttlCountdownQueue_.pop();
  }

  // Reschedule based on most recent timeout
  if (not ttlCountdownQueue_.empty()) {
    ttlCountdownTimer_->scheduleTimeout(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ttlCountdownQueue_.top().expiryTime - now));
  }

  if (expiredKeys.empty()) {
    // no key expires
    return;
  }
  tData_.addStatValue(
      "kvstore.expired_key_vals", expiredKeys.size(), fbzmq::SUM);
  thrift::Publication expiredKeysPub{};
  expiredKeysPub.expiredKeys = std::move(expiredKeys);
  floodPublication(std::move(expiredKeysPub));
}

void KvStore::bufferPublication(thrift::Publication&& publication) {
  tData_.addStatValue(
      "kvstore.rate_limit_suppress", 1, fbzmq::COUNT);
  tData_.addStatValue(
      "kvstore.rate_limit_keys", publication.keyVals.size(), fbzmq::AVG);
  // update or add keys
  for (auto const& key: publication.keyVals) {
    publicationBuffer_[key.first] = publication.nodeIds.value();
  }
  for (auto const& key: publication.expiredKeys) {
    if (publication.nodeIds.hasValue()) {
      publicationBuffer_[key] = publication.nodeIds.value();
    }
  }
}

void
KvStore::floodBufferedUpdates() {
  if (!publicationBuffer_.size()) {
    return;
  }
  thrift::Publication publication{};
  for (auto& keyVal: publicationBuffer_) {
    auto kvStoreIt = kvStore_.find(keyVal.first);
    if (kvStoreIt != kvStore_.end()) {
      publication.keyVals.emplace(make_pair(keyVal.first, kvStoreIt->second));
    } else {
      publication.expiredKeys.emplace_back(keyVal.first);
    }
    publication.nodeIds = keyVal.second;
  }
  publicationBuffer_.clear();
  return floodPublication(std::move(publication), false);
}

void
KvStore::finalizeFullSync(
    const std::vector<std::string>& keys,
    const std::string& senderId) {
  if (keys.empty()) {
    return;
  }
  VLOG(1) << " finalizeFullSync back to: " << senderId << " with keys: "
          << folly::join(",", keys);

  // build keyval to be sent
  std::unordered_map<std::string, thrift::Value> keyVals;
  for (const auto& key : keys) {
    const auto& it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      keyVals.emplace(key, it->second);
    }
  }

  thrift::Request updateRequest;
  updateRequest.cmd = thrift::Command::KEY_SET;
  updateRequest.keySetParams.keyVals = std::move(keyVals);
  updateRequest.keySetParams.solicitResponse = false;

  VLOG(1) << "sending finalizeFullSync back to " << senderId;
  auto const ret = peerSyncSock_.sendMultiple(
      fbzmq::Message::from(senderId).value(),
      fbzmq::Message(),
      fbzmq::Message::fromThriftObj(updateRequest, serializer_).value());
  if (ret.hasError()) {
    // this could fail when senderId goes offline
    LOG(ERROR) << "Failed to send finalizeFullSync to " << senderId
               << " using id " << senderId;
  }
}

void
KvStore::floodPublication(thrift::Publication&& publication, bool rateLimit) {
  // rate limit if configured
  if (floodLimiter_ && rateLimit && !floodLimiter_->consume(1)) {
    bufferPublication(std::move(publication));
    pendingPublicationTimer_->scheduleTimeout(
        Constants::kFloodPendingPublication,
        false);
    return;
  }
  // merge with buffered publication and flood
  if (publicationBuffer_.size()) {
    bufferPublication(std::move(publication));
    return floodBufferedUpdates();
  }
  // Update ttl on keys we are trying to advertise. Also remove keys which
  // are about to expire.
  updatePublicationTtl(publication, true);

  // If there are no changes then return
  if (publication.keyVals.empty() && publication.expiredKeys.empty()) {
    return;
  }

  // Find from whom we might have got this publication. Last entry is our ID
  // and hence second last entry is the node from whom we get this publication
  folly::Optional<std::string> senderId;
  if (publication.nodeIds.hasValue() and publication.nodeIds->size()) {
    senderId = publication.nodeIds->back();
  }
  if (not publication.nodeIds.hasValue()) {
    publication.nodeIds = std::vector<std::string>{};
  }
  publication.nodeIds->emplace_back(nodeId_);

  // Flood publication on local PUB socket
  //
  // Usually only local subscribers need to know, but we are also sending
  // on global socket so that it can help debugging things via breeze as
  // well as preserve backward compatibility
  auto const msg =
      fbzmq::Message::fromThriftObj(publication, serializer_).value();
  localPubSock_.sendOne(msg);
  globalPubSock_.sendOne(msg);

  //
  // Create request and send only keyValue updates to all neighbors
  //
  if (publication.keyVals.empty()) {
    return;
  }
  thrift::Request floodRequest;
  floodRequest.cmd = thrift::Command::KEY_SET;
  floodRequest.keySetParams.keyVals = publication.keyVals;
  floodRequest.keySetParams.solicitResponse = false;
  floodRequest.keySetParams.nodeIds = publication.nodeIds;
  for (auto& kv : peers_) {
    if (senderId.hasValue() && senderId.value() == kv.first) {
      // Do not flood towards senderId from whom we received this publication
      continue;
    }
    VLOG(4) << "Forwarding publication, received from: "
            << (senderId.hasValue() ? senderId.value() : "N/A")
            << ", to: " << kv.first
            << ", via: " << nodeId_;

    tData_.addStatValue(
        "kvstore.sent_publications", 1, fbzmq::COUNT);
    tData_.addStatValue(
        "kvstore.sent_key_vals", publication.keyVals.size(), fbzmq::SUM);

    // Send flood request
    auto const& peerCmdSocketId = kv.second.second;
    auto const ret = peerSyncSock_.sendMultiple(
        fbzmq::Message::from(peerCmdSocketId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(floodRequest, serializer_).value());
    if (ret.hasError()) {
      // this could be pretty common on initial connection setup
      LOG(ERROR) << "Failed to flood publication to peer " << kv.first
                 << " using id " << peerCmdSocketId;
    }
  }
}

size_t
KvStore::mergePublication(
    const thrift::Publication& rcvdPublication,
    folly::Optional<std::string> senderId) {
  // Add counters
  tData_.addStatValue("kvstore.received_publications", 1, fbzmq::COUNT);
  tData_.addStatValue(
      "kvstore.received_key_vals", rcvdPublication.keyVals.size(), fbzmq::SUM);

  const bool needFinalizeFullSync =
      senderId.hasValue() and
      rcvdPublication.tobeUpdatedKeys.hasValue() and
      not rcvdPublication.tobeUpdatedKeys->empty();

  // This can happen when KvStore is emitting expired-key updates
  if (rcvdPublication.keyVals.empty() and not needFinalizeFullSync) {
    return 0;
  }

  // Check for loop
  const auto& nodeIds = rcvdPublication.nodeIds;
  if (nodeIds.hasValue() and
      std::find(nodeIds->begin(), nodeIds->end(), nodeId_) != nodeIds->end()) {
    tData_.addStatValue("kvstore.looped_publications", 1, fbzmq::COUNT);
    return 0;
  }

  // Generate delta with local KvStore
  thrift::Publication deltaPublication(
      apache::thrift::FRAGILE,
      mergeKeyValues(kvStore_, rcvdPublication.keyVals, filters_),
      {}  /* expired keys */,
      {}  /* nodeIds */,
      {}  /* tobeUpdatedKeys */);

  const size_t kvUpdateCnt = deltaPublication.keyVals.size();
  tData_.addStatValue("kvstore.updated_key_vals", kvUpdateCnt, fbzmq::SUM);

  // Populate nodeIds and our nodeId_ to the end
  if (rcvdPublication.nodeIds.hasValue()) {
    deltaPublication.nodeIds = rcvdPublication.nodeIds;

  }

  // Update ttl values of keys
  updateTtlCountdownQueue(deltaPublication);

  if (not deltaPublication.keyVals.empty()) {
    // Flood change to all of our neighbors/subscribers
    floodPublication(std::move(deltaPublication));
  }

  // response to senderId with tobeUpdatedKeys + Vals
  // (last step in 3-way full-sync)
  if (needFinalizeFullSync) {
    finalizeFullSync(*rcvdPublication.tobeUpdatedKeys, *senderId);
  }

  return kvUpdateCnt;
}

fbzmq::thrift::CounterMap
KvStore::getCounters() {
  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();

  // Add some more flat counters
  counters["kvstore.num_keys"] = kvStore_.size();
  counters["kvstore.num_peers"] = peers_.size();
  counters["kvstore.pending_full_sync"] = peersToSyncWith_.size();
  counters["kvstore.zmq_event_queue_size"] = getEventQueueSize();

  return prepareSubmitCounters(std::move(counters));
}

void
KvStore::submitCounters() {
  VLOG(3) << "Submitting counters ... ";
  zmqMonitorClient_->setCounters(getCounters());
}

void
KvStore::logKvEvent(const std::string& event, const std::string& key) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("entity", "KvStore");
  sample.addString("node_name", nodeId_);
  sample.addString("key", key);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

}// namespace openr
