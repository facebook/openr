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
  kvStore_ = std::make_shared<KvStore>(
      zmqContext,
      nodeId,
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
  // Prepare request
  thrift::KvStoreRequest request;
  thrift::KeySetParams params;

  params.keyVals.emplace(std::move(key), std::move(value));
  params.solicitResponse = true;
  params.nodeIds = std::move(nodeIds);

  request.cmd = thrift::Command::KEY_SET;
  request.keySetParams = params;
  request.area = area;

  // Make ZMQ call and wait for response
  auto sendStatus = reqSock_.sendThriftObj(request, serializer_);
  if (sendStatus.hasError()) {
    LOG(ERROR) << "setKey send request failed: " << sendStatus.error();
    return false;
  }

  auto maybeMsg = reqSock_.recvOne();
  if (maybeMsg.hasError()) {
    LOG(ERROR) << "setKey recv response failed: " << maybeMsg.error();
    return false;
  }

  // Return the result
  const auto msg = maybeMsg->read<std::string>();
  return *msg == Constants::kSuccessResponse.toString();
}

bool
KvStoreWrapper::setKeys(
    const std::vector<std::pair<std::string, thrift::Value>>& keyVals,
    folly::Optional<std::vector<std::string>> nodeIds) {
  // Prepare request
  thrift::KvStoreRequest request;
  thrift::KeySetParams params;

  for (const auto& keyVal : keyVals) {
    params.keyVals.emplace(keyVal.first, keyVal.second);
  }
  params.solicitResponse = true;
  params.nodeIds = std::move(nodeIds);

  request.cmd = thrift::Command::KEY_SET;
  request.keySetParams = params;

  // Make ZMQ call and wait for response
  auto sendStatus = reqSock_.sendThriftObj(request, serializer_);
  if (sendStatus.hasError()) {
    LOG(ERROR) << "setKey send request failed: " << sendStatus.error();
    return false;
  }

  return true;
}

folly::Optional<thrift::Value>
KvStoreWrapper::getKey(std::string key, std::string area) {
  // Prepare request
  thrift::KvStoreRequest request;
  thrift::KeyGetParams params;

  params.keys.push_back(key);
  request.cmd = thrift::Command::KEY_GET;
  request.keyGetParams = params;
  request.area = area;

  // Make ZMQ call and wait for response
  reqSock_.sendThriftObj(request, serializer_);
  auto maybeMsg = reqSock_.recvThriftObj<thrift::Publication>(serializer_);
  if (maybeMsg.hasError()) {
    LOG(ERROR) << "getKey recv response failed: " << maybeMsg.error();
    return folly::none;
  }
  auto publication = maybeMsg.value();

  // Return the result
  auto it = publication.keyVals.find(key);
  if (it == publication.keyVals.end()) {
    return folly::none; // No value found
  } else {
    return it->second;
  }
}

std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper::dumpAll(
    std::optional<KvStoreFilters> filters, std::string area) {
  // Prepare request
  thrift::KvStoreRequest request;
  thrift::KeyDumpParams params;

  if (filters.has_value()) {
    std::string keyPrefix = folly::join(",", filters.value().getKeyPrefixes());
    params.prefix = keyPrefix;
    params.originatorIds = filters.value().getOrigniatorIdList();
  }

  request.cmd = thrift::Command::KEY_DUMP;
  request.keyDumpParams = params;
  request.area = area;

  // Make ZMQ call and wait for response
  reqSock_.sendThriftObj(request, serializer_);
  auto maybeMsg = reqSock_.recvThriftObj<thrift::Publication>(serializer_);
  if (maybeMsg.hasError()) {
    throw std::runtime_error(folly::sformat(
        "dumAll recv response failed: {}", maybeMsg.error().errString));
  }
  auto publication = maybeMsg.value();

  // Return the result
  return publication.keyVals;
}

std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper::dumpHashes(std::string const& prefix, std::string area) {
  // Prepare request
  thrift::KvStoreRequest request;
  thrift::KeyDumpParams params;

  params.prefix = prefix;
  request.cmd = thrift::Command::HASH_DUMP;
  request.keyDumpParams = params;
  request.area = area;

  // Make ZMQ call and wait for response
  reqSock_.sendThriftObj(request, serializer_);
  auto maybeMsg = reqSock_.recvThriftObj<thrift::Publication>(serializer_);
  if (maybeMsg.hasError()) {
    throw std::runtime_error(folly::sformat(
        "dumAll recv response failed: {}", maybeMsg.error().errString));
  }
  auto publication = maybeMsg.value();

  // Return the result
  return publication.keyVals;
}

std::unordered_map<std::string /* key */, thrift::Value>
KvStoreWrapper::syncKeyVals(
    thrift::KeyVals const& keyValHashes, std::string area) {
  // Prepare request
  thrift::KvStoreRequest request;
  thrift::KeyDumpParams params;

  params.keyValHashes = keyValHashes;
  request.cmd = thrift::Command::KEY_DUMP;
  request.keyDumpParams = params;
  request.area = area;

  // Make ZMQ call and wait for response
  reqSock_.sendThriftObj(request, serializer_);
  auto maybeMsg = reqSock_.recvThriftObj<thrift::Publication>(serializer_);
  if (maybeMsg.hasError()) {
    throw std::runtime_error(folly::sformat(
        "dumAll recv response failed: {}", maybeMsg.error().errString));
  }
  auto publication = maybeMsg.value();

  // Return the result
  return publication.keyVals;
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
  // Prepare request
  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::COUNTERS_GET;

  // Make ZMQ call and wait for response
  reqSock_.sendThriftObj(request, serializer_);
  auto maybeMsg =
      reqSock_.recvThriftObj<fbzmq::thrift::CounterValuesResponse>(serializer_);
  if (maybeMsg.hasError()) {
    LOG(FATAL) << "getCounters recv response failed: " << maybeMsg.error();
  }

  return std::move(maybeMsg->counters);
}

thrift::SptInfos
KvStoreWrapper::getFloodTopo(std::string area) {
  // Prepare request
  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::FLOOD_TOPO_GET;
  request.area = area;

  // Make ZMQ call and wait for response
  reqSock_.sendThriftObj(request, serializer_);
  auto maybeMsg = reqSock_.recvThriftObj<thrift::SptInfos>(serializer_);
  if (maybeMsg.hasError()) {
    LOG(FATAL) << "getFloodTopo recv response failed: " << maybeMsg.error();
  }

  return std::move(maybeMsg.value());
}

bool
KvStoreWrapper::addPeer(
    std::string peerName, thrift::PeerSpec spec, std::string area) {
  // Prepare request
  thrift::KvStoreRequest request;
  thrift::PeerAddParams params;

  params.peers.emplace(peerName, spec);
  request.cmd = thrift::Command::PEER_ADD;
  request.peerAddParams = params;
  request.area = area;

  // Make ZMQ call and wait for response
  reqSock_.sendThriftObj(request, serializer_);
  auto maybeMsg = reqSock_.recvThriftObj<thrift::PeerCmdReply>(serializer_);
  if (maybeMsg.hasError()) {
    LOG(ERROR) << "addPeer recv response failed: " << maybeMsg.error();
    return false;
  }
  auto peerCmdReply = maybeMsg.value();

  // Return the result
  auto it = peerCmdReply.peers.find(peerName);
  if (it == peerCmdReply.peers.end()) {
    return false;
  } else {
    return it->second == spec;
  }
}

bool
KvStoreWrapper::delPeer(std::string peerName, std::string area) {
  // Prepare request
  thrift::KvStoreRequest request;
  thrift::PeerDelParams params;

  params.peerNames.push_back(peerName);
  request.cmd = thrift::Command::PEER_DEL;
  request.peerDelParams = params;
  request.area = area;

  // Make ZMQ call and wait for response
  reqSock_.sendThriftObj(request, serializer_);
  auto maybeMsg = reqSock_.recvThriftObj<thrift::PeerCmdReply>(serializer_);
  if (maybeMsg.hasError()) {
    LOG(ERROR) << "delPeer recv response failed: " << maybeMsg.error();
    return false;
  }
  auto peerCmdReply = maybeMsg.value();

  // Return the result
  return peerCmdReply.peers.find(peerName) == peerCmdReply.peers.end();
}

std::unordered_map<std::string /* peerName */, thrift::PeerSpec>
KvStoreWrapper::getPeers(std::string area) {
  // Prepare request
  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::PEER_DUMP;
  request.area = area;

  // Make ZMQ call and wait for response
  reqSock_.sendThriftObj(request, serializer_);
  auto maybeMsg = reqSock_.recvThriftObj<thrift::PeerCmdReply>(serializer_);
  if (maybeMsg.hasError()) {
    throw std::runtime_error(folly::sformat(
        "getPeers recv response failed: {}", maybeMsg.error().errString));
  }
  auto peerCmdReply = maybeMsg.value();

  // Return the result
  return peerCmdReply.peers;
}

} // namespace openr
