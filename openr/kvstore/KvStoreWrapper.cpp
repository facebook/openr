/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KvStoreWrapper.h"

#include <memory>

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>

namespace {

const int kSocketHwm = 99999;

} // anonymous namespace

namespace openr {

KvStoreWrapper::KvStoreWrapper(
    fbzmq::Context& zmqContext,
    std::string nodeId,
    std::chrono::seconds dbSyncInterval,
    std::chrono::seconds monitorSubmitInterval,
    std::unordered_map<std::string, thrift::PeerSpec> peers,
    std::optional<KvStoreFilters> filters,
    KvStoreFloodRate kvStoreRate,
    std::chrono::milliseconds ttlDecr,
    bool enableFloodOptimization,
    bool isFloodRoot,
    const std::unordered_set<std::string>& areas)
    : nodeId(nodeId),
      localPubUrl(folly::sformat("inproc://{}-kvstore-pub", nodeId)),
      globalCmdUrl(folly::sformat("inproc://{}-kvstore-global-cmd", nodeId)),
      globalPubUrl(folly::sformat("inproc://{}-kvstore-global-pub", nodeId)),
      monitorSubmitUrl(folly::sformat("inproc://{}-monitor-submit", nodeId)),
      reqSock_(zmqContext),
      subSock_(zmqContext),
      enableFloodOptimization_(enableFloodOptimization) {
  VLOG(1) << "KvStoreWrapper: Creating KvStore.";
  // assume useFloodOptimization when enableFloodOptimization is True
  bool useFloodOptimization = enableFloodOptimization;
  kvStore_ = std::make_unique<KvStore>(
      zmqContext,
      nodeId,
      kvStoreUpdatesQueue_,
      KvStoreLocalPubUrl{localPubUrl},
      KvStoreGlobalPubUrl{globalPubUrl},
      KvStoreGlobalCmdUrl{globalCmdUrl},
      MonitorSubmitUrl{monitorSubmitUrl},
      folly::none /* ip-tos */,
      dbSyncInterval,
      monitorSubmitInterval,
      peers,
      std::move(filters),
      Constants::kHighWaterMark,
      kvStoreRate,
      ttlDecr,
      enableFloodOptimization,
      isFloodRoot,
      useFloodOptimization,
      areas);

  localCmdUrl = kvStore_->inprocCmdUrl;
}

void
KvStoreWrapper::run() noexcept {
  // connect sub socket for listening realtime updates from KvStore
  const auto subHwmOpt =
      subSock_.setSockOpt(ZMQ_RCVHWM, &kSocketHwm, sizeof(kSocketHwm));
  if (subHwmOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_RCVHWM to " << kSocketHwm << " "
               << subHwmOpt.error();
  }
  const auto subOpt =
      subSock_.setSockOpt(ZMQ_SUBSCRIBE, "", 0); // Subscribe to everything.
  if (subOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
               << ""
               << " " << subOpt.error();
  }
  const auto subConnect = subSock_.connect(fbzmq::SocketUrl{localPubUrl});
  if (subConnect.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << localPubUrl << "' "
               << subConnect.error();
  }

  // connect request socket for communicating with KvStore
  const auto reqConnect = reqSock_.connect(fbzmq::SocketUrl{localCmdUrl});
  if (reqConnect.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << localCmdUrl << "' "
               << reqConnect.error();
  }

  // Start kvstore
  kvStoreThread_ = std::thread([this]() {
    VLOG(1) << "KvStore " << nodeId << " running.";
    kvStore_->run();
    VLOG(1) << "KvStore " << nodeId << " stopped.";
  });
  kvStore_->waitUntilRunning();
}

void
KvStoreWrapper::stop() {
  // Return immediately if not running
  if (!kvStore_->isRunning()) {
    return;
  }

  // Close queue
  kvStoreUpdatesQueue_.close();

  // Destroy socket for communicating with kvstore
  reqSock_.close();
  subSock_.close();

  // Stop kvstore
  kvStore_->stop();
  kvStoreThread_.join();
}

bool
KvStoreWrapper::setKey(
    std::string key,
    thrift::Value value,
    folly::Optional<std::vector<std::string>> nodeIds,
    std::string area) {
  // Prepare KeySetParams
  thrift::KeySetParams params;
  params.keyVals.emplace(std::move(key), std::move(value));
  params.nodeIds = std::move(nodeIds);

  try {
    kvStore_->setKvStoreKeyVals(std::move(params), area).get();
  } catch (std::exception const& e) {
    LOG(ERROR) << "Exception to set key in kvstore: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

bool
KvStoreWrapper::setKeys(
    const std::vector<std::pair<std::string, thrift::Value>>& keyVals,
    folly::Optional<std::vector<std::string>> nodeIds,
    std::string area) {
  // Prepare KeySetParams
  thrift::KeySetParams params;
  params.nodeIds = std::move(nodeIds);
  for (const auto& keyVal : keyVals) {
    params.keyVals.emplace(keyVal.first, keyVal.second);
  }

  try {
    kvStore_->setKvStoreKeyVals(std::move(params), area).get();
  } catch (std::exception const& e) {
    LOG(ERROR) << "Exception to set key in kvstore: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

folly::Optional<thrift::Value>
KvStoreWrapper::getKey(std::string key, std::string area) {
  // Prepare KeyGetParams
  thrift::KeyGetParams params;
  params.keys.push_back(key);

  thrift::Publication pub;
  try {
    pub = *(kvStore_->getKvStoreKeyVals(std::move(params), area).get());
  } catch (std::exception const& e) {
    LOG(WARNING) << "Exception to get key from kvstore: "
                 << folly::exceptionStr(e);
    return folly::none; // No value found
  }

  // Return the result
  auto it = pub.keyVals.find(key);
  if (it == pub.keyVals.end()) {
    return folly::none; // No value found
  }
  return it->second;
}

std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper::dumpAll(
    std::optional<KvStoreFilters> filters, std::string area) {
  // Prepare KeyDumpParams
  thrift::KeyDumpParams params;
  if (filters.has_value()) {
    std::string keyPrefix = folly::join(",", filters.value().getKeyPrefixes());
    params.prefix = keyPrefix;
    params.originatorIds = filters.value().getOrigniatorIdList();
  }

  auto pub = *(kvStore_->dumpKvStoreKeys(std::move(params), area).get());
  return pub.keyVals;
}

std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper::dumpHashes(std::string const& prefix, std::string area) {
  // Prepare KeyDumpParams
  thrift::KeyDumpParams params;
  params.prefix = prefix;

  auto pub = *(kvStore_->dumpKvStoreHashes(std::move(params), area).get());
  return pub.keyVals;
}

std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper::syncKeyVals(
    thrift::KeyVals const& keyValHashes, std::string area) {
  // Prepare KeyDumpParams
  thrift::KeyDumpParams params;
  params.keyValHashes = keyValHashes;

  auto pub = *(kvStore_->dumpKvStoreKeys(std::move(params), area).get());
  return pub.keyVals;
}

thrift::Publication
KvStoreWrapper::recvPublication(std::chrono::milliseconds timeout) {
  auto maybeMsg =
      subSock_.recvThriftObj<thrift::Publication>(serializer_, timeout);
  if (maybeMsg.hasError()) {
    throw std::runtime_error(folly::sformat(
        "recvPublication failed: {}", maybeMsg.error().errString));
  }
  return maybeMsg.value();
}

fbzmq::thrift::CounterMap
KvStoreWrapper::getCounters() {
  return kvStore_->getCounters();
}

thrift::SptInfos
KvStoreWrapper::getFloodTopo(std::string area) {
  auto sptInfos = *(kvStore_->getSpanningTreeInfos(area).get());
  return sptInfos;
}

bool
KvStoreWrapper::addPeer(
    std::string peerName, thrift::PeerSpec spec, std::string area) {
  // Prepare peerAddParams
  thrift::PeerAddParams params;
  params.peers.emplace(peerName, spec);

  try {
    kvStore_->addUpdateKvStorePeers(params, area).get();
  } catch (std::exception const& e) {
    LOG(ERROR) << "Failed to add peers: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

bool
KvStoreWrapper::delPeer(std::string peerName, std::string area) {
  // Prepare peerDelParams
  thrift::PeerDelParams params;
  params.peerNames.emplace_back(peerName);

  try {
    kvStore_->deleteKvStorePeers(params, area).get();
  } catch (std::exception const& e) {
    LOG(ERROR) << "Failed to delete peers: " << folly::exceptionStr(e);
    return false;
  }
  return true;
}

std::unordered_map<std::string /* peerName */, thrift::PeerSpec>
KvStoreWrapper::getPeers(std::string area) {
  auto peers = *(kvStore_->getKvStorePeers(area).get());
  return peers;
}

} // namespace openr
