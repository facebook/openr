/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/logging/xlog.h>

#include <openr/common/Constants.h>
#include <openr/kvstore/KvStoreWrapper.h>

namespace openr {

template <class ClientType>
KvStoreWrapper<ClientType>::KvStoreWrapper(
    const std::unordered_set<std::string>& areaIds,
    const thrift::KvStoreConfig& kvStoreConfig,
    std::optional<messaging::RQueue<PeerEvent>> peerUpdatesQueue,
    std::optional<messaging::RQueue<KeyValueRequest>> kvRequestQueue)
    : nodeId_(*kvStoreConfig.node_name()),
      areaIds_(areaIds),
      kvStoreConfig_(kvStoreConfig) {
  // create kvStore instance
  kvStore_ = std::make_unique<KvStore<ClientType>>(
      kvStoreUpdatesQueue_,
      peerUpdatesQueue.has_value() ? peerUpdatesQueue.value()
                                   : dummyPeerUpdatesQueue_.getReader(),
      kvRequestQueue.has_value() ? kvRequestQueue.value()
                                 : dummyKvRequestQueue_.getReader(),
      logSampleQueue_,
      areaIds_,
      kvStoreConfig_);

  // we need to spin up a thrift server for KvStore clients to connect to. See
  // https://openr.readthedocs.io/en/latest/Protocol_Guide/KvStore.html#incremental-updates-flooding-update
  kvStoreServiceHandler_ = std::make_shared<KvStoreServiceHandler<ClientType>>(
      nodeId_, kvStore_.get());
}

template <class ClientType>
void
KvStoreWrapper<ClientType>::run() noexcept {
  // Start kvstore
  kvStoreThread_ = std::thread([this]() {
    XLOG(DBG1) << "KvStore " << nodeId_ << " running.";
    kvStore_->run();
    XLOG(DBG1) << "KvStore " << nodeId_ << " stopped.";
  });
  kvStore_->waitUntilRunning();

  // Setup thrift server for client to connect to
  std::shared_ptr<apache::thrift::ThriftServer> server =
      std::make_shared<apache::thrift::ThriftServer>();
  if (sslContext_) {
    server->setSSLConfig(sslContext_);
  }
  server->setNumIOWorkerThreads(1);
  server->setNumAcceptThreads(1);
  server->setPort(0);
  server->setInterface(kvStoreServiceHandler_);
  thriftServerThread_.start(std::move(server));
}

template <class ClientType>
void
KvStoreWrapper<ClientType>::stop() {
  // Return immediately if not running
  if (not kvStore_->isRunning()) {
    return;
  }

  XLOG(DBG1) << fmt::format("Stopping kvStore: {}", nodeId_);

  // Close queue
  kvStoreUpdatesQueue_.close();
  dummyPeerUpdatesQueue_.close();
  dummyKvRequestQueue_.close();
  logSampleQueue_.close();

  if (kvStoreServiceHandler_) {
    stopThriftServer();
  }

  // Stop kvstore. Destructor will automatically be called when out-of-scope
  kvStore_->stop();
  kvStoreThread_.join();
  XLOG(DBG1) << fmt::format("Successfully stopped kvStore: {}", nodeId_);
}

template <class ClientType>
bool
KvStoreWrapper<ClientType>::setKey(
    AreaId const& area,
    std::string key,
    thrift::Value value,
    std::optional<std::vector<std::string>> nodeIds) {
  // Prepare KeySetParams
  thrift::KeySetParams params;
  params.keyVals()->emplace(std::move(key), std::move(value));
  params.nodeIds().from_optional(std::move(nodeIds));

  try {
    kvStore_->semifuture_setKvStoreKeyVals(area, std::move(params)).get();
  } catch (std::exception const& e) {
    XLOG(ERR) << "Exception to set key in kvstore: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

template <class ClientType>
bool
KvStoreWrapper<ClientType>::setKeys(
    AreaId const& area,
    const std::vector<std::pair<std::string, thrift::Value>>& keyVals,
    std::optional<std::vector<std::string>> nodeIds) {
  // Prepare KeySetParams
  thrift::KeySetParams params;
  params.nodeIds().from_optional(std::move(nodeIds));
  for (const auto& [key, val] : keyVals) {
    params.keyVals()->emplace(key, val);
  }

  try {
    auto result =
        kvStore_->semifuture_setKvStoreKeyValues(area, std::move(params)).get();
    if (result->noMergeReasons()->size() != 0) {
      XLOG(ERR) << fmt::format(
          "Error when merging key-value with size: {}",
          result->noMergeReasons()->size());
    }
  } catch (std::exception const& e) {
    XLOG(ERR) << "Exception to set key in kvstore: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

template <class ClientType>
bool
KvStoreWrapper<ClientType>::injectThriftFailure(
    AreaId const& area, std::string const& peerName) {
  try {
    kvStore_->semifuture_injectThriftFailure(area, peerName);
  } catch (std::exception const& e) {
    XLOG(ERR) << "Exception to thrift failure injection: "
              << folly::exceptionStr(e);
    return false;
  }

  return true;
}

template <class ClientType>
void
KvStoreWrapper<ClientType>::pushToKvStoreUpdatesQueue(
    const AreaId& area,
    const std::unordered_map<std::string /* key */, thrift::Value>& keyVals) {
  thrift::Publication pub;
  pub.area() = area;
  pub.keyVals() = keyVals;
  kvStoreUpdatesQueue_.push(std::move(pub));
}

template <class ClientType>
std::optional<thrift::Value>
KvStoreWrapper<ClientType>::getKey(AreaId const& area, std::string key) {
  // Prepare KeyGetParams
  thrift::KeyGetParams params;
  params.keys()->push_back(key);

  thrift::Publication pub;
  try {
    auto maybeGetKey =
        kvStore_->semifuture_getKvStoreKeyVals(area, std::move(params))
            .getTry(Constants::kReadTimeout);
    if (maybeGetKey.hasValue()) {
      pub = *(maybeGetKey.value());
    } else {
      XLOG(ERR) << "Failed to retrieve key from KvStore. Key: " << key;
      return std::nullopt;
    }
  } catch (const folly::FutureTimeout&) {
    XLOG(ERR) << "Timed out retrieving key: " << key;
  } catch (std::exception const& e) {
    XLOG(WARNING) << "Exception to get key from kvstore: "
                  << folly::exceptionStr(e);
    return std::nullopt; // No value found
  }

  // Return the result
  auto it = pub.keyVals()->find(key);
  if (it == pub.keyVals()->end()) {
    return std::nullopt; // No value found
  }
  return it->second;
}

template <class ClientType>
std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper<ClientType>::dumpAll(
    AreaId const& area, std::optional<KvStoreFilters> filters) {
  // Prepare KeyDumpParams
  thrift::KeyDumpParams params;
  if (filters.has_value()) {
    params.originatorIds() = filters.value().getOriginatorIdList();
    params.senderId() = nodeId_;
    params.keys() = filters.value().getKeyPrefixes();
  }

  auto pub = *kvStore_->semifuture_dumpKvStoreKeys(std::move(params), {area})
                  .get()
                  ->begin();
  return *pub.keyVals();
}

template <class ClientType>
std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper<ClientType>::dumpHashes(
    AreaId const& area, std::string const& prefix) {
  // Prepare KeyDumpParams
  thrift::KeyDumpParams params;
  params.keys() = {prefix};
  params.senderId() = nodeId_;

  auto pub =
      *(kvStore_->semifuture_dumpKvStoreHashes(area, std::move(params)).get());
  return *pub.keyVals();
}

template <class ClientType>
SelfOriginatedKeyVals
KvStoreWrapper<ClientType>::dumpAllSelfOriginated(AreaId const& area) {
  auto keyVals =
      *(kvStore_->semifuture_dumpKvStoreSelfOriginatedKeys(area).get());
  return keyVals;
}

template <class ClientType>
std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper<ClientType>::syncKeyVals(
    AreaId const& area, thrift::KeyVals const& keyValHashes) {
  // Prepare KeyDumpParams
  thrift::KeyDumpParams params;
  params.keyValHashes() = keyValHashes;
  params.senderId() = nodeId_;

  auto pub = *kvStore_->semifuture_dumpKvStoreKeys(std::move(params), {area})
                  .get()
                  ->begin();
  return *pub.keyVals();
}

template <class ClientType>
thrift::Publication
KvStoreWrapper<ClientType>::recvPublication() {
  while (true) {
    auto maybePublication = kvStoreUpdatesQueueReader_.get(); // perform read
    if (maybePublication.hasError()) {
      throw std::runtime_error(std::string("recvPublication failed"));
    }

    // TODO: add timeout to avoid infinite waiting
    if (auto* pub =
            std::get_if<thrift::Publication>(&maybePublication.value())) {
      return *pub;
    }
  }
  throw std::runtime_error(std::string("timeout receiving publication"));
}

template <class ClientType>
void
KvStoreWrapper<ClientType>::recvKvStoreSyncedSignal() {
  while (true) {
    auto maybeEvent = kvStoreUpdatesQueueReader_.get(); // perform read
    if (maybeEvent.hasError()) {
      throw std::runtime_error(std::string("recvPublication failed"));
    }

    // TODO: add timeout to avoid infinite waiting
    if (auto* event =
            std::get_if<thrift::InitializationEvent>(&maybeEvent.value())) {
      CHECK(*event == thrift::InitializationEvent::KVSTORE_SYNCED);
      return;
    }
  }
  throw std::runtime_error(std::string("timeout receiving publication"));
}

/*
 * @brief  A wrapper/helper to read kvStoreUpdates queue and check for
 *         ADJACENCY_DB_SYNCED event
 *
 * @param  void
 * @return void
 */
template <class ClientType>
void
KvStoreWrapper<ClientType>::recvSelfAdjSyncedSignal() {
  while (true) {
    auto maybeEvent = kvStoreUpdatesQueueReader_.get(); // perform read
    if (maybeEvent.hasError()) {
      throw std::runtime_error(std::string("recvPublication failed"));
    }

    if (auto* event =
            std::get_if<thrift::InitializationEvent>(&maybeEvent.value())) {
      CHECK(*event == thrift::InitializationEvent::ADJACENCY_DB_SYNCED);
      return;
    }
  }
  throw std::runtime_error(std::string("timeout receiving publication"));
}

/*
 * @brief  A wrapper/helper to check if initialSelfOriginatedKeysTimer_
 *         currently scheduled or not
 *
 * @param  void
 * @return void
 */
template <class ClientType>
bool
KvStoreWrapper<ClientType>::checkInitialSelfOriginatedKeysTimerScheduled() {
  return (kvStore_->semifuture_checkInitialSelfOriginatedKeysTimerScheduled()
              .get());
}

template <class ClientType>
bool
KvStoreWrapper<ClientType>::addPeer(
    AreaId const& area, std::string peerName, thrift::PeerSpec spec) {
  thrift::PeersMap peers{{peerName, spec}};
  return addPeers(area, peers);
}

template <class ClientType>
bool
KvStoreWrapper<ClientType>::addPeers(
    AreaId const& area, thrift::PeersMap& peers) {
  try {
    kvStore_->semifuture_addUpdateKvStorePeers(area, peers).get();
  } catch (std::exception const& e) {
    XLOG(ERR) << "Failed to add peers: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

template <class ClientType>
bool
KvStoreWrapper<ClientType>::delPeer(AreaId const& area, std::string peerName) {
  try {
    kvStore_->semifuture_deleteKvStorePeers(area, {peerName}).get();
  } catch (std::exception const& e) {
    XLOG(ERR) << "Failed to delete peers: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

template <class ClientType>
std::optional<thrift::KvStorePeerState>
KvStoreWrapper<ClientType>::getPeerState(
    AreaId const& area, std::string const& peerName) {
  return kvStore_->semifuture_getKvStorePeerState(area, peerName).get();
}

/**
 * @brief util function to fetch peer state epoch time
 *
 * @param area      name of area kvstore peer belongs
 * @param peerName  name of the kvstore peer
 *
 * @return int64_t  epoch time in ms
 */
template <class ClientType>
int64_t
KvStoreWrapper<ClientType>::getPeerStateEpochTimeMs(
    AreaId const& area, std::string const& peerName) {
  return kvStore_->semifuture_getKvStorePeerStateEpochTimeMs(area, peerName)
      .get();
}

/**
 * @brief util function to fetch peer state elapsed time
 *
 * @param area      name of area kvstore peer belongs
 * @param peerName  name of the kvstore peer
 *
 * @return int64_t  elapsed time in ms
 */
template <class ClientType>
int64_t
KvStoreWrapper<ClientType>::getPeerStateElapsedTimeMs(
    AreaId const& area, std::string const& peerName) {
  return kvStore_->semifuture_getKvStorePeerStateElapsedTimeMs(area, peerName)
      .get();
}

/**
 * @brief util function to fetch peer state flaps
 *
 * @param area      name of area kvstore peer belongs
 * @param peerName  name of the kvstore peer
 *
 * @return int32_t  number of flaps
 */
template <class ClientType>
int32_t
KvStoreWrapper<ClientType>::getPeerFlaps(
    AreaId const& area, std::string const& peerName) {
  return kvStore_->semifuture_getKvStorePeerFlaps(area, peerName).get();
}

template <class ClientType>
std::unordered_map<std::string /* peerName */, thrift::PeerSpec>
KvStoreWrapper<ClientType>::getPeers(AreaId const& area) {
  auto peers = *(kvStore_->semifuture_getKvStorePeers(area).get());
  return peers;
}

template <class ClientType>
std::vector<thrift::KvStoreAreaSummary>
KvStoreWrapper<ClientType>::getSummary(std::set<std::string> selectAreas) {
  return *(
      kvStore_->semifuture_getKvStoreAreaSummaryInternal(selectAreas).get());
}

/*
 * ATTN: DO NOT REMOVE THIS.
 * This is explicitly instantiate all the possible template instances.
 */
template class KvStoreWrapper<thrift::OpenrCtrlCppAsyncClient>;
template class KvStoreWrapper<thrift::KvStoreServiceAsyncClient>;

} // namespace openr
