/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KvStoreClient.h"

#include <openr/common/OpenrClient.h>
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
  //
  // sanity check
  //
  CHECK_NE(eventLoop_, static_cast<void*>(nullptr));
  CHECK(!nodeId.empty());
  CHECK(!kvStoreLocalCmdUrl.empty());
  CHECK(!kvStoreLocalPubUrl.empty());

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

  //
  // initialize timers
  //
  initTimers();
}

KvStoreClient::KvStoreClient(
    fbzmq::Context& context,
    fbzmq::ZmqEventLoop* eventLoop,
    std::string const& nodeId,
    folly::SocketAddress const& sockAddr)
    : useThriftClient_(true),
      sockAddr_(sockAddr),
      nodeId_(nodeId),
      eventLoop_(eventLoop),
      context_(context) {
  // sanity check
  CHECK_NE(eventLoop_, static_cast<void*>(nullptr));
  CHECK(!nodeId.empty());
  CHECK(!sockAddr_.empty());

  // initialize timers
  initTimers();
}

KvStoreClient::~KvStoreClient() {
  // kvStoreSubSock will ONLY be intialized when it is NOT using thriftClient
  if (useThriftClient_) {
    return;
  }

  // Removes the KvStore socket from the eventLoop
  eventLoop_->removeSocket(fbzmq::RawZmqSocketPtr{*kvStoreSubSock_});
}

void
KvStoreClient::initTimers() {
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

  // Create check persistKey timer
  if (checkPersistKeyPeriod_.hasValue()) {
    checkPersistKeyTimer_ = fbzmq::ZmqTimeout::make(
        eventLoop_, [this]() noexcept { checkPersistKeyInStore(); });

    checkPersistKeyTimer_->scheduleTimeout(checkPersistKeyPeriod_.value());
  }
}

void
KvStoreClient::initOpenrCtrlClient() {
  // Do not create new client if one exists already
  if (openrCtrlClient_) {
    return;
  }

  try {
    openrCtrlClient_ = openr::getOpenrCtrlPlainTextClient(
        evb_, folly::IPAddress(sockAddr_.getAddressStr()), sockAddr_.getPort());
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Failed to connect to Open/R. Exception: "
               << folly::exceptionStr(ex);
    openrCtrlClient_ = nullptr;
  }
}

void
KvStoreClient::prepareKvStoreCmdSock() noexcept {
  CHECK(!useThriftClient_)
      << "prepareKvStoreCmdSock() NOT supported over Thrift";

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
  CHECK(!useThriftClient_)
      << "checkPersistKeyInStore() NOT supported over Thrift";

  // Prepare request
  thrift::KvStoreRequest request;
  thrift::KeyGetParams params;

  std::chrono::milliseconds timeout{checkPersistKeyPeriod_.value()};
  // go through persisted keys map for each area
  for (const auto& persistKeyValsEntry : persistedKeyVals_) {
    const auto& area = persistKeyValsEntry.first;
    auto& persistedKeyVals = persistKeyValsEntry.second;

    if (persistedKeyVals.empty()) {
      continue;
    }

    for (auto const& key : persistedKeyVals) {
      params.keys.push_back(key.first);
    }

    request.cmd = thrift::Command::KEY_GET;
    request.keyGetParams = params;
    request.area = area;

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
      timeout = 1000ms;
      checkPersistKeyTimer_->scheduleTimeout(1000ms);
      continue;
    }

    auto& publication = *maybePublication;
    std::unordered_map<std::string, thrift::Value> keyVals;
    for (auto const& key : persistedKeyVals) {
      auto rxkey = publication.keyVals.find(key.first);
      if (rxkey == publication.keyVals.end()) {
        keyVals.emplace(key.first, persistedKeyVals.at(key.first));
      }
    }
    // Advertise to KvStore
    if (not keyVals.empty()) {
      const auto ret = setKeysHelper(std::move(keyVals), area);
      if (ret.hasError()) {
        LOG(ERROR) << "Error sending SET_KEY request to KvStore. "
                   << "Exception: " << ret.error();
      }
    }
    processPublication(publication);
  }

  timeout = std::min(timeout, checkPersistKeyPeriod_.value());
  checkPersistKeyTimer_->scheduleTimeout(timeout);
}

bool
KvStoreClient::persistKey(
    std::string const& key,
    std::string const& value,
    std::chrono::milliseconds const ttl /* = Constants::kTtlInfInterval */,
    std::string const& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  VLOG(3) << "KvStoreClient: persistKey called for key:" << key
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
KvStoreClient::buildThriftValue(
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

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::setKey(
    std::string const& key,
    std::string const& value,
    uint32_t version /* = 0 */,
    std::chrono::milliseconds ttl /* = Constants::kTtlInfInterval */,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  VLOG(3) << "KvStoreClient: setKey called for key " << key;

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

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::setKey(
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
KvStoreClient::scheduleTtlUpdates(
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
  CHECK(not ttlThriftValue.value.hasValue());

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
KvStoreClient::unsetKey(
    std::string const& key,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  VLOG(3) << "KvStoreClient: unsetKey called for key " << key << " area "
          << area;

  persistedKeyVals_[area].erase(key);
  backoffs_.erase(key);
  keyTtlBackoffs_[area].erase(key);
  keysToAdvertise_[area].erase(key);
}

void
KvStoreClient::clearKey(
    std::string const& key,
    std::string keyValue,
    std::chrono::milliseconds ttl,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  VLOG(1) << "KvStoreClient: clear key called for key " << key;

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
  if (!ret) {
    LOG(ERROR) << "Error sending SET_KEY request to KvStore: " << ret.error();
  }
}

folly::Expected<thrift::Value, fbzmq::Error>
KvStoreClient::getKey(
    std::string const& key,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  VLOG(3) << "KvStoreClient: getKey called for key " << key << ", area "
          << area;

  // use thrift-port talking to kvstore
  if (useThriftClient_) {
    // init openrCtrlClient to talk to KvStore
    initOpenrCtrlClient();

    if (!openrCtrlClient_) {
      return folly::makeUnexpected(
          fbzmq::Error(0, "can't init OpenrCtrlClient"));
    }

    thrift::Publication pub;
    try {
      openrCtrlClient_->sync_getKvStoreKeyVals(
          pub, std::vector<std::string>{key});
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to dump key-val from KvStore. Exception: "
                 << folly::exceptionStr(ex);
      openrCtrlClient_ = nullptr;
      return folly::makeUnexpected(fbzmq::Error(0, ex.what()));
    }

    // check key existence
    if (pub.keyVals.find(key) == pub.keyVals.end()) {
      return folly::makeUnexpected(fbzmq::Error(0, "can't find key"));
    }
    return pub.keyVals.at(key);
  }

  // Prepare request
  thrift::KvStoreRequest request;
  thrift::KeyGetParams params;
  params.keys.push_back(key);

  request.cmd = thrift::Command::KEY_GET;
  request.keyGetParams = params;
  request.area = area;

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
  }
  return it->second;
}

folly::Expected<thrift::Publication, fbzmq::Error>
KvStoreClient::dumpImpl(
    fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>& sock,
    apache::thrift::CompactSerializer& serializer,
    std::string const& prefix,
    folly::Optional<std::chrono::milliseconds> recvTimeout,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  // Prepare request
  thrift::KvStoreRequest request;
  thrift::KeyDumpParams params;

  params.prefix = prefix;
  request.cmd = thrift::Command::KEY_DUMP;
  request.keyDumpParams = params;
  request.area = area;

  // Send request
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
    folly::Optional<std::chrono::milliseconds> recvTimeout /* folly::none */,
    folly::Optional<int> maybeIpTos /* folly::none */,
    const std::string& area /* thrift::KvStore_constants::kDefaultArea() */) {
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

  auto startTime = std::chrono::steady_clock::now();

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

      auto maybe = dumpImpl(sock, serializer, prefix, recvTimeout, area);

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

  const auto elapsedTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - startTime)
          .count();

  LOG(INFO) << "Took: " << elapsedTime << "ms to retrieve KvStore snapshot";

  return std::make_pair(merged, unreachedUrls);
}

/*
 * static method to dump KvStore key-val over multiple instances
 */
std::pair<
    folly::Optional<std::unordered_map<std::string /* key */, thrift::Value>>,
    std::vector<fbzmq::SocketUrl> /* unreached url */>
KvStoreClient::dumpAllWithThriftClientFromMultiple(
    const std::vector<folly::SocketAddress>& sockAddrs,
    const std::string& keyPrefix,
    std::chrono::milliseconds connectTimeout,
    std::chrono::milliseconds processTimeout,
    const folly::SocketAddress& bindAddr) {
  folly::EventBase evb;
  std::vector<folly::SemiFuture<thrift::Publication>> calls;
  std::unordered_map<std::string, thrift::Value> merged;
  std::vector<fbzmq::SocketUrl> unreachedUrls;

  thrift::KeyDumpParams params;
  params.prefix = keyPrefix;

  LOG(INFO) << "Prepare requests to all Open/R instances";

  auto startTime = std::chrono::steady_clock::now();
  for (auto const& sockAddr : sockAddrs) {
    std::unique_ptr<thrift::OpenrCtrlCppAsyncClient> client{nullptr};
    try {
      client = getOpenrCtrlPlainTextClient(
          evb,
          folly::IPAddress(sockAddr.getAddressStr()),
          sockAddr.getPort(),
          connectTimeout,
          processTimeout,
          bindAddr);
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to connect to Open/R instance at address of: "
                 << sockAddr.getAddressStr()
                 << ". Exception: " << folly::exceptionStr(ex);
    }
    if (!client) {
      unreachedUrls.push_back(fbzmq::SocketUrl{sockAddr.getAddressStr()});
      continue;
    }

    VLOG(3) << "Successfully connected to Open/R with addr: "
            << sockAddr.getAddressStr();

    calls.emplace_back(client->semifuture_getKvStoreKeyValsFiltered(params));
  }

  // can't connect to ANY single Open/R instance
  if (calls.empty()) {
    return std::make_pair(folly::none, unreachedUrls);
  }

  folly::collectAllSemiFuture(calls).via(&evb).thenValue(
      [&](std::vector<folly::Try<openr::thrift::Publication>>&& results) {
        LOG(INFO) << "Merge values received from Open/R instances"
                  << ", results size: " << results.size();

        // loop semifuture collection to merge all values
        for (auto& result : results) {
          VLOG(3) << "hasException: " << result.hasException()
                  << ", hasValue: " << result.hasValue();

          // folly::Try will contain either value or exception
          // Do NOT CHECK(result.hasValue()) since exception can happen.
          if (result.hasException()) {
            LOG(WARNING) << "Exception happened: "
                         << folly::exceptionStr(result.exception());
          } else if (result.hasValue()) {
            VLOG(3) << "KvStore publication received";
            KvStore::mergeKeyValues(merged, result.value().keyVals);
          }
        }
        evb.terminateLoopSoon();
      });

  // magic happens here
  evb.loopForever();

  // record time used to fetch from all Open/R instances
  const auto elapsedTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - startTime)
          .count();

  LOG(INFO) << "Took: " << elapsedTime << "ms to retrieve KvStore snapshot";

  return std::make_pair(merged, unreachedUrls);
}

folly::Expected<std::unordered_map<std::string, thrift::Value>, fbzmq::Error>
KvStoreClient::dumpAllWithPrefix(
    const std::string& prefix /* = "" */,
    const std::string& area /* thrift::KvStore_constants::kDefaultArea() */) {
  CHECK(!useThriftClient_) << "dumpAllWithPrefix() NOT supported over Thrift";

  prepareKvStoreCmdSock();
  auto maybePub =
      dumpImpl(*kvStoreCmdSock_, serializer_, prefix, recvTimeout_, area);
  if (maybePub.hasError()) {
    kvStoreCmdSock_.reset();
    return folly::makeUnexpected(maybePub.error());
  }
  return maybePub.value().keyVals;
}

folly::Optional<thrift::Value>
KvStoreClient::subscribeKey(
    std::string const& key,
    KeyCallback callback,
    bool fetchKeyValue,
    std::string const& area /* thrift::KvStore_constants::kDefaultArea() */) {
  VLOG(3) << "KvStoreClient: subscribeKey called for key " << key;
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
KvStoreClient::subscribeKeyFilter(
    KvStoreFilters kvFilters, KeyCallback callback) {
  keyPrefixFilter_ = std::move(kvFilters);
  keyPrefixFilterCallback_ = std::move(callback);
  return;
}

void
KvStoreClient::unSubscribeKeyFilter() {
  keyPrefixFilterCallback_ = nullptr;
  keyPrefixFilter_ = KvStoreFilters({}, {});
  return;
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
    std::unordered_map<std::string, thrift::PeerSpec> peers,
    std::string const& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  VLOG(3) << "KvStoreClient: addPeers called for " << peers.size() << " peers.";

  CHECK(!useThriftClient_) << "addPeers() NOT supported over Thrift";

  // Prepare request
  thrift::KvStoreRequest request;
  thrift::PeerAddParams params;

  params.peers = std::move(peers);
  request.cmd = thrift::Command::PEER_ADD;
  request.peerAddParams = params;
  request.area = area;

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

  if (reply.peers.size() < params.peers.size()) {
    return folly::makeUnexpected(fbzmq::Error(
        0,
        "Failed to submit new peer requests to KvStore, server "
        "error..."));
  }
  return folly::Unit();
}

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::delPeer(
    std::string const& peerName,
    std::string const& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  VLOG(3) << "KvStoreClient: delPeer called for peer " << peerName;

  std::vector<std::string> peerNames;
  peerNames.push_back(peerName);
  return delPeersHelper(peerNames, area);
}

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::delPeers(
    const std::vector<std::string>& peerNames,
    const std::string& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  VLOG(3) << "KvStoreClient: delPeers called for " << peerNames.size()
          << "peers";

  return delPeersHelper(peerNames, area);
}

folly::Expected<std::unordered_map<std::string, thrift::PeerSpec>, fbzmq::Error>
KvStoreClient::getPeers(
    const std::string& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  VLOG(3) << "KvStoreClient: getPeers called";

  CHECK(!useThriftClient_) << "getPeers() NOT supported over Thrift";

  // Prepare request
  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::PEER_DUMP;
  request.area = area;

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
  std::string area{thrift::KvStore_constants::kDefaultArea()};

  if (publication.area.hasValue()) {
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
KvStoreClient::advertisePendingKeys() {
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
    if (ret) {
      for (auto const& key : keys) {
        keysToAdvertise.erase(key);
      }
    } else {
      LOG(ERROR) << "Error sending SET_KEY request to KvStore. "
                 << "Will retry again. Error: " << ret.error();
    }
  }

  // Schedule next-timeout for processing/clearing backoffs
  VLOG(2) << "Scheduling timer after " << timeout.count() << "ms.";
  advertiseKeyValsTimer_->scheduleTimeout(timeout);
}

void
KvStoreClient::advertiseTtlUpdates() {
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
      if (not ret) {
        LOG(ERROR) << "Error sending SET_KEY request to KvStore. "
                   << "Will retry again. Exception: " << ret.error().errString;
      }
    }
  }

  // Schedule next-timeout for processing/clearing backoffs
  VLOG(2) << "Scheduling ttl timer after " << timeout.count() << "ms.";
  ttlTimer_->scheduleTimeout(timeout);
}

folly::Expected<folly::Unit, fbzmq::Error>
KvStoreClient::setKeysHelper(
    std::unordered_map<std::string, thrift::Value> keyVals,
    std::string const& area) {
  // Return if nothing to advertise.
  if (keyVals.empty()) {
    return folly::Unit();
  }

  for (auto const& kv : keyVals) {
    VLOG(3) << "Advertising key: " << kv.first
            << ", version: " << kv.second.version
            << ", originatorId: " << kv.second.originatorId
            << ", ttlVersion: " << kv.second.ttlVersion
            << ", val: " << (kv.second.value.hasValue() ? "valid" : "null")
            << ", area: " << area;
    if (not kv.second.value.hasValue()) {
      // avoid empty optinal exception
      continue;
    }
  }

  // use thrift-port talking to kvstore
  if (useThriftClient_) {
    // Init openrCtrlClient to talk to kvStore
    initOpenrCtrlClient();

    if (!openrCtrlClient_) {
      return folly::makeUnexpected(
          fbzmq::Error(0, "can't init OpenrCtrlClient"));
    }

    thrift::KeySetParams keySetParams;
    keySetParams.keyVals = std::move(keyVals);
    try {
      openrCtrlClient_->sync_setKvStoreKeyVals(keySetParams, area);
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to set key-val from KvStore. Exception: "
                 << folly::exceptionStr(ex);
      openrCtrlClient_ = nullptr;
      return folly::makeUnexpected(fbzmq::Error(0, ex.what()));
    }
    return folly::Unit();
  }

  // Build request
  thrift::KvStoreRequest request;
  thrift::KeySetParams params;

  params.keyVals = std::move(keyVals);
  request.cmd = thrift::Command::KEY_SET;
  request.keySetParams = params;
  request.area = area;

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
KvStoreClient::delPeersHelper(
    const std::vector<std::string>& peerNames,
    const std::string& area /* = thrift::KvStore_constants::kDefaultArea()*/) {
  CHECK(!useThriftClient_) << "delPeersHelper() NOT supported over Thrift";

  // Prepare request
  thrift::KvStoreRequest request;
  thrift::PeerDelParams params;

  params.peerNames = peerNames;
  request.cmd = thrift::Command::PEER_DEL;
  request.peerDelParams = params;
  request.area = area;

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
