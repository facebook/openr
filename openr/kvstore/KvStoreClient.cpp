/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KvStoreClient.h"

#include <openr/common/Util.h>

#include <fbzmq/zmq/Zmq.h>
#include <folly/SharedMutex.h>
#include <folly/String.h>

namespace openr {

KvStoreClient::KvStoreClient(
    fbzmq::Context& context,
    fbzmq::ZmqEventLoop* eventLoop,
    std::string const& nodeId,
    std::string const& kvStoreLocalCmdUrl,
    std::string const& kvStoreLocalPubUrl,
    folly::Optional<std::chrono::milliseconds> checkPersistKeyPeriod,
    folly::Optional<std::chrono::milliseconds> recvTimeout)
    : nodeId_(nodeId),
      eventLoop_(eventLoop),
      context_(context),
      kvStoreLocalCmdUrl_(kvStoreLocalCmdUrl),
      kvStoreLocalPubUrl_(kvStoreLocalPubUrl),
      checkPersistKeyPeriod_(checkPersistKeyPeriod),
      recvTimeout_(recvTimeout),
      kvStoreCmdSock_(nullptr),
      kvStoreSubSock_(
          context, folly::none, folly::none, fbzmq::NonblockingFlag{false}) {
  // sanity check
  CHECK_NE(eventLoop_, static_cast<void*>(nullptr));
  CHECK(!nodeId.empty());
  CHECK(!kvStoreLocalCmdUrl.empty());
  CHECK(!kvStoreLocalPubUrl.empty());

  // Prepare sockets

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

  // Create timer to advertise pending key-vals
  advertiseKeyValsTimer_ =
      fbzmq::ZmqTimeout::make(eventLoop_, [this]() noexcept {
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
      eventLoop_, [this]() noexcept { advertiseTtlUpdates(); });

  if (checkPersistKeyPeriod_.hasValue()) {
    checkPersistKeyTimer_ = fbzmq::ZmqTimeout::make(
      eventLoop_, [this]() noexcept { checkPersistKeyInStore(); });

    checkPersistKeyTimer_->scheduleTimeout(checkPersistKeyPeriod_.value());
  }

  // Attach socket callback
  eventLoop_->addSocket(
      fbzmq::RawZmqSocketPtr{*kvStoreSubSock_}, ZMQ_POLLIN, [&](int) noexcept {
        // Read publication from socket and process it
        auto maybePublication =
            kvStoreSubSock_.recvThriftObj<thrift::Publication>(
                serializer_, recvTimeout_);
        if (maybePublication.hasError()) {
          LOG(ERROR) << "Failed to read publication from KvStore SUB socket. "
                     << "Exception: " << maybePublication.error();
        } else {
          processPublication(maybePublication.value());
        }
      });
}

KvStoreClient::~KvStoreClient() {
  // Removes the KvStore socket from the eventLoop
  eventLoop_->removeSocket(fbzmq::RawZmqSocketPtr{*kvStoreSubSock_});
}

void
KvStoreClient::prepareKvStoreCmdSock() noexcept {
  if (kvStoreCmdSock_) {
    return;
  }
  kvStoreCmdSock_ = std::make_unique<fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>>(
      context_, folly::none, folly::none, fbzmq::NonblockingFlag{false});
  const auto kvStoreCmd =
      kvStoreCmdSock_->connect(fbzmq::SocketUrl{kvStoreLocalCmdUrl_});
  if (kvStoreCmd.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << kvStoreLocalCmdUrl_ << "' "
               << kvStoreCmd.error();
  }
}

void
KvStoreClient::checkPersistKeyInStore() {
  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::KEY_GET;

  if (persistedKeyVals_.empty()) {
    return;
  }

  for (auto const& key: persistedKeyVals_) {
    request.keyGetParams.keys.push_back(key.first);
  }
  // Send request
  prepareKvStoreCmdSock();
  kvStoreCmdSock_->sendThriftObj(request, serializer_);

  // Receive response
  auto maybePublication = kvStoreCmdSock_->recvThriftObj<thrift::Publication>(
      serializer_, recvTimeout_);

  if (not maybePublication) {
    kvStoreCmdSock_.reset();
    LOG(ERROR) << "Failed to read publication from KvStore SUB socket. "
               << "Exception: " << maybePublication.error();
    // retry in 1 sec
    checkPersistKeyTimer_->scheduleTimeout(1000ms);
    return;
  }

  auto& publication = *maybePublication;
  std::unordered_map<std::string, thrift::Value> keyVals;
  for (auto const& key: persistedKeyVals_) {
    auto rxkey  = publication.keyVals.find(key.first);
    if (rxkey == publication.keyVals.end()) {
       keyVals.emplace(key.first, persistedKeyVals_[key.first]);
    }
  }
  // Advertise to KvStore
  if (not keyVals.empty()) {
    const auto ret = setKeysHelper(std::move(keyVals));
    if (ret.hasError()) {
      LOG(ERROR) << "Error sending SET_KEY request to KvStore. "
                 << "Exception: " << ret.error();
    }
  }
  processPublication(publication);
  checkPersistKeyTimer_->scheduleTimeout(checkPersistKeyPeriod_.value());
}

void
KvStoreClient::persistKey(
    std::string const& key,
    std::string const& value,
    std::chrono::milliseconds ttl /* = Constants::kTtlInfInterval */) {
  VLOG(3) << "KvStoreClient: persistKey called for key " << key;

  // Look it up in the existing
  auto keyIt = persistedKeyVals_.find(key);

  // Default thrift value to use with invalid version=0
  thrift::Value thriftValue(
      apache::thrift::FRAGILE,
      0,
      nodeId_,
      value,
      ttl.count(),
      0 /* ttl version */,
      0 /* hash */);
  CHECK(thriftValue.value);

  // Retrieve the existing value for the key. If key is persisted before then
  // it is the one we have cached locally else we need to fetch it from KvStore
  if (keyIt == persistedKeyVals_.end()) {
    // Get latest value from KvStore
    auto maybeValue = getKey(key);
    if (maybeValue.hasValue()) {
      thriftValue = maybeValue.value();
      // TTL update pub is never saved in kvstore
      DCHECK(thriftValue.value);
    }
  } else {
    thriftValue = keyIt->second;
    auto ttlIt = keyTtlBackoffs_.find(key);
    if (ttlIt != keyTtlBackoffs_.end()) {
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

  // Cache it in persistedKeyVals_. Override the existing one
  persistedKeyVals_[key] = thriftValue;

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
    keysToAdvertise_.insert(key);
  }

  // Best effort to advertise pending keys
  advertisePendingKeys();

  scheduleTtlUpdates(
      key, thriftValue.version, thriftValue.ttlVersion, ttl.count());
}

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::setKey(
    std::string const& key,
    std::string const& value,
    uint32_t version /* = 0 */,
    std::chrono::milliseconds ttl /* = Constants::kTtlInfInterval */) {
  VLOG(3) << "KvStoreClient: setKey called for key " << key;

  // Create 'thrift::Value' object which will be sent to KvStore
  thrift::Value thriftValue(
      apache::thrift::FRAGILE,
      version,
      nodeId_,
      value,
      ttl.count(),
      0 /* ttl version */,
      0 /* hash */);
  CHECK(thriftValue.value);

  // Use one version number higher than currently in KvStore if not specified
  if (!version) {
    auto maybeValue = getKey(key);
    if (maybeValue.hasValue()) {
      thriftValue.version = maybeValue->version + 1;
    } else {
      thriftValue.version = 1;
    }
  }

  // Advertise new key-value to KvStore
  std::unordered_map<std::string, thrift::Value> keyVals;
  DCHECK(thriftValue.value);
  keyVals.emplace(key, thriftValue);
  const auto ret = setKeysHelper(std::move(keyVals));

  scheduleTtlUpdates(
      key, thriftValue.version, thriftValue.ttlVersion, ttl.count());

  return ret;
}

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::setKey(
    std::string const& key, thrift::Value const& thriftValue) {
  CHECK(thriftValue.value);
  std::unordered_map<std::string, thrift::Value> keyVals;
  keyVals.emplace(key, thriftValue);
  const auto ret = setKeysHelper(std::move(keyVals));

  scheduleTtlUpdates(
      key, thriftValue.version, thriftValue.ttlVersion, thriftValue.ttl);

  return ret;
}

void
KvStoreClient::scheduleTtlUpdates(
    std::string const& key,
    uint32_t version,
    uint32_t ttlVersion,
    int64_t ttl) {
  // infinite TTL does not need update
  if (ttl == Constants::kTtlInfinity) {
    // in case ttl is finite before
    keyTtlBackoffs_.erase(key);
    return;
  }

  // do not send value to reduce update overhead
  thrift::Value ttlThriftValue(
      apache::thrift::FRAGILE,
      version,
      nodeId_,
      "" /* value */,
      ttl,
      ttlVersion /* ttl version */,
      0 /* hash */);
  ttlThriftValue.value = folly::none;
  CHECK(not ttlThriftValue.value.hasValue());

  // renew before Ttl expires about every ttl/3, i.e., try twice
  // use ExponentialBackoff to track remaining time
  keyTtlBackoffs_[key] = std::make_pair(
      ttlThriftValue,
      ExponentialBackoff<std::chrono::milliseconds>(
          std::chrono::milliseconds(ttl / 4),
          std::chrono::milliseconds(ttl / 4 + 1)));

  advertiseTtlUpdates();
}

void
KvStoreClient::unsetKey(std::string const& key) {
  VLOG(3) << "KvStoreClient: unsetKey called for key " << key;

  persistedKeyVals_.erase(key);
  backoffs_.erase(key);
  keyTtlBackoffs_.erase(key);
  keysToAdvertise_.erase(key);
}

folly::Expected<thrift::Value, fbzmq::Error>
KvStoreClient::getKey(std::string const& key) {
  VLOG(3) << "KvStoreClient: getKey called for key " << key;

  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::KEY_GET;
  request.keyGetParams.keys.push_back(key);

  // Send request
  prepareKvStoreCmdSock();
  kvStoreCmdSock_->sendThriftObj(request, serializer_);

  // Receive response
  auto maybePublication = kvStoreCmdSock_->recvThriftObj<thrift::Publication>(
      serializer_, recvTimeout_);
  if (not maybePublication) {
    kvStoreCmdSock_.reset();
    return folly::makeUnexpected(maybePublication.error());
  }
  auto& publication = *maybePublication;
  VLOG(3) << "Received " << publication.keyVals.size() << " key-vals.";

  auto it = publication.keyVals.find(key);
  if (it == publication.keyVals.end()) {
    return folly::makeUnexpected(fbzmq::Error(0, "key not found"));
  } else {
    return it->second;
  }
}

folly::Expected<thrift::Publication, fbzmq::Error>
KvStoreClient::dumpImpl(
    fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>& sock,
    apache::thrift::CompactSerializer& serializer,
    std::string const& prefix,
    folly::Optional<std::chrono::milliseconds> recvTimeout) {
  // Send request
  thrift::Request request;
  request.cmd = thrift::Command::KEY_DUMP;
  request.keyDumpParams.prefix = prefix;
  sock.sendThriftObj(request, serializer);

  // Receive response
  return sock.recvThriftObj<thrift::Publication>(serializer, recvTimeout);
}

// static
/**
 * For simplicity of logic/code, this implementation shoots out multiple threads
 * to dump multiple stores. While not being very efficient on resource use, it
 * allows for very simple code logic, and does not require event loops: we
 * simply dump each KvStore in its thread directly, merging into shared array
 * as the results arrive.
 */
std::pair<
    folly::Optional<std::unordered_map<std::string /* key */, thrift::Value>>,
    std::vector<fbzmq::SocketUrl> /* unreached url */>
KvStoreClient::dumpAllWithPrefixMultiple(
    fbzmq::Context& context,
    const std::vector<fbzmq::SocketUrl>& kvStoreCmdUrls,
    const std::string& prefix,
    folly::Optional<std::chrono::milliseconds> recvTimeout,
    folly::Optional<int> maybeIpTos) {
  // this protects the shared map
  std::mutex m;
  // we'll aggregate responses into this map
  std::unordered_map<std::string, thrift::Value> merged;
  // query threads will be here
  std::vector<std::thread> threads;
  std::atomic<size_t> failureCount{0};

  // url to which fail to connect
  folly::SharedMutex mutex;
  std::vector<fbzmq::SocketUrl> unreachedUrls;

  for (auto const& url : kvStoreCmdUrls) {
    threads.emplace_back([&]() {
      apache::thrift::CompactSerializer serializer;
      fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> sock(context);
      auto ret = sock.connect(url);

      if (not ret.hasValue()) {
        VLOG(4) << "connect to " << std::string(url)
                << " has failed: " << ret.error();
        {
          folly::SharedMutex::WriteHolder lock(mutex);
          unreachedUrls.push_back(url);
        }
        failureCount++;
        return;
      }

      if (maybeIpTos.hasValue()) {
        const int ipTos = maybeIpTos.value();
        const auto sockTos = sock.setSockOpt(ZMQ_TOS, &ipTos, sizeof(int));
        if (sockTos.hasError()) {
          VLOG(4) << "Error setting ZMQ_TOS to " << ipTos << " "
                  << sockTos.error();
          {
            folly::SharedMutex::WriteHolder lock(mutex);
            unreachedUrls.push_back(url);
          }
          failureCount++;
          return;
        }
      }

      auto maybe = dumpImpl(sock, serializer, prefix, recvTimeout);

      if (not maybe.hasValue()) {
        VLOG(4) << "Dumping from " << std::string(url)
                << " has failed: " << maybe.error();
        {
          folly::SharedMutex::WriteHolder lock(mutex);
          unreachedUrls.push_back(url);
        }
        failureCount++;
        return;
      }

      const auto& dump = maybe.value();
      {
        std::lock_guard<std::mutex> g(m);
        KvStore::mergeKeyValues(merged, dump.keyVals);
      }
    });
  } // for

  // all threads will eventually terminate due to receive timeout, of course
  // if you specified one. Otherwise we might be stuck waiting for dead
  // KvStores. Omitting recvTimeout is thus not recommended, unless you use
  // that for unittesting.
  for (auto& t : threads) {
    t.join();
  }

  if (failureCount >= kvStoreCmdUrls.size()) {
    return std::make_pair(folly::none, unreachedUrls);
  }

  return std::make_pair(merged, unreachedUrls);
}

folly::Expected<std::unordered_map<std::string, thrift::Value>, fbzmq::Error>
KvStoreClient::dumpAllWithPrefix(const std::string& prefix /* = "" */) {
  prepareKvStoreCmdSock();
  auto maybePub = dumpImpl(*kvStoreCmdSock_, serializer_, prefix, recvTimeout_);
  if (maybePub.hasError()) {
    kvStoreCmdSock_.reset();
    return folly::makeUnexpected(maybePub.error());
  }
  return maybePub.value().keyVals;
}

folly::Optional<thrift::Value>
KvStoreClient::subscribeKey(std::string const& key, KeyCallback callback,
        bool fetchKeyValue) {
  VLOG(3) << "KvStoreClient: subscribeKey called for key " << key;
  CHECK(bool(callback)) << "Callback function for " << key << " is empty";
  keyCallbacks_[key] = std::move(callback);

  if (fetchKeyValue) {
    auto maybeValue = getKey(key);
    if (maybeValue.hasValue()) {
      return maybeValue.value();
    }
  }
  return folly::none;
}

void
KvStoreClient::unsubscribeKey(std::string const& key) {
  VLOG(3) << "KvStoreClient: unsubscribeKey called for key " << key;
  // Store callback into KeyCallback map
  if (keyCallbacks_.erase(key) == 0) {
    LOG(WARNING) << "UnsubscribeKey called for non-existing key" << key;
  }
}

void
KvStoreClient::setKvCallback(KeyCallback callback) {
  kvCallback_ = std::move(callback);
}

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::addPeers(
    std::unordered_map<std::string, thrift::PeerSpec> peers) {
  VLOG(3) << "KvStoreClient: addPeers called for " << peers.size() << " peers.";

  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::PEER_ADD;
  request.peerAddParams.peers = std::move(peers);

  // Send request
  prepareKvStoreCmdSock();
  kvStoreCmdSock_->sendThriftObj(request, serializer_);

  // Receive response
  auto maybeReply = kvStoreCmdSock_->recvThriftObj<thrift::PeerCmdReply>(
      serializer_, recvTimeout_);
  if (not maybeReply) {
    kvStoreCmdSock_.reset();
    return folly::makeUnexpected(maybeReply.error());
  }
  auto& reply = *maybeReply;

  if (reply.peers.size() < request.peerAddParams.peers.size()) {
    return folly::makeUnexpected(fbzmq::Error(
        0,
        "Failed to submit new peer requests to KvStore, server "
        "error..."));
  }
  return folly::Unit();
}

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::delPeer(std::string const& peerName) {
  VLOG(3) << "KvStoreClient: delPeer called for peer " << peerName;

  std::vector<std::string> peerNames;
  peerNames.push_back(peerName);
  return delPeersHelper(peerNames);
}

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::delPeers(const std::vector<std::string>& peerNames) {
  VLOG(3) << "KvStoreClient: delPeers called for " << peerNames.size()
          << "peers";

  return delPeersHelper(peerNames);
}

folly::Expected<std::unordered_map<std::string, thrift::PeerSpec>, fbzmq::Error>
KvStoreClient::getPeers() {
  VLOG(3) << "KvStoreClient: getPeers called";

  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::PEER_DUMP;

  // Send request
  prepareKvStoreCmdSock();
  kvStoreCmdSock_->sendThriftObj(request, serializer_);

  // Receive response
  auto maybeReply = kvStoreCmdSock_->recvThriftObj<thrift::PeerCmdReply>(
      serializer_, recvTimeout_);
  if (not maybeReply) {
    kvStoreCmdSock_.reset();
    return folly::makeUnexpected(maybeReply.error());
  }

  return maybeReply->peers;
}

void
KvStoreClient::processExpiredKeys(thrift::Publication const& publication) {

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
KvStoreClient::processPublication(thrift::Publication const& publication) {
  // Go through received key-values and find out the ones which need update
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
    auto it = persistedKeyVals_.find(key);
    auto cb = keyCallbacks_.find(key);
    // set key w/ finite TTL
    auto sk = keyTtlBackoffs_.find(key);

    // key set but not persisted
    if (sk != keyTtlBackoffs_.end() and it == persistedKeyVals_.end()) {
      auto& setValue = sk->second.first;
      if (rcvdValue.version > setValue.version or
          (rcvdValue.version == setValue.version and
           rcvdValue.originatorId > setValue.originatorId)) {
        // key lost, cancel TTL update
        keyTtlBackoffs_.erase(sk);
      } else if (
          rcvdValue.version == setValue.version and
          rcvdValue.originatorId == setValue.originatorId and
          rcvdValue.ttlVersion > setValue.ttlVersion) {
        // If version, value and originatorId is same then we should look up
        // ttlVersion and update local value if rcvd ttlVersion is higher
        // NOTE: We don't need to advertise the value back
        if (sk != keyTtlBackoffs_.end() and
            sk->second.first.ttlVersion < rcvdValue.ttlVersion) {
          VLOG(1) << "Bumping TTL version for key " << key << " to "
                  << (rcvdValue.ttlVersion + 1) << " from "
                  << setValue.ttlVersion;
          setValue.ttlVersion = rcvdValue.ttlVersion + 1;
        }
      }
    }

    if (it == persistedKeyVals_.end()) {
      // We need to alert callback if a key is not persisted and we
      // received a change notification for it.
      if (cb != keyCallbacks_.end()) {
        (cb->second)(key, rcvdValue);
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
    if (sk != keyTtlBackoffs_.end()) {
      currentValue.ttlVersion = sk->second.first.ttlVersion;
    }

    // update local ttlVersion if received higher ttlVersion.
    // advertiseTtlUpdates will bump ttlVersion before advertising, so just
    // update to latest ttlVersion works fine
    if (currentValue.ttlVersion < rcvdValue.ttlVersion) {
      currentValue.ttlVersion = rcvdValue.ttlVersion;
      if (sk != keyTtlBackoffs_.end()) {
        sk->second.first.ttlVersion = rcvdValue.ttlVersion;
      }
    }

    if (valueChange && cb != keyCallbacks_.end()) {
      (cb->second)(key, currentValue);
    }

    if (valueChange) {
      keysToAdvertise_.insert(key);
    }
  } // for

  advertisePendingKeys();

  if (publication.expiredKeys.size()) {
    processExpiredKeys(publication);
  }
}

void
KvStoreClient::advertisePendingKeys() {
  // Return immediately if there is nothing to advertise
  if (keysToAdvertise_.empty()) {
    return;
  }

  // Build set of keys to advertise
  std::chrono::milliseconds timeout = Constants::kMaxBackoff;

  std::unordered_map<std::string, thrift::Value> keyVals;
  std::vector<std::string> keys;
  for (auto const& key : keysToAdvertise_) {
    const auto& thriftValue = persistedKeyVals_.at(key);

    // Proceed only if backoff is active
    auto& backoff = backoffs_.at(key);
    if (not backoff.canTryNow()) {
      VLOG(2) << "Skipping key/val: " << key << "/"
              << folly::humanify(thriftValue.value.value());
      timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());
      continue;
    }

    // Apply backoff
    VLOG(2) << "Advertising key/val: " << key << "/"
            << folly::humanify(thriftValue.value.value())
            << " with backoff applied.";
    backoff.reportError();
    timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());

    // Set in keyVals which is going to be advertise to the kvStore.
    DCHECK(thriftValue.value);
    keyVals.emplace(key, thriftValue);
    keys.push_back(key);
  }

  // Advertise to KvStore
  const auto ret = setKeysHelper(std::move(keyVals));
  if (ret) {
    for (auto const& key : keys) {
      keysToAdvertise_.erase(key);
    }
  } else {
    LOG(ERROR) << "Error sending SET_KEY request to KvStore. "
               << "Will retry again. Error: " << ret.error();
  }

  // Schedule next-timeout for processing/clearing backoffs
  VLOG(2) << "Scheduling timer after " << timeout.count() << "ms.";
  advertiseKeyValsTimer_->scheduleTimeout(timeout);
}

void
KvStoreClient::advertiseTtlUpdates() {
  // Build set of keys to advertise ttl updates
  auto timeout = Constants::kMaxTtlUpdateInterval;

  std::unordered_map<std::string, thrift::Value> keyVals;

  for (auto& kv : keyTtlBackoffs_) {
    const auto& key = kv.first;
    auto& backoff = kv.second.second;
    if (not backoff.canTryNow()) {
      VLOG(2) << "Skipping key: " << key;
      timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());
      continue;
    }

    // Apply backoff
    backoff.reportError();
    timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());

    auto& thriftValue = kv.second.first;
    const auto it = persistedKeyVals_.find(key);
    if (it != persistedKeyVals_.end()) {
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

    VLOG(2) << "Advertising ttl update for key: " << key
            << ", version: " << thriftValue.version
            << ", ttlVersion: " << thriftValue.ttlVersion;

    keyVals.emplace(key, thriftValue);
  }

  // Advertise to KvStore
  if (not keyVals.empty()) {
    const auto ret = setKeysHelper(std::move(keyVals));
    if (not ret) {
      LOG(ERROR) << "Error sending SET_KEY request to KvStore. "
                 << "Will retry again. Exception: " << ret.error().errString;
    }
  }

  // Schedule next-timeout for processing/clearing backoffs
  VLOG(2) << "Scheduling ttl timer after " << timeout.count() << "ms.";
  ttlTimer_->scheduleTimeout(timeout);
}

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::setKeysHelper(
    std::unordered_map<std::string, thrift::Value> keyVals) {
  // Return if nothing to advertise.
  if (keyVals.empty()) {
    return folly::Unit();
  }

  for (auto const& kv : keyVals) {
    VLOG(3) << "Advertising key: " << kv.first
            << ", version: " << kv.second.version
            << ", ttlVersion: " << kv.second.ttlVersion
            << ", val: " << (kv.second.value.hasValue() ? "valid" : "null");
    if (not kv.second.value.hasValue()) {
      // avoid empty optinal exception
      continue;
    }
  }

  // Build request
  thrift::Request request;
  request.cmd = thrift::Command::KEY_SET;
  request.keySetParams.keyVals = std::move(keyVals);

  // Send request to KvStore
  prepareKvStoreCmdSock();
  kvStoreCmdSock_->sendThriftObj(request, serializer_);

  // Receive response back
  auto maybeReply = kvStoreCmdSock_->recvOne(recvTimeout_);
  if (not maybeReply) {
    kvStoreCmdSock_.reset();
    return folly::makeUnexpected(maybeReply.error());
  }

  const auto response = maybeReply->read<std::string>().value();
  if (response != Constants::kSuccessResponse.toString()) {
    return folly::makeUnexpected(
        fbzmq::Error(0, "KvStore error in SET_KEY. Received: " + response));
  }
  return folly::Unit();
}

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::delPeersHelper(const std::vector<std::string>& peerNames) {
  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::PEER_DEL;
  request.peerDelParams.peerNames = peerNames;

  // Send request
  prepareKvStoreCmdSock();
  kvStoreCmdSock_->sendThriftObj(request, serializer_);

  // Receive response
  auto maybeReply = kvStoreCmdSock_->recvThriftObj<thrift::PeerCmdReply>(
      serializer_, recvTimeout_);
  if (not maybeReply) {
    kvStoreCmdSock_.reset();
    return folly::makeUnexpected(maybeReply.error());
  }

  return folly::Unit();
}

} // namespace openr
