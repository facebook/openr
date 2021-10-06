/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>

#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/kvstore/KvStoreWrapper.h>

namespace openr {

KvStoreWrapper::KvStoreWrapper(
    fbzmq::Context& zmqContext,
    std::shared_ptr<const Config> config,
    std::optional<messaging::RQueue<PeerEvent>> peerUpdatesQueue,
    std::optional<messaging::RQueue<KeyValueRequest>> kvRequestQueue)
    : nodeId(config->getNodeName()),
      globalCmdUrl(folly::sformat("inproc://{}-kvstore-global-cmd", nodeId)),
      config_(config) {
  kvStore_ = std::make_unique<KvStore>(
      zmqContext,
      kvStoreUpdatesQueue_,
      kvStoreSyncEventsQueue_,
      peerUpdatesQueue.has_value() ? peerUpdatesQueue.value()
                                   : dummyPeerUpdatesQueue_.getReader(),
      kvRequestQueue.has_value() ? kvRequestQueue.value()
                                 : dummyKvRequestQueue_.getReader(),
      logSampleQueue_,
      KvStoreGlobalCmdUrl{globalCmdUrl},
      config_);

  // we need to spin up a thrift server for KvStore clients to connect to. See
  // https://openr.readthedocs.io/en/latest/Protocol_Guide/KvStore.html#incremental-updates-flooding-update
  // for info on data flow
  thriftServer_ = std::make_unique<OpenrThriftServerWrapper>(
      nodeId,
      nullptr, // Decision
      nullptr, // Fib
      kvStore_.get(),
      nullptr, // LinkMonitor
      nullptr, // Monitor
      nullptr, // PersistentStore
      nullptr, // PrefixManager
      nullptr, // Spark
      config);
}

void
KvStoreWrapper::run() noexcept {
  // Start kvstore
  kvStoreThread_ = std::thread([this]() {
    XLOG(DBG1) << "KvStore " << nodeId << " running.";
    kvStore_->run();
    XLOG(DBG1) << "KvStore " << nodeId << " stopped.";
  });
  kvStore_->waitUntilRunning();

  thriftServer_->run();
}

void
KvStoreWrapper::stop() {
  XLOG(DBG1) << "Stopping KvStoreWrapper";
  // Return immediately if not running
  if (!kvStore_->isRunning()) {
    return;
  }

  // Close queue
  kvStoreUpdatesQueue_.close();
  kvStoreSyncEventsQueue_.close();
  dummyPeerUpdatesQueue_.close();
  dummyKvRequestQueue_.close();
  logSampleQueue_.close();

  if (thriftServer_) {
    thriftServer_->stop();
    thriftServer_.reset();
  }

  // Stop kvstore
  kvStore_->stop();
  kvStoreThread_.join();
  XLOG(DBG1) << "KvStoreWrapper stopped.";
}

bool
KvStoreWrapper::setKey(
    AreaId const& area,
    std::string key,
    thrift::Value value,
    std::optional<std::vector<std::string>> nodeIds) {
  // Prepare KeySetParams
  thrift::KeySetParams params;
  params.keyVals_ref()->emplace(std::move(key), std::move(value));
  params.nodeIds_ref().from_optional(std::move(nodeIds));

  try {
    kvStore_->semifuture_setKvStoreKeyVals(area, std::move(params)).get();
  } catch (std::exception const& e) {
    LOG(ERROR) << "Exception to set key in kvstore: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

bool
KvStoreWrapper::setKeys(
    AreaId const& area,
    const std::vector<std::pair<std::string, thrift::Value>>& keyVals,
    std::optional<std::vector<std::string>> nodeIds) {
  // Prepare KeySetParams
  thrift::KeySetParams params;
  params.nodeIds_ref().from_optional(std::move(nodeIds));
  for (const auto& [key, val] : keyVals) {
    params.keyVals_ref()->emplace(key, val);
  }

  try {
    kvStore_->semifuture_setKvStoreKeyVals(area, std::move(params)).get();
  } catch (std::exception const& e) {
    LOG(ERROR) << "Exception to set key in kvstore: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

void
KvStoreWrapper::pushToKvStoreUpdatesQueue(
    const AreaId& area,
    const std::unordered_map<std::string /* key */, thrift::Value>& keyVals) {
  thrift::Publication pub;
  pub.area_ref() = area;
  pub.keyVals_ref() = keyVals;
  kvStoreUpdatesQueue_.push(Publication(pub));
}

std::optional<thrift::Value>
KvStoreWrapper::getKey(AreaId const& area, std::string key) {
  // Prepare KeyGetParams
  thrift::KeyGetParams params;
  params.keys_ref()->push_back(key);

  thrift::Publication pub;
  try {
    auto maybeGetKey =
        kvStore_->semifuture_getKvStoreKeyVals(area, std::move(params))
            .getTry(Constants::kReadTimeout);
    if (maybeGetKey.hasValue()) {
      pub = *(maybeGetKey.value());
    } else {
      LOG(ERROR) << "Failed to retrieve key from KvStore. Key: " << key;
      return std::nullopt;
    }
  } catch (const folly::FutureTimeout&) {
    LOG(ERROR) << "Timed out retrieving key: " << key;
  } catch (std::exception const& e) {
    LOG(WARNING) << "Exception to get key from kvstore: "
                 << folly::exceptionStr(e);
    return std::nullopt; // No value found
  }

  // Return the result
  auto it = pub.keyVals_ref()->find(key);
  if (it == pub.keyVals_ref()->end()) {
    return std::nullopt; // No value found
  }
  return it->second;
}

std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper::dumpAll(
    AreaId const& area, std::optional<KvStoreFilters> filters) {
  // Prepare KeyDumpParams
  thrift::KeyDumpParams params;
  if (filters.has_value()) {
    std::string keyPrefix = folly::join(",", filters.value().getKeyPrefixes());
    params.prefix_ref() = keyPrefix;
    params.originatorIds_ref() = filters.value().getOriginatorIdList();
    if (not keyPrefix.empty()) {
      params.keys_ref() = filters.value().getKeyPrefixes();
    }
  }

  auto pub = *kvStore_->semifuture_dumpKvStoreKeys(std::move(params), {area})
                  .get()
                  ->begin();
  return *pub.keyVals_ref();
}

std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper::dumpHashes(AreaId const& area, std::string const& prefix) {
  // Prepare KeyDumpParams
  thrift::KeyDumpParams params;
  params.prefix_ref() = prefix;
  params.keys_ref() = {prefix};

  auto pub =
      *(kvStore_->semifuture_dumpKvStoreHashes(area, std::move(params)).get());
  return *pub.keyVals_ref();
}

SelfOriginatedKeyVals
KvStoreWrapper::dumpAllSelfOriginated(AreaId const& area) {
  auto keyVals =
      *(kvStore_->semifuture_dumpKvStoreSelfOriginatedKeys(area).get());
  return keyVals;
}

std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper::syncKeyVals(
    AreaId const& area, thrift::KeyVals const& keyValHashes) {
  // Prepare KeyDumpParams
  thrift::KeyDumpParams params;
  params.keyValHashes_ref() = keyValHashes;

  auto pub = *kvStore_->semifuture_dumpKvStoreKeys(std::move(params), {area})
                  .get()
                  ->begin();
  return *pub.keyVals_ref();
}

thrift::Publication
KvStoreWrapper::recvPublication() {
  auto maybePublication = kvStoreUpdatesQueueReader_.get(); // perform read
  if (maybePublication.hasError()) {
    throw std::runtime_error(std::string("recvPublication failed"));
  }
  return maybePublication.value().tPublication;
}

void
KvStoreWrapper::recvKvStoreSyncedSignal() {
  // Read publication from queue, and make sure kvStoreSynced flag is set.
  auto maybePublication = kvStoreUpdatesQueueReader_.get(); // perform read
  if (maybePublication.hasError()) {
    throw std::runtime_error(std::string("recvPublication failed"));
  }
  CHECK(maybePublication.value().kvStoreSynced);
}

KvStoreSyncEvent
KvStoreWrapper::recvSyncEvent() {
  auto maybeEvent = kvStoreSyncEventsQueueReader_.get(); // perform read
  if (maybeEvent.hasError()) {
    throw std::runtime_error(std::string("recvPublication failed"));
  }
  auto event = maybeEvent.value();
  return event;
}

thrift::SptInfos
KvStoreWrapper::getFloodTopo(AreaId const& area) {
  auto sptInfos = *(kvStore_->semifuture_getSpanningTreeInfos(area).get());
  return sptInfos;
}

bool
KvStoreWrapper::addPeer(
    AreaId const& area, std::string peerName, thrift::PeerSpec spec) {
  thrift::PeersMap peers{{peerName, spec}};
  return addPeers(area, peers);
}

bool
KvStoreWrapper::addPeers(AreaId const& area, thrift::PeersMap& peers) {
  try {
    kvStore_->semifuture_addUpdateKvStorePeers(area, peers).get();
  } catch (std::exception const& e) {
    LOG(ERROR) << "Failed to add peers: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

bool
KvStoreWrapper::delPeer(AreaId const& area, std::string peerName) {
  try {
    kvStore_->semifuture_deleteKvStorePeers(area, {peerName}).get();
  } catch (std::exception const& e) {
    LOG(ERROR) << "Failed to delete peers: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

std::optional<thrift::KvStorePeerState>
KvStoreWrapper::getPeerState(AreaId const& area, std::string const& peerName) {
  return kvStore_->semifuture_getKvStorePeerState(area, peerName).get();
}

std::unordered_map<std::string /* peerName */, thrift::PeerSpec>
KvStoreWrapper::getPeers(AreaId const& area) {
  auto peers = *(kvStore_->semifuture_getKvStorePeers(area).get());
  return peers;
}

std::vector<thrift::KvStoreAreaSummary>
KvStoreWrapper::getSummary(std::set<std::string> selectAreas) {
  return *(
      kvStore_->semifuture_getKvStoreAreaSummaryInternal(selectAreas).get());
}

} // namespace openr
