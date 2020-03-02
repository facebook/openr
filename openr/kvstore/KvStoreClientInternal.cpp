/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KvStoreClientInternal.h"

#include <openr/common/OpenrClient.h>
#include <openr/common/Util.h>

#include <fbzmq/zmq/Zmq.h>
#include <folly/SharedMutex.h>
#include <folly/String.h>

namespace openr {

KvStoreClientInternal::KvStoreClientInternal(
    fbzmq::Context& context,
    OpenrEventBase* eventBase,
    std::string const& nodeId,
    std::string const& kvStoreLocalCmdUrl,
    std::string const& kvStoreLocalPubUrl,
    KvStore* kvStore,
    folly::Optional<std::chrono::milliseconds> checkPersistKeyPeriod,
    folly::Optional<std::chrono::milliseconds> recvTimeout)
    : nodeId_(nodeId),
      eventBase_(eventBase),
      context_(context),
      kvStoreLocalCmdUrl_(kvStoreLocalCmdUrl),
      kvStoreLocalPubUrl_(kvStoreLocalPubUrl),
      kvStore_(kvStore),
      checkPersistKeyPeriod_(checkPersistKeyPeriod),
      recvTimeout_(recvTimeout),
      kvStoreCmdSock_(nullptr),
      kvStoreSubSock_(
          context, folly::none, folly::none, fbzmq::NonblockingFlag{false}) {
  //
  // sanity check
  //
  CHECK_NE(eventBase_, static_cast<void*>(nullptr));
  CHECK(!nodeId.empty());
  CHECK(!kvStoreLocalCmdUrl.empty());
  CHECK(!kvStoreLocalPubUrl.empty());
  CHECK(kvStore_);

  //
  // Prepare sockets
  //

  // Connect to subscriber endpoint
  const auto kvStoreSub =
      kvStoreSubSock_.connect(fbzmq::SocketUrl{kvStoreLocalPubUrl_});
  if (kvStoreSub.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << kvStoreLocalPubUrl_ << "' "
               << kvStoreSub.error();
  }

  // Subscribe to everything
  const auto kvStoreSubOpt = kvStoreSubSock_.setSockOpt(ZMQ_SUBSCRIBE, "", 0);
  if (kvStoreSubOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
               << ""
               << " " << kvStoreSubOpt.error();
  }

  // Attach socket callback
  eventBase_->addSocket(*kvStoreSubSock_, ZMQ_POLLIN, [&](int) noexcept {
    // Read publication from socket and process it
    auto maybePublication = kvStoreSubSock_.recvThriftObj<thrift::Publication>(
        serializer_, recvTimeout_);
    if (maybePublication.hasError()) {
      LOG(ERROR) << "Failed to read publication from KvStore SUB socket. "
                 << "Exception: " << maybePublication.error();
    } else {
      processPublication(maybePublication.value());
    }
  });

  // initialize timers
  initTimers();
}

KvStoreClientInternal::~KvStoreClientInternal() {
  // Removes the KvStore socket from the eventBase
  eventBase_->removeSocket(fbzmq::RawZmqSocketPtr{*kvStoreSubSock_});
}

void
KvStoreClientInternal::initTimers() {
  // Create timer to advertise pending key-vals
  advertiseKeyValsTimer_ =
      fbzmq::ZmqTimeout::make(eventBase_->getEvb(), [this]() noexcept {
        VLOG(3) << "Received timeout event.";

        // Advertise all pending keys
        advertisePendingKeys();

        // Clear all backoff if they are passed away
        for (auto& kv : backoffs_) {
          if (kv.second.canTryNow()) {
            VLOG(2) << "Clearing off the exponential backoff for key "
                    << kv.first;
            kv.second.reportSuccess();
          }
        }
      });

  // Create ttl timer
  ttlTimer_ = fbzmq::ZmqTimeout::make(
      eventBase_->getEvb(), [this]() noexcept { advertiseTtlUpdates(); });

  // Create check persistKey timer
  if (checkPersistKeyPeriod_.has_value()) {
    checkPersistKeyTimer_ = fbzmq::ZmqTimeout::make(
        eventBase_->getEvb(), [this]() noexcept { checkPersistKeyInStore(); });

    checkPersistKeyTimer_->scheduleTimeout(checkPersistKeyPeriod_.value());
  }
}

void
KvStoreClientInternal::checkPersistKeyInStore() {
  std::chrono::milliseconds timeout{checkPersistKeyPeriod_.value()};

  // go through persisted keys map for each area
  for (const auto& persistKeyValsEntry : persistedKeyVals_) {
    const auto& area = persistKeyValsEntry.first;
    auto& persistedKeyVals = persistKeyValsEntry.second;

    if (persistedKeyVals.empty()) {
      continue;
    }

    // Prepare KEY_GET params
    thrift::KeyGetParams params;
    thrift::Publication pub;
    for (auto const& key : persistedKeyVals) {
      params.keys.emplace_back(key.first);
    }

    // Get KvStore response
    try {
      pub = *(kvStore_->getKvStoreKeyVals(params, area).get());
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to get keyvals from kvstore. Exception: "
                 << ex.what();
      // retry in 1 sec
      timeout = 1000ms;
      checkPersistKeyTimer_->scheduleTimeout(1000ms);
      continue;
    }

    // Find expired keys from latest KvStore
    std::unordered_map<std::string, thrift::Value> keyVals;
    for (auto const& key : persistedKeyVals) {
      auto rxkey = pub.keyVals.find(key.first);
      if (rxkey == pub.keyVals.end()) {
        keyVals.emplace(key.first, persistedKeyVals.at(key.first));
      }
    }

    // Advertise to KvStore
    if (not keyVals.empty()) {
      const auto ret = setKeysHelper(std::move(keyVals), area);
      if (!ret.has_value()) {
        LOG(ERROR) << "Error sending SET_KEY request to KvStore.";
      }
    }
    processPublication(pub);
  }

  timeout = std::min(timeout, checkPersistKeyPeriod_.value());
  checkPersistKeyTimer_->scheduleTimeout(timeout);
}

bool
KvStoreClientInternal::persistKey(
    std::string const& key,
    std::string const& value,
    std::chrono::milliseconds const ttl /* = Constants::kTtlInfInterval */,
    std::string const& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  VLOG(3) << "KvStoreClientInternal: persistKey called for key:" << key
          << " area:" << area;

  auto& persistedKeyVals = persistedKeyVals_[area];
  const auto& keyTtlBackoffs = keyTtlBackoffs_[area];
  auto& keysToAdvertise = keysToAdvertise_[area];
  // Look it up in the existing
  auto keyIt = persistedKeyVals.find(key);

  // Default thrift value to use with invalid version=0
  thrift::Value thriftValue = createThriftValue(
      0,
      nodeId_,
      value,
      ttl.count(),
      0 /* ttl version */,
      folly::none /* hash */);
  CHECK(thriftValue.value);

  // Retrieve the existing value for the key. If key is persisted before then
  // it is the one we have cached locally else we need to fetch it from KvStore
  if (keyIt == persistedKeyVals.end()) {
    // Get latest value from KvStore
    auto maybeValue = getKey(key, area);
    if (maybeValue.hasValue()) {
      thriftValue = maybeValue.value();
      // TTL update pub is never saved in kvstore
      DCHECK(thriftValue.value);
    }
  } else {
    thriftValue = keyIt->second;
    if (thriftValue.value.value() == value and thriftValue.ttl == ttl.count()) {
      // this is a no op, return early and change no state
      return false;
    }
    auto ttlIt = keyTtlBackoffs.find(key);
    if (ttlIt != keyTtlBackoffs.end()) {
      thriftValue.ttlVersion = ttlIt->second.first.ttlVersion;
    }
  }

  // Decide if we need to re-advertise the key back to kv-store
  bool valueChange = false;
  if (!thriftValue.version) {
    thriftValue.version = 1;
    valueChange = true;
  } else if (
      thriftValue.originatorId != nodeId_ || *thriftValue.value != value) {
    thriftValue.version++;
    thriftValue.ttlVersion = 0;
    thriftValue.value = value;
    thriftValue.originatorId = nodeId_;
    valueChange = true;
  }

  // We must update ttl value to new one. When ttl changes but value doesn't
  // then we should advertise ttl immediately so that new ttl is in effect
  const bool hasTtlChanged = ttl.count() != thriftValue.ttl;
  thriftValue.ttl = ttl.count();

  // Cache it in persistedKeyVals_. Override the existing one
  persistedKeyVals[key] = thriftValue;

  // Override existing backoff as well
  backoffs_[key] = ExponentialBackoff<std::chrono::milliseconds>(
      Constants::kInitialBackoff, Constants::kMaxBackoff);

  // Invoke callback with updated value
  auto cb = keyCallbacks_.find(key);
  if (cb != keyCallbacks_.end() && valueChange) {
    (cb->second)(key, thriftValue);
  }

  // Add keys to list of pending keys
  if (valueChange) {
    keysToAdvertise.insert(key);
  }

  // Best effort to advertise pending keys
  advertisePendingKeys();

  scheduleTtlUpdates(
      key,
      thriftValue.version,
      thriftValue.ttlVersion,
      ttl.count(),
      hasTtlChanged,
      area);

  return true;
}

thrift::Value
KvStoreClientInternal::buildThriftValue(
    std::string const& key,
    std::string const& value,
    uint32_t version /* = 0 */,
    std::chrono::milliseconds ttl /* = Constants::kTtlInfInterval */,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  // Create 'thrift::Value' object which will be sent to KvStore
  thrift::Value thriftValue = createThriftValue(
      version, nodeId_, value, ttl.count(), 0 /* ttl version */, 0 /* hash */);
  CHECK(thriftValue.value);

  // Use one version number higher than currently in KvStore if not specified
  if (!version) {
    auto maybeValue = getKey(key, area);
    if (maybeValue.hasValue()) {
      thriftValue.version = maybeValue->version + 1;
    } else {
      thriftValue.version = 1;
    }
  }
  return thriftValue;
}

folly::Optional<folly::Unit>
KvStoreClientInternal::setKey(
    std::string const& key,
    std::string const& value,
    uint32_t version /* = 0 */,
    std::chrono::milliseconds ttl /* = Constants::kTtlInfInterval */,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  VLOG(3) << "KvStoreClientInternal: setKey called for key " << key;

  // Build new key-value pair
  thrift::Value thriftValue = buildThriftValue(key, value, version, ttl, area);

  std::unordered_map<std::string, thrift::Value> keyVals;
  keyVals.emplace(key, thriftValue);

  // Advertise new key-value to KvStore
  const auto ret = setKeysHelper(std::move(keyVals), area);

  scheduleTtlUpdates(
      key,
      thriftValue.version,
      thriftValue.ttlVersion,
      ttl.count(),
      false /* advertiseImmediately */,
      area);

  return ret;
}

folly::Optional<folly::Unit>
KvStoreClientInternal::setKey(
    std::string const& key,
    thrift::Value const& thriftValue,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  CHECK(thriftValue.value);

  std::unordered_map<std::string, thrift::Value> keyVals;
  keyVals.emplace(key, thriftValue);

  const auto ret = setKeysHelper(std::move(keyVals), area);

  scheduleTtlUpdates(
      key,
      thriftValue.version,
      thriftValue.ttlVersion,
      thriftValue.ttl,
      false /* advertiseImmediately */,
      area);

  return ret;
}

void
KvStoreClientInternal::scheduleTtlUpdates(
    std::string const& key,
    uint32_t version,
    uint32_t ttlVersion,
    int64_t ttl,
    bool advertiseImmediately,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  // infinite TTL does not need update

  auto& keyTtlBackoffs = keyTtlBackoffs_[area];
  if (ttl == Constants::kTtlInfinity) {
    // in case ttl is finite before
    keyTtlBackoffs.erase(key);
    return;
  }

  // do not send value to reduce update overhead
  thrift::Value ttlThriftValue = createThriftValue(
      version,
      nodeId_,
      std::string("") /* value */,
      ttl,
      ttlVersion /* ttl version */,
      0 /* hash */);
  ttlThriftValue.value = folly::none;
  CHECK(not ttlThriftValue.value.has_value());

  // renew before Ttl expires about every ttl/3, i.e., try twice
  // use ExponentialBackoff to track remaining time
  keyTtlBackoffs[key] = std::make_pair(
      ttlThriftValue,
      ExponentialBackoff<std::chrono::milliseconds>(
          std::chrono::milliseconds(ttl / 4),
          std::chrono::milliseconds(ttl / 4 + 1)));

  // Delay first ttl advertisement by (ttl / 4). We have just advertised key or
  // update and would like to avoid sending unncessary immediate ttl update
  if (not advertiseImmediately) {
    keyTtlBackoffs.at(key).second.reportError();
  }

  advertiseTtlUpdates();
}

void
KvStoreClientInternal::unsetKey(
    std::string const& key,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  VLOG(3) << "KvStoreClientInternal: unsetKey called for key " << key
          << " area " << area;

  persistedKeyVals_[area].erase(key);
  backoffs_.erase(key);
  keyTtlBackoffs_[area].erase(key);
  keysToAdvertise_[area].erase(key);
}

void
KvStoreClientInternal::clearKey(
    std::string const& key,
    std::string keyValue,
    std::chrono::milliseconds ttl,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  VLOG(1) << "KvStoreClientInternal: clear key called for key " << key;

  // erase keys
  unsetKey(key, area);

  // if key doesn't exist in KvStore no need to add it as "empty". This
  // condition should not exist.
  auto maybeValue = getKey(key, area);
  if (!maybeValue.hasValue()) {
    return;
  }
  // overwrite all values, increment version, reset value to empty
  auto& thriftValue = maybeValue.value();
  thriftValue.originatorId = nodeId_;
  thriftValue.version++;
  thriftValue.ttl = ttl.count();
  thriftValue.ttlVersion = 0;
  thriftValue.value = std::move(keyValue);

  std::unordered_map<std::string, thrift::Value> keyVals;
  keyVals.emplace(key, std::move(thriftValue));
  // Advertise to KvStore
  const auto ret = setKeysHelper(std::move(keyVals), area);
  if (!ret.has_value()) {
    LOG(ERROR) << "Error sending SET_KEY request to KvStore";
  }
}

folly::Optional<thrift::Value>
KvStoreClientInternal::getKey(
    std::string const& key,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  VLOG(3) << "KvStoreClientInternal: getKey called for key " << key << ", area "
          << area;
  CHECK(kvStore_);

  thrift::Publication pub;
  try {
    thrift::KeyGetParams params;
    params.keys.emplace_back(key);
    pub = *(kvStore_->getKvStoreKeyVals(params, area).get());
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to get keyvals from kvstore. Exception: "
               << ex.what();
    return folly::none;
  }
  VLOG(3) << "Received " << pub.keyVals.size() << " key-vals.";

  auto it = pub.keyVals.find(key);
  if (it == pub.keyVals.end()) {
    LOG(ERROR) << "Key: " << key << " NOT found in kvstore. Area: " << area;
    return folly::none;
  }
  return it->second;
}

folly::Optional<std::unordered_map<std::string, thrift::Value>>
KvStoreClientInternal::dumpAllWithPrefix(
    const std::string& prefix /* = "" */,
    const std::string& area /* thrift::KvStore_constants::kDefaultArea() */) {
  CHECK(kvStore_);

  thrift::Publication pub;
  try {
    thrift::KeyDumpParams params;
    params.prefix = prefix;
    pub = *(kvStore_->dumpKvStoreKeys(std::move(params), area).get());
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to add peers to kvstore. Exception: " << ex.what();
    return folly::none;
  }
  return pub.keyVals;
}

folly::Optional<thrift::Value>
KvStoreClientInternal::subscribeKey(
    std::string const& key,
    KeyCallback callback,
    bool fetchKeyValue,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  VLOG(3) << "KvStoreClientInternal: subscribeKey called for key " << key;
  CHECK(bool(callback)) << "Callback function for " << key << " is empty";
  keyCallbacks_[key] = std::move(callback);

  if (fetchKeyValue) {
    auto maybeValue = getKey(key, area);
    if (maybeValue.hasValue()) {
      return maybeValue.value();
    }
  }
  return folly::none;
}

void
KvStoreClientInternal::subscribeKeyFilter(
    KvStoreFilters kvFilters, KeyCallback callback) {
  keyPrefixFilter_ = std::move(kvFilters);
  keyPrefixFilterCallback_ = std::move(callback);
  return;
}

void
KvStoreClientInternal::unSubscribeKeyFilter() {
  keyPrefixFilterCallback_ = nullptr;
  keyPrefixFilter_ = KvStoreFilters({}, {});
  return;
}

void
KvStoreClientInternal::unsubscribeKey(std::string const& key) {
  VLOG(3) << "KvStoreClientInternal: unsubscribeKey called for key " << key;
  // Store callback into KeyCallback map
  if (keyCallbacks_.erase(key) == 0) {
    LOG(WARNING) << "UnsubscribeKey called for non-existing key" << key;
  }
}

void
KvStoreClientInternal::setKvCallback(KeyCallback callback) {
  kvCallback_ = std::move(callback);
}

folly::Optional<folly::Unit>
KvStoreClientInternal::addPeers(
    std::unordered_map<std::string, thrift::PeerSpec> peers,
    std::string const& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  VLOG(3) << "KvStoreClientInternal: addPeers called for " << peers.size()
          << " peers.";
  CHECK(kvStore_);

  try {
    thrift::PeerAddParams params;
    params.peers = std::move(peers);
    kvStore_->addUpdateKvStorePeers(std::move(params), area).get();
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to add peers to kvstore. Exception: " << ex.what();
    return folly::none;
  }
  return folly::Unit();
}

folly::Optional<folly::Unit>
KvStoreClientInternal::delPeer(
    std::string const& peerName,
    std::string const& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  VLOG(3) << "KvStoreClientInternal: delPeer called for peer " << peerName;

  std::vector<std::string> peerNames;
  peerNames.push_back(peerName);
  return delPeersHelper(std::move(peerNames), area);
}

folly::Optional<folly::Unit>
KvStoreClientInternal::delPeers(
    const std::vector<std::string>& peerNames,
    const std::string& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  VLOG(3) << "KvStoreClientInternal: delPeers called for " << peerNames.size()
          << "peers";

  return delPeersHelper(peerNames, area);
}

// This is a PRIVATE method
folly::Optional<folly::Unit>
KvStoreClientInternal::delPeersHelper(
    const std::vector<std::string>& peerNames,
    const std::string& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  CHECK(kvStore_);

  try {
    thrift::PeerDelParams params;
    params.peerNames = peerNames;
    kvStore_->deleteKvStorePeers(std::move(params), area).get();
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to del peers from kvstore. Exception: " << ex.what();
    return folly::none;
  }
  return folly::Unit();
}

folly::Optional<std::unordered_map<std::string, thrift::PeerSpec>>
KvStoreClientInternal::getPeers(
    const std::string& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  VLOG(3) << "KvStoreClientInternal: getPeers called";
  CHECK(kvStore_);

  thrift::PeersMap peers;
  try {
    peers = *(kvStore_->getKvStorePeers(area).get());
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to dump peers from kvstore. Exception: " << ex.what();
    return folly::none;
  }
  return peers;
}

void
KvStoreClientInternal::processExpiredKeys(
    thrift::Publication const& publication) {
  auto const& expiredKeys = publication.expiredKeys;

  for (auto const& key : expiredKeys) {
    /* callback registered by the thread */
    if (kvCallback_) {
      kvCallback_(key, folly::none);
    }
    /* key specific registered callback */
    auto cb = keyCallbacks_.find(key);
    if (cb != keyCallbacks_.end()) {
      (cb->second)(key, folly::none);
    }
  }
}

void
KvStoreClientInternal::processPublication(
    thrift::Publication const& publication) {
  // Go through received key-values and find out the ones which need update
  std::string area{thrift::KvStore_constants::kDefaultArea()};

  if (publication.area.has_value()) {
    area = publication.area.value();
  }

  auto& persistedKeyVals = persistedKeyVals_[area];
  auto& keyTtlBackoffs = keyTtlBackoffs_[area];
  auto& keysToAdvertise = keysToAdvertise_[area];

  for (auto const& kv : publication.keyVals) {
    auto const& key = kv.first;
    auto const& rcvdValue = kv.second;

    if (not rcvdValue.value) {
      // ignore TTL update
      continue;
    }

    if (kvCallback_) {
      kvCallback_(key, rcvdValue);
    }

    // Update local keyVals as per need
    auto it = persistedKeyVals.find(key);
    auto cb = keyCallbacks_.find(key);
    // set key w/ finite TTL
    auto sk = keyTtlBackoffs.find(key);

    // key set but not persisted
    if (sk != keyTtlBackoffs.end() and it == persistedKeyVals.end()) {
      auto& setValue = sk->second.first;
      if (rcvdValue.version > setValue.version or
          (rcvdValue.version == setValue.version and
           rcvdValue.originatorId > setValue.originatorId)) {
        // key lost, cancel TTL update
        keyTtlBackoffs.erase(sk);
      } else if (
          rcvdValue.version == setValue.version and
          rcvdValue.originatorId == setValue.originatorId and
          rcvdValue.ttlVersion > setValue.ttlVersion) {
        // If version, value and originatorId is same then we should look up
        // ttlVersion and update local value if rcvd ttlVersion is higher
        // NOTE: We don't need to advertise the value back
        if (sk != keyTtlBackoffs.end() and
            sk->second.first.ttlVersion < rcvdValue.ttlVersion) {
          VLOG(1) << "Bumping TTL version for (key, version, originatorId) "
                  << folly::sformat(
                         "({}, {}, {})",
                         key,
                         rcvdValue.version,
                         rcvdValue.originatorId)
                  << " to " << (rcvdValue.ttlVersion + 1) << " from "
                  << setValue.ttlVersion;
          setValue.ttlVersion = rcvdValue.ttlVersion + 1;
        }
      }
    }

    if (it == persistedKeyVals.end()) {
      // We need to alert callback if a key is not persisted and we
      // received a change notification for it.
      if (cb != keyCallbacks_.end()) {
        (cb->second)(key, rcvdValue);
      }
      // callback for a given key filter
      if (keyPrefixFilterCallback_ &&
          keyPrefixFilter_.keyMatch(key, rcvdValue)) {
        keyPrefixFilterCallback_(key, rcvdValue);
      }
      // Skip rest of the processing. We are not interested.
      continue;
    }

    // Ignore if received version is strictly old
    auto& currentValue = it->second;
    if (currentValue.version > rcvdValue.version) {
      continue;
    }

    // Update if our version is old
    bool valueChange = false;
    if (currentValue.version < rcvdValue.version) {
      // Bump-up version number
      currentValue.originatorId = nodeId_;
      currentValue.version = rcvdValue.version + 1;
      currentValue.ttlVersion = 0;
      valueChange = true;
    }

    // version is same but originator id is different. Then we need to
    // advertise with a higher version.
    if (!valueChange and rcvdValue.originatorId != nodeId_) {
      currentValue.originatorId = nodeId_;
      currentValue.version++;
      currentValue.ttlVersion = 0;
      valueChange = true;
    }

    // Need to re-advertise if value doesn't matches. This can happen when our
    // update is reflected back
    if (!valueChange and currentValue.value != rcvdValue.value) {
      currentValue.originatorId = nodeId_;
      currentValue.version++;
      currentValue.ttlVersion = 0;
      valueChange = true;
    }

    // copy ttlVersion from ttl backoff map
    if (sk != keyTtlBackoffs.end()) {
      currentValue.ttlVersion = sk->second.first.ttlVersion;
    }

    // update local ttlVersion if received higher ttlVersion.
    // advertiseTtlUpdates will bump ttlVersion before advertising, so just
    // update to latest ttlVersion works fine
    if (currentValue.ttlVersion < rcvdValue.ttlVersion) {
      currentValue.ttlVersion = rcvdValue.ttlVersion;
      if (sk != keyTtlBackoffs.end()) {
        sk->second.first.ttlVersion = rcvdValue.ttlVersion;
      }
    }

    if (valueChange && cb != keyCallbacks_.end()) {
      (cb->second)(key, currentValue);
    }

    if (valueChange) {
      keysToAdvertise.insert(key);
    }
  } // for

  advertisePendingKeys();

  if (publication.expiredKeys.size()) {
    processExpiredKeys(publication);
  }
}

void
KvStoreClientInternal::advertisePendingKeys() {
  std::chrono::milliseconds timeout = Constants::kMaxBackoff;

  // advertise pending key for each area
  for (auto& keysToAdvertiseEntry : keysToAdvertise_) {
    auto& keysToAdvertise = keysToAdvertiseEntry.second;
    auto& area = keysToAdvertiseEntry.first;
    // Return immediately if there is nothing to advertise
    if (keysToAdvertise.empty()) {
      continue;
    }
    auto& persistedKeyVals = persistedKeyVals_[keysToAdvertiseEntry.first];

    // Build set of keys to advertise
    std::unordered_map<std::string, thrift::Value> keyVals;
    std::vector<std::string> keys;
    for (auto const& key : keysToAdvertise) {
      const auto& thriftValue = persistedKeyVals.at(key);

      // Proceed only if backoff is active
      auto& backoff = backoffs_.at(key);
      auto const& eventType = backoff.canTryNow() ? "Advertising" : "Skipping";
      VLOG(1) << eventType
              << " (key, version, originatorId, ttlVersion, ttl, area) "
              << folly::sformat(
                     "({}, {}, {}, {}, {})",
                     key,
                     thriftValue.version,
                     thriftValue.originatorId,
                     thriftValue.ttlVersion,
                     thriftValue.ttl,
                     area);
      VLOG(2) << "With value: " << folly::humanify(thriftValue.value.value());

      if (not backoff.canTryNow()) {
        timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());
        continue;
      }

      // Apply backoff
      backoff.reportError();
      timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());

      // Set in keyVals which is going to be advertise to the kvStore.
      DCHECK(thriftValue.value);
      keyVals.emplace(key, thriftValue);
      keys.push_back(key);
    }

    // Advertise to KvStore
    const auto ret = setKeysHelper(std::move(keyVals), area);
    if (ret.has_value()) {
      for (auto const& key : keys) {
        keysToAdvertise.erase(key);
      }
    } else {
      LOG(ERROR) << "Error sending SET_KEY request to KvStore.";
    }
  }

  // Schedule next-timeout for processing/clearing backoffs
  VLOG(2) << "Scheduling timer after " << timeout.count() << "ms.";
  advertiseKeyValsTimer_->scheduleTimeout(timeout);
}

void
KvStoreClientInternal::advertiseTtlUpdates() {
  // Build set of keys to advertise ttl updates
  auto timeout = Constants::kMaxTtlUpdateInterval;

  // advertise TTL updates for each area
  for (auto& keyTtlBackoffsEntry : keyTtlBackoffs_) {
    auto& keyTtlBackoffs = keyTtlBackoffsEntry.second;
    auto& persistedKeyVals = persistedKeyVals_[keyTtlBackoffsEntry.first];
    auto& area = keyTtlBackoffsEntry.first;

    std::unordered_map<std::string, thrift::Value> keyVals;

    for (auto& kv : keyTtlBackoffs) {
      const auto& key = kv.first;
      auto& backoff = kv.second.second;
      if (not backoff.canTryNow()) {
        VLOG(2) << "Skipping key: " << key << ", area: " << area;
        timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());
        continue;
      }

      // Apply backoff
      backoff.reportError();
      timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());

      auto& thriftValue = kv.second.first;
      const auto it = persistedKeyVals.find(key);
      if (it != persistedKeyVals.end()) {
        // we may have got a newer vesion for persisted key
        if (thriftValue.version < it->second.version) {
          thriftValue.version = it->second.version;
          thriftValue.ttlVersion = it->second.ttlVersion;
        }
      }
      // bump ttl version
      thriftValue.ttlVersion++;
      // Set in keyVals which is going to be advertised to the kvStore.
      DCHECK(not thriftValue.value);

      VLOG(1)
          << "Advertising ttl update (key, version, originatorId, ttlVersion, area)"
          << folly::sformat(
                 " ({}, {}, {}, {}, {})",
                 key,
                 thriftValue.version,
                 thriftValue.originatorId,
                 thriftValue.ttlVersion,
                 area);
      keyVals.emplace(key, thriftValue);
    }

    // Advertise to KvStore
    if (not keyVals.empty()) {
      const auto ret = setKeysHelper(std::move(keyVals), area);
      if (!ret.has_value()) {
        LOG(ERROR) << "Error sending SET_KEY request to KvStore.";
      }
    }
  }

  // Schedule next-timeout for processing/clearing backoffs
  VLOG(2) << "Scheduling ttl timer after " << timeout.count() << "ms.";
  ttlTimer_->scheduleTimeout(timeout);
}

folly::Optional<folly::Unit>
KvStoreClientInternal::setKeysHelper(
    std::unordered_map<std::string, thrift::Value> keyVals,
    std::string const& area) {
  CHECK(kvStore_);

  // Return if nothing to advertise.
  if (keyVals.empty()) {
    return folly::Unit();
  }

  // Debugging purpose print-out
  for (auto const& kv : keyVals) {
    VLOG(3) << "Advertising key: " << kv.first
            << ", version: " << kv.second.version
            << ", originatorId: " << kv.second.originatorId
            << ", ttlVersion: " << kv.second.ttlVersion
            << ", val: " << (kv.second.value.has_value() ? "valid" : "null")
            << ", area: " << area;
    if (not kv.second.value.has_value()) {
      // avoid empty optinal exception
      continue;
    }
  }

  thrift::KeySetParams params;
  params.keyVals = std::move(keyVals);

  try {
    kvStore_->setKvStoreKeyVals(params, area).get();
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to set key-val from KvStore. Exception: "
               << ex.what();
    return folly::none;
  }
  return folly::Unit();
}

} // namespace openr
