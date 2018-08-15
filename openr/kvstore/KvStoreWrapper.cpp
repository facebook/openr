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
    folly::Optional<KvStoreFilters> filters)
    : nodeId(nodeId),
      localCmdUrl(folly::sformat("inproc://{}-kvstore-cmd", nodeId)),
      localPubUrl(folly::sformat("inproc://{}-kvstore-pub", nodeId)),
      globalCmdUrl(folly::sformat("inproc://{}-kvstore-global-cmd", nodeId)),
      globalPubUrl(folly::sformat("inproc://{}-kvstore-global-pub", nodeId)),
      monitorSubmitUrl(folly::sformat("inproc://{}-monitor-submit", nodeId)),
      reqSock_(zmqContext),
      subSock_(zmqContext) {


  VLOG(1) << "KvStoreWrapper: Creating KvStore.";
  kvStore_ = std::make_unique<KvStore>(
      zmqContext,
      nodeId,
      KvStoreLocalPubUrl{localPubUrl},
      KvStoreGlobalPubUrl{globalPubUrl},
      KvStoreLocalCmdUrl{localCmdUrl},
      KvStoreGlobalCmdUrl{globalCmdUrl},
      MonitorSubmitUrl{monitorSubmitUrl},
      folly::none /* ip-tos */,
      dbSyncInterval,
      monitorSubmitInterval,
      peers,
      false /* enable legacy flooding */,
      std::move(filters));
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
KvStoreWrapper::setKey(std::string key, thrift::Value value) {
  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::KEY_SET;
  request.keySetParams.keyVals.emplace(std::move(key), std::move(value));
  request.keySetParams.solicitResponse = true;

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

folly::Optional<thrift::Value>
KvStoreWrapper::getKey(std::string key) {
  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::KEY_GET;
  request.keyGetParams.keys.push_back(key);

  // Make ZMQ call and wait for response
  reqSock_.sendThriftObj(request, serializer_);
  auto maybeMsg = reqSock_.recvThriftObj<thrift::Publication>(serializer_);
  if (maybeMsg.hasError()) {
    LOG(ERROR) << "getKey recv response failed: " << maybeMsg.error();
    return nullptr;
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
KvStoreWrapper::dumpAll(folly::Optional<KvStoreFilters> filters) {
  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::KEY_DUMP_ALL;
  if (filters.hasValue()) {
    std::string keyPrefix =
      folly::join(",", filters.value().getKeyPrefixes());
    request.keyDumpParams.prefix = keyPrefix;
    request.keyDumpParams.originatorIds = filters.value().getOrigniatorIdList();
  }
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
KvStoreWrapper::dumpHashes(std::string const& prefix) {
  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::HASH_DUMP;
  request.keyDumpParams.prefix = prefix;

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
KvStoreWrapper::syncKeyVals(thrift::KeyVals const& keyValHashes) {
  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::KEY_DUMP;
  request.keyDumpParams.keyValHashes = keyValHashes;

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
  thrift::Request request;
  request.cmd = thrift::Command::COUNTERS_GET;

  // Make ZMQ call and wait for response
  reqSock_.sendThriftObj(request, serializer_);
  auto maybeMsg = reqSock_.recvThriftObj<fbzmq::thrift::CounterValuesResponse>(
      serializer_);
  if (maybeMsg.hasError()) {
    LOG(FATAL) << "getCounters recv response failed: " << maybeMsg.error();
  }

  return std::move(maybeMsg->counters);
}

bool
KvStoreWrapper::addPeer(std::string peerName, thrift::PeerSpec spec) {
  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::PEER_ADD;
  request.peerAddParams.peers.emplace(peerName, spec);

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
KvStoreWrapper::delPeer(std::string peerName) {
  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::PEER_DEL;
  request.peerDelParams.peerNames.push_back(peerName);

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
KvStoreWrapper::getPeers() {
  // Prepare request
  thrift::Request request;
  request.cmd = thrift::Command::PEER_DUMP;

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
