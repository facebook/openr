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

namespace openr {

KvStore::KvStore(
    // initializers for immutable state
    fbzmq::Context& zmqContext,
    std::string nodeId,
    KvStoreLocalPubUrl localPubUrl,
    KvStoreGlobalPubUrl globalPubUrl,
    KvStoreLocalCmdUrl localCmdUrl,
    KvStoreGlobalCmdUrl globalCmdUrl,
    MonitorSubmitUrl monitorSubmitUrl,
    folly::Optional<int> maybeIpTos,
    folly::Optional<fbzmq::KeyPair> keyPair,
    std::chrono::seconds dbSyncInterval,
    std::chrono::seconds monitorSubmitInterval,
    // initializer for mutable state
    std::unordered_map<std::string, thrift::PeerSpec> peers,
    // initializer for optionals
    folly::Optional<fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER>>
        preBoundGlobalPubSock /* = folly::none */,
    folly::Optional<fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>>
        preBoundGlobalCmdSock /* = folly::none */)
    : zmqContext_(zmqContext),
      nodeId_(std::move(nodeId)),
      localPubUrl_(std::move(localPubUrl)),
      globalPubUrl_(std::move(globalPubUrl)),
      localCmdUrl_(std::move(localCmdUrl)),
      globalCmdUrl_(std::move(globalCmdUrl)),
      dbSyncInterval_(dbSyncInterval),
      monitorSubmitInterval_(monitorSubmitInterval),
      peers_(std::move(peers)),
      // initialize zmq sockets
      localPubSock_{zmqContext},
      peerSubSock_{zmqContext,
                   fbzmq::IdentityString{folly::sformat(
                       Constants::kGlobalSubIdTemplate, nodeId_)},
                   keyPair},
      localCmdSock_{zmqContext,
                    fbzmq::IdentityString{folly::sformat(
                        Constants::kLocalCmdIdTemplate, nodeId_)},
                    keyPair,
                    fbzmq::NonblockingFlag{true}},
      peerSyncSock_{zmqContext,
                    fbzmq::IdentityString{folly::sformat(
                        Constants::kPeerSyncIdTemplate, nodeId_)},
                    keyPair,
                    fbzmq::NonblockingFlag{true}} {
  CHECK(not nodeId_.empty());
  CHECK(not localPubUrl_.empty());
  CHECK(not globalPubUrl_.empty());
  CHECK(not localCmdUrl_.empty());
  CHECK(not globalCmdUrl_.empty());

  if (preBoundGlobalPubSock) {
    globalPubSock_ = std::move(*preBoundGlobalPubSock);
  } else {
    // allocate new global pub socket if not provided
    globalPubSock_ = fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER>(
        zmqContext,
        fbzmq::IdentityString{
            folly::sformat(Constants::kGlobalPubIdTemplate, nodeId_)},
        keyPair,
        fbzmq::NonblockingFlag{true});
  }

  if (preBoundGlobalCmdSock) {
    globalCmdSock_ = std::move(*preBoundGlobalCmdSock);
  } else {
    // allocate new global cmd socket if not provided
    globalCmdSock_ = fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>(
        zmqContext,
        fbzmq::IdentityString{
            folly::sformat(Constants::kGlobalCmdIdTemplate, nodeId_)},
        keyPair,
        fbzmq::NonblockingFlag{true});
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
  const int hwm = Constants::kHighWaterMark;
  const auto localPubHwm =
      localPubSock_.setSockOpt(ZMQ_SNDHWM, &hwm, sizeof(int));
  if (localPubHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm << " "
               << localPubHwm.error();
  }
  const auto globalPubHwm =
      globalPubSock_.setSockOpt(ZMQ_SNDHWM, &hwm, sizeof(int));
  if (globalPubHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm << " "
               << globalPubHwm.error();
  }

  const auto localCmdSndHwm =
      localCmdSock_.setSockOpt(ZMQ_SNDHWM, &hwm, sizeof(int));
  if (localCmdSndHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm << " "
               << localCmdSndHwm.error();
  }
  const auto globalCmdSndHwm =
      globalCmdSock_.setSockOpt(ZMQ_SNDHWM, &hwm, sizeof(int));
  if (globalCmdSndHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm << " "
               << globalCmdSndHwm.error();
  }
  const auto peersSyncSndHwm =
      peerSyncSock_.setSockOpt(ZMQ_SNDHWM, &hwm, sizeof(int));
  if (peersSyncSndHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm << " "
               << peersSyncSndHwm.error();
  }
  const auto localCmdRcvHwm =
      localCmdSock_.setSockOpt(ZMQ_RCVHWM, &hwm, sizeof(int));
  if (localCmdRcvHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm << " "
               << localCmdRcvHwm.error();
  }
  const auto globalCmdRcvHwm =
      globalCmdSock_.setSockOpt(ZMQ_RCVHWM, &hwm, sizeof(int));
  if (globalCmdRcvHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm << " "
               << globalCmdRcvHwm.error();
  }
  const auto peerSyncRcvHwm =
      peerSyncSock_.setSockOpt(ZMQ_RCVHWM, &hwm, sizeof(int));
  if (peerSyncRcvHwm.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SNDHWM to " << hwm << " "
               << peerSyncRcvHwm.error();
  }

  // Set send timeout to avoid sockets from being in hanging state forever.
  const int sendTimeout = 1000;  // 1s
  {
    const auto sockOptRet =
        globalCmdSock_.setSockOpt(ZMQ_SNDTIMEO, &sendTimeout, sizeof(int));
    if (sockOptRet.hasError()) {
      LOG(FATAL) << "Error setting ZMQ_SNDTIMEO to " << sendTimeout << " "
                 << sockOptRet.error();
    }
  }
  {
    const auto sockOptRet =
        localCmdSock_.setSockOpt(ZMQ_SNDTIMEO, &sendTimeout, sizeof(int));
    if (sockOptRet.hasError()) {
      LOG(FATAL) << "Error setting ZMQ_SNDTIMEO to " << sendTimeout << " "
                 << sockOptRet.error();
    }
  }

  // enable handover for inter process router socket
  const int handover = 1;
  const auto globalCmdHandover =
      globalCmdSock_.setSockOpt(ZMQ_ROUTER_HANDOVER, &handover, sizeof(int));
  if (globalCmdHandover.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_ROUTER_HANDOVER to " << handover << " "
               << globalCmdHandover.error();
  }
  const auto peerSyncHandover =
      peerSyncSock_.setSockOpt(ZMQ_ROUTER_HANDOVER, &handover, sizeof(int));
  if (peerSyncHandover.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_ROUTER_HANDOVER to " << handover << " "
               << peerSyncHandover.error();
  }

  // set keep-alive to retire old flows
  const auto globalCmdKeepAlive = globalCmdSock_.setKeepAlive(
      Constants::kKeepAliveEnable,
      Constants::kKeepAliveTime.count(),
      Constants::kKeepAliveCnt,
      Constants::kKeepAliveIntvl.count());
  if (globalCmdKeepAlive.hasError()) {
    LOG(FATAL) << "Error setting KeepAlive " << globalCmdKeepAlive.error();
  }
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
    const auto globalCmdTos =
        globalCmdSock_.setSockOpt(ZMQ_TOS, &ipTos, sizeof(int));
    if (globalCmdTos.hasError()) {
      LOG(FATAL) << "Error setting ZMQ_TOS to " << ipTos << " "
                 << globalCmdTos.error();
    }
    const auto peerSyncTos =
        peerSyncSock_.setSockOpt(ZMQ_TOS, &ipTos, sizeof(int));
    if (peerSyncTos.hasError()) {
      LOG(FATAL) << "Error setting ZMQ_TOS to " << ipTos << " "
                 << peerSyncTos.error();
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
  if (!preBoundGlobalPubSock) {
    VLOG(2) << "KvStore: Binding globalPubUrl '" << globalPubUrl_ << "'";
    const auto globalPubBind =
        globalPubSock_.bind(fbzmq::SocketUrl{globalPubUrl_});
    if (globalPubBind.hasError()) {
      LOG(FATAL) << "Error binding to URL '" << globalPubUrl_ << "' "
                 << globalPubBind.error();
    }
  }

  VLOG(2) << "KvStore: Binding localCmdUrl '" << localCmdUrl_ << "'";
  const auto localCmdBind = localCmdSock_.bind(fbzmq::SocketUrl{localCmdUrl_});
  if (localCmdBind.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << localCmdUrl_ << "' "
               << localCmdBind.error();
  }
  if (!preBoundGlobalCmdSock) {
    VLOG(2) << "KvStore: Binding globalCmdUrl '" << globalCmdUrl_ << "'";
    const auto globalCmdBind =
        globalCmdSock_.bind(fbzmq::SocketUrl{globalCmdUrl_});
    if (globalCmdBind.hasError()) {
      LOG(FATAL) << "Error binding to URL '" << globalCmdUrl_ << "' "
                 << globalCmdBind.error();
    }
  }

  // Subscribe to all messages
  auto const peerSyncSub = peerSubSock_.setSockOpt(ZMQ_SUBSCRIBE, "", 0);
  if (peerSyncSub.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
               << ""
               << " " << peerSyncSub.error();
  }

  // Attach socket callbacks/schedule events
  attachCallbacks();

  VLOG(2) << "Subscribing/connecting to all peers...";

  // Copy local peers into new object
  std::unordered_map<std::string, thrift::PeerSpec> peersToAdd{};
  peers_.swap(peersToAdd); // peers_ is empty now

  // Add all existing peers again. This will also ensure querying full dump
  // from each peer.
  addPeers(peersToAdd);

  // Hook up timer with countdownTtl(). The actual scheduling happens within
  // mergeKeyValues()
  ttlCountdownTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { countdownTtl(); });
}

// static, public
thrift::Publication
KvStore::mergeKeyValues(
    std::unordered_map<std::string, thrift::Value>& kvStore,
    std::unordered_map<std::string, thrift::Value> const& keyVals) {
  // the publication to build if we update our KV store
  thrift::Publication thriftPub{};

  for (const auto& kv : keyVals) {
    auto const& key = kv.first;
    auto const& value = kv.second;

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
      //
      // update ttl,ttlVersion only
      //
      CHECK(kvStoreIt != kvStore.end());

      // update TTL only, nothing else
      kvStoreIt->second.ttl = value.ttl;
      kvStoreIt->second.ttlVersion = value.ttlVersion;
    }

    // announce the update
    thriftPub.keyVals.emplace(key, value);
  }

  return thriftPub;
}

void KvStore::updateTtlCountdownQueue(
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

  auto const thriftPub = maybeThriftPub.value();

  // Add counters
  tData_.addStatValue("kvstore.received_publications", 1, fbzmq::COUNT);
  tData_.addStatValue(
      "kvstore.received_key_vals", thriftPub.keyVals.size(), fbzmq::SUM);

  // This can happen when KvStore is emitting expired-key updates
  if (thriftPub.keyVals.empty()) {
    VLOG(3) << "Received publication with empty keyVals.";
    return;
  }

  // If the following publication is not empty, means we received some
  // new information and we haven't processed this publication before.
  // If so, relay it, else stop
  auto deltaPub = mergeKeyValues(kvStore_, thriftPub.keyVals);
  updateTtlCountdownQueue(deltaPub);
  tData_.addStatValue(
      "kvstore.updated_key_vals", deltaPub.keyVals.size(), fbzmq::SUM);

  // Now relay the changes only (if any). These publications can be generated by
  // clients attached to remote KvStores or a sync between two peers. In case
  // of later we really don't want to forward the full publication as it can
  // have old key-vals in it
  if (not deltaPub.keyVals.empty()) {
    auto const msg =
        fbzmq::Message::fromThriftObj(deltaPub, serializer_).value();
    localPubSock_.sendOne(msg);
    globalPubSock_.sendOne(msg);
  }
}

// build publication out of the requested keys (per request)
// if not keys provided, will return publication with empty keyVals
thrift::Publication
KvStore::getKeyVals(std::vector<std::string> const& keys) {
  thrift::Publication thriftPub;

  DCHECK(!keys.empty()) << "getKeyVals with empty key vector";

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
KvStore::dumpAllWithPrefix(std::string const& prefix) const {
  thrift::Publication thriftPub;
  for (auto const& kv : kvStore_) {
    if (kv.first.compare(0, prefix.length(), prefix) != 0) {
      continue;
    }
    thriftPub.keyVals[kv.first] = kv.second;
  }
  return thriftPub;
}

// dump the hashes of my KV store whose keys match the given prefix
// if prefix is the empty string, the full hash store is dumped
thrift::Publication
KvStore::dumpHashWithPrefix(std::string const& prefix) const {
  thrift::Publication thriftPub;
  for (auto const& kv : kvStore_) {
    if (kv.first.compare(0, prefix.length(), prefix) != 0) {
      continue;
    }
    CHECK(kv.second.hash.hasValue());
    auto& value = thriftPub.keyVals[kv.first];
    value.version = kv.second.version;
    value.originatorId = kv.second.originatorId;
    value.hash = kv.second.hash;
    value.ttl = kv.second.ttl;
    value.ttlVersion = kv.second.ttlVersion;
  }
  return thriftPub;
}

// add new peers to subscribe to
void
KvStore::addPeers(
    std::unordered_map<std::string, thrift::PeerSpec> const& peers) {
  for (auto const& kv : peers) {
    auto const& peerName = kv.first;
    auto const& peerSpec = kv.second;

    try {
      auto it = peers_.find(peerName);
      bool pubUrlUpdated{false};
      bool cmdUrlUpdated{false};

      if (it != peers_.end()) {
        LOG(INFO) << "Updating existing peer " << peerName;

        // If public key is different then we need to update both sockets.
        bool hasKeyChanged = (it->second.publicKey != peerSpec.publicKey);
        if (hasKeyChanged) {
          LOG(INFO) << "Public key of peer " << peerName << " has changed.";
        }

        if (hasKeyChanged || it->second.pubUrl != peerSpec.pubUrl) {
          pubUrlUpdated = true;
          LOG(INFO) << "Unsubscribing from " << peerSpec.pubUrl;
          auto const ret =
              peerSubSock_.disconnect(fbzmq::SocketUrl{it->second.pubUrl});
          if (ret.hasError()) {
            LOG(FATAL) << "Error Disconnecting to URL '" << it->second.pubUrl
                       << "' " << ret.error();
          }
        }

        if (hasKeyChanged || it->second.cmdUrl != peerSpec.cmdUrl) {
          cmdUrlUpdated = true;
          LOG(INFO) << "Disconnecting from " << peerSpec.cmdUrl;
          const auto ret =
              peerSyncSock_.disconnect(fbzmq::SocketUrl{it->second.cmdUrl});
          if (ret.hasError()) {
            LOG(FATAL) << "Error Disconnecting to URL '" << it->second.cmdUrl
                       << "' " << ret.error();
          }
        }

        // Update entry with new data
        it->second = peerSpec;
      } else {
        LOG(INFO) << "Adding new peer " << peerName;

        pubUrlUpdated = true;
        cmdUrlUpdated = true;

        // Insert entry for peer into peers_
        std::tie(it, std::ignore) = peers_.emplace(peerName, peerSpec);
      }

      if (pubUrlUpdated) {
        LOG(INFO) << "Subscribing to " << peerSpec.pubUrl;
        if (peerSpec.publicKey.length() == crypto_sign_ed25519_PUBLICKEYBYTES) {
          const auto peerSyncAddKey = peerSubSock_.addServerKey(
              fbzmq::SocketUrl{peerSpec.pubUrl},
              fbzmq::PublicKey{peerSpec.publicKey});
          if (peerSyncAddKey.hasError()) {
            LOG(FATAL) << "Error adding ServerKey " << peerSyncAddKey.error();
          }
        }
        if (peerSubSock_.connect(fbzmq::SocketUrl{peerSpec.pubUrl})
                .hasError()) {
          LOG(FATAL) << "Error connecting to URL '" << peerSpec.pubUrl << "'";
        }
      }

      if (cmdUrlUpdated) {
        LOG(INFO) << "Connecting sync channel to " << peerSpec.cmdUrl;
        if (peerSpec.publicKey.length() == crypto_sign_ed25519_PUBLICKEYBYTES) {
          const auto peerSyncAddKey = peerSyncSock_.addServerKey(
              fbzmq::SocketUrl{peerSpec.cmdUrl},
              fbzmq::PublicKey{peerSpec.publicKey});
          if (peerSyncAddKey.hasError()) {
            LOG(FATAL) << "Error adding ServerKey " << peerSyncAddKey.error();
          }
        }
        if (peerSyncSock_.connect(fbzmq::SocketUrl{peerSpec.cmdUrl})
                .hasError()) {
          LOG(FATAL) << "Error connecting to URL '" << peerSpec.cmdUrl << "'";
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

  // Request immediate full sync from peers after adding peers
  requestFullSyncFromPeers();
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

    auto const& peerSpec = it->second;

    LOG(INFO) << "Unsubscribing from: " << peerSpec.pubUrl;
    auto subRes = peerSubSock_.disconnect(fbzmq::SocketUrl{peerSpec.pubUrl});
    if (subRes.hasError()) {
      LOG(ERROR) << "Failed to unsubscribe. " << subRes.error();
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
  auto timeout = std::chrono::milliseconds(Constants::kMaxBackoff);

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
    auto const peerCmdSocketId =
        folly::sformat(Constants::kGlobalCmdIdTemplate, peerName);

    thrift::Request dumpRequest;
    dumpRequest.cmd = thrift::Command::KEY_DUMP;

    VLOG(1) << "Sending full sync request to peer " << peerName << " using id "
            << peerCmdSocketId;

    auto const ret = peerSyncSock_.sendMultiple(
        fbzmq::Message::from(peerCmdSocketId).value(),
        fbzmq::Message(),
        fbzmq::Message::fromThriftObj(dumpRequest, serializer_).value());

    if (ret.hasError()) {
      // this could be pretty common on initial connection setup
      VLOG(2) << "Failed to send full sync request to peer " << peerName
              << " using id " << peerCmdSocketId << " (will try again). "
              << ret.error();
      expBackoff.reportError(); // Apply exponential backoff
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
    VLOG(5) << peersToSyncWith_.size() << " peers still require sync.";
    // schedule next timeout
    fullSyncTimer_->scheduleTimeout(timeout);
  }
}

// dump all peers we are subscribed to
thrift::PeerCmdReply
KvStore::dumpPeers() {
  thrift::PeerCmdReply reply;
  reply.peers.insert(peers_.begin(), peers_.end());
  return reply;
}

// process a request pending on cmd_ socket
void
KvStore::processRequest(
    fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& cmdSock) noexcept {
  fbzmq::Message requestIdMsg, delimMsg, thriftReqMsg;

  const auto ret = cmdSock.recvMultiple(requestIdMsg, delimMsg, thriftReqMsg);

  if (ret.hasError()) {
    LOG(ERROR) << "processRequest: Error receiving command: " << ret.error();
    return;
  }

  const auto requestId = requestIdMsg.read<std::string>().value();
  const auto delim = delimMsg.read<std::string>().value();

  if (not delimMsg.empty()) {
    LOG(ERROR) << "processRequest: Non-empty delimiter: " << delim;
    return;
  }

  VLOG(4) << "processRequest, got id: `" << folly::backslashify(requestId)
          << "` and delim: `" << folly::backslashify(delim) << "`";

  const auto maybeThriftReq =
      thriftReqMsg.readThriftObj<thrift::Request>(serializer_);

  if (maybeThriftReq.hasError()) {
    LOG(ERROR) << "processRequest: failed reading thrift::Request "
               << maybeThriftReq.error();
    return;
  }

  const auto thriftReq = maybeThriftReq.value();

  VLOG(3)
      << "processRequest: command: `"
      << apache::thrift::TEnumTraits<thrift::Command>::findName(thriftReq.cmd)
      << "` received";

  // send back the id and the delimiter
  {
    auto ret = cmdSock.sendMultipleMore(requestIdMsg, delimMsg);
    if (ret.hasError()) {
      LOG(ERROR) << "Failed to send first two parts of the message. "
                 << ret.error();
      return;
    }
  }

  std::vector<std::string> keys;
  switch (thriftReq.cmd) {
  case thrift::Command::KEY_SET: {
    VLOG(3) << "Set key requested";
    tData_.addStatValue("kvstore.cmd_key_set", 1, fbzmq::COUNT);

    // to catch the issues in unittests
    DCHECK(!thriftReq.keySetParams.keyVals.empty());
    DCHECK(thriftReq.keyGetParams.keys.empty());

    if (thriftReq.keySetParams.keyVals.empty() ||
        !thriftReq.keyGetParams.keys.empty()) {
      LOG(ERROR) << "Malformed set request, ignoring";
      cmdSock.sendOne(fbzmq::Message::from(Constants::kErrorResponse).value());
      return;
    }

    // Update hash
    auto keyVals = thriftReq.keySetParams.keyVals;
    for (auto& kv : keyVals) {
      auto& value = kv.second;
      if (value.value.hasValue()) {
        value.hash =
            generateHash(value.version, value.originatorId, value.value);
      }
    }

    const auto thriftPub = mergeKeyValues(kvStore_, keyVals);
    updateTtlCountdownQueue(thriftPub);

    // publish the updated key-values to the peers
    if (!thriftPub.keyVals.empty()) {
      VLOG(3) << "Publishing KV update to the peers after SET command";
      auto const msg =
          fbzmq::Message::fromThriftObj(thriftPub, serializer_).value();
      localPubSock_.sendOne(msg);
      globalPubSock_.sendOne(msg);
    } else {
      VLOG(3) << "No new values to publish to clients/peers...";
    }

    // respond to the client
    cmdSock.sendOne(fbzmq::Message::from(Constants::kSuccessResponse).value());
    break;
  }
  case thrift::Command::KEY_GET: {
    VLOG(3) << "Get key-values requested";
    tData_.addStatValue("kvstore.cmd_key_get", 1, fbzmq::COUNT);

    // to catch the issues in unittests
    DCHECK(!thriftReq.keyGetParams.keys.empty());
    DCHECK(thriftReq.keySetParams.keyVals.empty());

    if (thriftReq.keyGetParams.keys.empty() ||
        !thriftReq.keySetParams.keyVals.empty()) {
      LOG(ERROR) << "Malformed GET request, ignoring";
      cmdSock.sendOne(
          fbzmq::Message::fromThriftObj(thrift::Publication{}, serializer_)
              .value());
      return;
    }

    const auto thriftPub = getKeyVals(thriftReq.keyGetParams.keys);
    cmdSock.sendOne(
        fbzmq::Message::fromThriftObj(thriftPub, serializer_).value());
    break;
  }
  case thrift::Command::KEY_DUMP: {
    VLOG(3) << "Dump all keys requested";
    tData_.addStatValue("kvstore.cmd_key_dump", 1, fbzmq::COUNT);

    const auto thriftPub = dumpAllWithPrefix(thriftReq.keyDumpParams.prefix);
    const auto retPub = cmdSock.sendOne(
        fbzmq::Message::fromThriftObj(thriftPub, serializer_).value());
    if (retPub.hasError()) {
      LOG(ERROR) << "Cannot send full dump: " << retPub.error();
    }
    break;
  }
  case thrift::Command::HASH_DUMP: {
    VLOG(3) << "Dump all hashes requested";
    tData_.addStatValue("kvstore.cmd_hash_dump", 1, fbzmq::COUNT);

    const auto hashDump = dumpHashWithPrefix(thriftReq.keyDumpParams.prefix);
    const auto request = cmdSock.sendOne(
        fbzmq::Message::fromThriftObj(hashDump, serializer_).value());
    if (request.hasError()) {
      LOG(ERROR) << "Cannot send hash dump: " << request.error();
    }
    break;
  }
  case thrift::Command::PEER_ADD:
    VLOG(2) << "Peer addition requested";
    tData_.addStatValue("kvstore.cmd_peer_add", 1, fbzmq::COUNT);

    if (thriftReq.peerAddParams.peers.empty()) {
      LOG(ERROR) << "Malformed peer-add request, ignoring";
      cmdSock.sendOne(fbzmq::Message::from(Constants::kErrorResponse).value());
      return;
    }
    addPeers(thriftReq.peerAddParams.peers);
    cmdSock.sendOne(
        fbzmq::Message::fromThriftObj(dumpPeers(), serializer_).value());
    break;

  case thrift::Command::PEER_DEL:
    VLOG(2) << "Peer deletion requested";
    tData_.addStatValue("kvstore.cmd_per_del", 1, fbzmq::COUNT);

    if (thriftReq.peerDelParams.peerNames.empty()) {
      LOG(ERROR) << "Malformed peer-del request, ignoring";
      cmdSock.sendOne(fbzmq::Message::from(Constants::kErrorResponse).value());
      return;
    }
    delPeers(thriftReq.peerDelParams.peerNames);
    cmdSock.sendOne(
        fbzmq::Message::fromThriftObj(dumpPeers(), serializer_).value());
    break;

  case thrift::Command::PEER_DUMP:
    VLOG(2) << "Peer dump requested";
    tData_.addStatValue("kvstore.cmd_peer_dump", 1, fbzmq::COUNT);
    cmdSock.sendOne(
        fbzmq::Message::fromThriftObj(dumpPeers(), serializer_).value());
    break;

  default:
    LOG(ERROR) << "Unknown command received";
    cmdSock.sendOne(fbzmq::Message::from(Constants::kErrorResponse).value());
    break;
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
  auto syncPub =
      syncPubMsg.readThriftObj<thrift::Publication>(serializer_).value();

  VLOG(1) << "Sync response received from " << requestId << " with "
          << syncPub.keyVals.size() << " key value pairs";

  auto deltaPub = mergeKeyValues(kvStore_, syncPub.keyVals);
  updateTtlCountdownQueue(deltaPub);

  // publish the updated key-values to the peers
  if (!deltaPub.keyVals.empty()) {
    VLOG(3) << "Publishing KV update to the peers after sync...";
    auto const msg =
        fbzmq::Message::fromThriftObj(deltaPub, serializer_).value();
    localPubSock_.sendOne(msg);
    globalPubSock_.sendOne(msg);
  } else {
    VLOG(3) << "No new values to publish to clients/peers...";
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

  VLOG(2) << "Requesting periodic sync from one of the neighbor ...";

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
  peersToSyncWith_.emplace(
      randomNeighbor,
      ExponentialBackoff<std::chrono::milliseconds>(
          Constants::kInitialBackoff, Constants::kMaxBackoff));

  // initial full sync request if peersToSyncWith_ was empty
  if (not fullSyncTimer_->isScheduled()) {
    fullSyncTimer_->scheduleTimeout(Constants::kInitialBackoff);
  }
}

// this will poll the sockets listening to the requests
void
KvStore::attachCallbacks() {
  VLOG(2) << "KvStore: Registering events callbacks ...";

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

  addSocket(
      fbzmq::RawZmqSocketPtr{*localCmdSock_}, ZMQ_POLLIN, [this](int) noexcept {
        // we received a command from local threads
        VLOG(3) << "KvStore " << nodeId_ << " : Command received... (locally)";
        processRequest(localCmdSock_);
      });

  addSocket(
      fbzmq::RawZmqSocketPtr{*globalCmdSock_},
      ZMQ_POLLIN,
      [this](int) noexcept {
        // we received a command from external peer
        VLOG(3) << "KvStore " << nodeId_ << " : Command received... (globally)";
        processRequest(globalCmdSock_);
      });

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
KvStore::countdownTtl() {
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
    if (it != kvStore_.end() && it->second.version == top.version &&
        it->second.ttlVersion == top.ttlVersion) {
      expiredKeys.push_back(top.key);
      kvStore_.erase(it);
      VLOG(1) << "Delete expired key: " << top.key;
      logKvEvent("KEY_EXPIRE", top.key);
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
  thrift::Publication expiredKeysPub{};
  expiredKeysPub.expiredKeys = expiredKeys;
  // Usually only local subscribers need to know, but we are also sending on
  // global socket so that it can help debugging things via breeze
  auto const msg =
      fbzmq::Message::fromThriftObj(expiredKeysPub, serializer_).value();
  localPubSock_.sendOne(msg);
  globalPubSock_.sendOne(msg);
}

int
KvStore::getPrefixCount() {
  int count = 0;
  for (auto const& kv : kvStore_) {
    auto const& key = kv.first;
    if (key.find("prefix::") == 0) {
      ++count;
    }
  }
  return count;
}

void
KvStore::submitCounters() {
  VLOG(2) << "Submitting counters ... ";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();

  // Add some more flat counters
  counters["kvstore.num_keys"] = kvStore_.size();
  counters["kvstore.num_peers"] = peers_.size();
  counters["kvstore.num_prefixes"] = getPrefixCount();
  counters["kvstore.pending_full_sync"] = peersToSyncWith_.size();

  // Prepare for submitting counters
  fbzmq::CounterMap submittingCounters = prepareSubmitCounters(counters);

  CHECK(zmqMonitorClient_);
  zmqMonitorClient_->setCounters(submittingCounters);
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
      Constants::kEventLogCategory,
      {sample.toJson()}));
}

} // namespace openr
