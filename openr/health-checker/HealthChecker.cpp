/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HealthChecker.h"

#include <folly/MapUtil.h>
#include <folly/Random.h>
#include <openr/common/Util.h>

using apache::thrift::FRAGILE;

namespace {
const int kMaxPingPacketSize = 1028;
} // namespace

namespace openr {

HealthChecker::HealthChecker(
    std::string const& myNodeName,
    thrift::HealthCheckOption healthCheckOption,
    uint32_t healthCheckPct,
    uint16_t udpPingPort,
    std::chrono::seconds pingInterval,
    folly::Optional<int> maybeIpTos,
    const AdjacencyDbMarker& adjacencyDbMarker,
    const PrefixDbMarker& prefixDbMarker,
    const KvStoreLocalCmdUrl& storeCmdUrl,
    const KvStoreLocalPubUrl& storePubUrl,
    const HealthCheckerCmdUrl& healthCheckerCmdUrl,
    const MonitorSubmitUrl& monitorSubmitUrl,
    fbzmq::Context& zmqContext)
    : myNodeName_(myNodeName),
      healthCheckOption_(healthCheckOption),
      healthCheckPct_(healthCheckPct),
      udpPingPort_(udpPingPort),
      pingInterval_(pingInterval),
      adjacencyDbMarker_(adjacencyDbMarker),
      prefixDbMarker_(prefixDbMarker),
      maybeIpTos_(maybeIpTos),
      repSock_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}) {
  // Sanity check on healthCheckPct validation
  if (healthCheckPct_ > 100) {
    LOG(FATAL) << "Invalid healthCheckPct value: " << healthCheckPct_
               << ", skipping health check....";
  }

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);
  kvStoreClient_ = std::make_unique<KvStoreClient>(
      zmqContext, this, myNodeName, storeCmdUrl, storePubUrl);

  const auto repBind = repSock_.bind(fbzmq::SocketUrl{healthCheckerCmdUrl});
  if (repBind.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << std::string(healthCheckerCmdUrl)
               << "' " << repBind.error();
  }

  // Initialize sockets in event loop
  scheduleTimeout(std::chrono::seconds(0), [this]() noexcept { prepare(); });
}

void
HealthChecker::prepare() noexcept {
  // get a dump from kvStore and set callback to process all future publications

  const auto adjMap = kvStoreClient_->dumpAllWithPrefix(adjacencyDbMarker_);
  const auto prefixMap = kvStoreClient_->dumpAllWithPrefix(prefixDbMarker_);
  if (not adjMap || not prefixMap) {
    LOG(ERROR) << "Intial kv store dump failed";
  } else {
    for (const auto& kv : *adjMap) {
      processKeyVal(kv.first, kv.second);
    }
    for (const auto& kv : *prefixMap) {
      processKeyVal(kv.first, kv.second);
    }
  }

  kvStoreClient_->setKvCallback([this](
      const std::string& key,
      folly::Optional<thrift::Value> thriftVal) noexcept {
        processKeyVal(key, thriftVal);
  });

  // Listen for request on health checker cmd socket
  addSocket(
      fbzmq::RawZmqSocketPtr{*repSock_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(2) << "HealthChecker: received request on cmd socket";
        processRequest();
      });

  // Schedule periodic timer for sending pings
  pingTimer_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
    printInfo();
    pingNodes();
  });
  pingTimer_->scheduleTimeout(pingInterval_, true /* isPeriodic */);
  // Schedule periodic timer for monitor submission
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(
      Constants::kMonitorSubmitInterval, true /* isPeriodic */);
}

void
HealthChecker::createPingSocket() noexcept {
  // Socket fd exists
  if (pingSocketFd_.hasValue()) {
    return;
  }

  // Sanity checks on our loopback address
  auto myNodeIt = nodeInfo_.find(myNodeName_);
  if (myNodeIt == nodeInfo_.end() || myNodeIt->second.ipAddress.addr.empty()) {
    LOG(WARNING) << "Can't create ping socket because of no known v6 address.";
    return;
  }
  const auto myLoopbackAddr = toIPAddress(myNodeIt->second.ipAddress);
  if (not myLoopbackAddr.isV6()) {
    LOG(WARNING) << "Can't create ping socket because of no known v6 address.";
    return;
  }

  // prepare and bind udp ping socket
  VLOG(2) << "Preparing and binding UDP socket to receive health check pings";
  int socketFd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (socketFd < 0) {
    LOG(ERROR) << "Failed creating UDP socket: " << folly::errnoStr(errno);
    return;
  }

  // make v6 only
  const int v6Only = 1;
  if (setsockopt(
        socketFd, IPPROTO_IPV6, IPV6_V6ONLY,
        &v6Only, sizeof(v6Only)) != 0) {
    LOG(ERROR) << "Failed making the socket v6 only: "
               << folly::errnoStr(errno);
    close(socketFd);
    return;
  }

  // Set ip-tos
  if (maybeIpTos_) {
    const int ipTos = *maybeIpTos_;
    if (::setsockopt(
            socketFd, IPPROTO_IPV6, IPV6_TCLASS,
            &ipTos, sizeof(int)) != 0) {
      LOG(ERROR) << "Failed setting ip-tos value on socket. Error: "
                 << folly::errnoStr(errno);
      close(socketFd);
      return;
    }
  }

  // Set source address
  const auto pingSockAddr = folly::SocketAddress(myLoopbackAddr, udpPingPort_);
  sockaddr_storage addrStorage;
  pingSockAddr.getAddress(&addrStorage);
  sockaddr* saddr = reinterpret_cast<sockaddr*>(&addrStorage);
  if (::bind(socketFd, saddr, pingSockAddr.getActualSize()) != 0) {
    LOG(ERROR) << "Failed binding the socket: " << folly::errnoStr(errno)
               << " " << myLoopbackAddr.str();
    close(socketFd);
    return;
  }

  // Listen for incoming messages on ping FD
  addSocketFd(socketFd, ZMQ_POLLIN, [this](int) noexcept {
    try {
      processMessage();
    } catch (std::exception const& err) {
      LOG(ERROR) << "HealthChecker: error processing health check ping "
                 << folly::exceptionStr(err);
    }
  });

  // Assign socketFd to state variable
  LOG(INFO) << "Created new ping socket with fd " << socketFd
            << " with source address " << myLoopbackAddr.str();
  pingSocketFd_ = socketFd;
}

void
HealthChecker::closePingSocket() noexcept {
  if (not pingSocketFd_.hasValue()) {
    return;
  }

  // Remove socket fd from polling list and close-fd
  removeSocketFd(pingSocketFd_.value());
  close(pingSocketFd_.value());

  // Set state value for fd to none
  LOG(INFO) << "Closed an existing socket fd " << pingSocketFd_.value();
  pingSocketFd_ = folly::none;
}

void
HealthChecker::pingNodes() {
  if (not pingSocketFd_.hasValue()) {
    LOG(ERROR) << "Ping socket has not initialized yet. Skipping ping nodes.";
    return;
  }

  for (const auto& node : nodesToPing_) {
    try {
      auto& info = nodeInfo_.at(node);
      if (info.ipAddress.addr.empty()) {
        continue;
      }
      folly::SocketAddress socketAddr(
          toIPAddress(info.ipAddress), udpPingPort_);
      tData_.addStatValue("health_checker.ping_to_" + node, 1, fbzmq::COUNT);
      sendDatagram(
          node,
          socketAddr,
          thrift::HealthCheckerMessageType::PING,
          ++info.lastValSent);
    } catch (const std::exception& e) {
      continue;
    }
  }
}

void
HealthChecker::processKeyVal(
    std::string const& key, folly::Optional<thrift::Value> val) noexcept {
  std::string prefix, nodeName;
  folly::split(
      Constants::kPrefixNameSeparator.toString(), key, prefix, nodeName);

  if (!val.hasValue()) {
    VLOG(4) << "HealthChecker: key expired:" << key << " node:" << nodeName;

    if (nodeInfo_.find(nodeName) == nodeInfo_.end()) {
        return;
    }
    if (key.find(adjacencyDbMarker_) == 0) {
      nodeInfo_[nodeName].neighbors.clear();
    }
    if (key.find(prefixDbMarker_) == 0) {
      nodeInfo_[nodeName].ipAddress = toBinaryAddress(
            std::move(folly::IPAddress{}));
    }
    if (nodeInfo_[nodeName].neighbors.empty() &&
        nodeInfo_[nodeName].ipAddress.addr.empty()) {
      VLOG(2) << "HealthChecker: Erasing node:" << nodeName;
      nodeInfo_.erase(nodeName);
      nodesToPing_.erase(nodeName);
    }
    return;
  }

  if (key.find(adjacencyDbMarker_) == 0) {
    const auto adjacencyDb =
        fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
            val.value().value.value(), serializer_);
    CHECK_EQ(nodeName, adjacencyDb.thisNodeName);
    processAdjDb(adjacencyDb);
  }

  if (key.find(prefixDbMarker_) == 0) {
    auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
        val.value().value.value(), serializer_);
    CHECK_EQ(nodeName, prefixDb.thisNodeName);
    processPrefixDb(prefixDb);
  }
}

void
HealthChecker::processAdjDb(thrift::AdjacencyDatabase const& adjDb) {
  auto& neighbors = nodeInfo_[adjDb.thisNodeName].neighbors;
  neighbors.clear();
  for (auto const& adj : adjDb.adjacencies) {
    neighbors.push_back(adj.otherNodeName);
  }
  updateNodesToPing();
}

void
HealthChecker::processPrefixDb(thrift::PrefixDatabase const& prefixDb) {
  // Find all valid node addresses from prefixEntries
  std::vector<thrift::BinaryAddress> nodeAddrs;
  for (auto const& prefixEntry : prefixDb.prefixEntries) {
    folly::CIDRNetwork prefix;
    folly::Optional<folly::IPAddress> addr;

    // Sanity check for prefix
    try {
      prefix = toIPNetwork(prefixEntry.prefix);
    } catch (const std::exception& e) {
      continue;
    }

    // Ignore if not v6
    if (not prefix.first.isV6()) {
      continue;
    }

    // Use prefix if type LOOPBACK
    if (prefixEntry.type == thrift::PrefixType::LOOPBACK &&
        prefix.second == 128) {
      addr = prefix.first;
    }

    // Use prefix if type PREFIX_ALLOCATOR
    if (prefixEntry.type == thrift::PrefixType::PREFIX_ALLOCATOR) {
      addr = createLoopbackAddr(prefix);
    }

    // Skip if no address
    if (not addr.hasValue()) {
      continue;
    }

    // Add valid address
    nodeAddrs.emplace_back(toBinaryAddress(*addr));
  }

  // Check if address exists
  bool foundOldIpAddress{false};
  for (const auto& nodeAddr : nodeAddrs) {
    if (nodeInfo_[prefixDb.thisNodeName].ipAddress == nodeAddr) {
      foundOldIpAddress = true;
      break;
    }
  }
  if (nodeAddrs.empty() or foundOldIpAddress) {
    return;
  }

  // Update node's IPAddress
  nodeInfo_[prefixDb.thisNodeName].ipAddress = nodeAddrs.at(0);
  if (prefixDb.thisNodeName == myNodeName_) {
    closePingSocket();
    createPingSocket();
  }
}

void
HealthChecker::updateNodesToPing() {
  switch (healthCheckOption_) {
  case thrift::HealthCheckOption::PingNeighborOfNeighbor:
    for (auto const& neighbor : nodeInfo_[myNodeName_].neighbors) {
      nodesToPing_.insert(
          nodeInfo_[neighbor].neighbors.begin(),
          nodeInfo_[neighbor].neighbors.end());
    }
    // remove this node and its adjacencies
    nodesToPing_.erase(myNodeName_);
    for (auto const& neighbor : nodeInfo_[myNodeName_].neighbors) {
      nodesToPing_.erase(neighbor);
    }
    break;

  case thrift::HealthCheckOption::PingTopology:
    // ping all nodes in topology
    for (auto const& node : nodeInfo_) {
      nodesToPing_.insert(node.first);
    }
    // remove this node
    nodesToPing_.erase(myNodeName_);
    break;

  case thrift::HealthCheckOption::PingRandom:
    // randomly select nodes based on pct given
    for (auto const& node : nodeInfo_) {
      if (folly::Random::rand32() % 100 < healthCheckPct_) {
        nodesToPing_.insert(node.first);
      }
    }
    // remove this node
    nodesToPing_.erase(myNodeName_);
    break;

  default:
    LOG(ERROR) << "Invalid HealthCheckOption: " << (int32_t)healthCheckOption_
               << ", no nodesToPing_ updated";
    break;
  }
}

void
HealthChecker::sendDatagram(
    const std::string& nodeName,
    folly::SocketAddress const& addr,
    thrift::HealthCheckerMessageType msgType,
    int64_t seqNum) {
  thrift::HealthCheckerMessage message(
      apache::thrift::FRAGILE, myNodeName_, msgType, seqNum);
  const auto packet = fbzmq::util::writeThriftObjStr(message, serializer_);

  sockaddr_storage addrStorage;
  auto addrLen = addr.getAddress(&addrStorage);

  auto bytesSent = ::sendto(
      pingSocketFd_.value(),
      const_cast<char*>(packet.data()),
      packet.size(),
      0,
      reinterpret_cast<struct sockaddr*>(&addrStorage),
      addrLen);

  if ((bytesSent < 0) || (static_cast<size_t>(bytesSent) != packet.size())) {
    LOG(ERROR) << "Failed sending datagram to node: " << nodeName
               << " at IP address: " << addr.getAddressStr();
  }
}

void
HealthChecker::processMessage() {
  CHECK(pingSocketFd_.hasValue());

  std::array<char, kMaxPingPacketSize> buf;

  sockaddr_storage addrStorage;
  socklen_t addrlen = sizeof(addrStorage);
  auto bytesRead = ::recvfrom(
      pingSocketFd_.value(),
      buf.data(),
      kMaxPingPacketSize,
      0,
      reinterpret_cast<struct sockaddr*>(&addrStorage),
      &addrlen);

  std::string readBuf(buf.data(), bytesRead);

  // build the source socket address from recvfrom data
  folly::SocketAddress srcAddr{};
  // this will throw if sender address was not filled in
  srcAddr.setFromSockaddr(
      reinterpret_cast<struct sockaddr*>(&addrStorage), addrlen);

  const auto healthCheckerMessage =
      fbzmq::util::readThriftObjStr<thrift::HealthCheckerMessage>(
          readBuf, serializer_);
  const auto& fromNodeName = healthCheckerMessage.fromNodeName;
  auto& info = nodeInfo_[fromNodeName];
  switch (healthCheckerMessage.type) {
  case thrift::HealthCheckerMessageType::PING: {
    tData_.addStatValue(
        "health_checker.ping_from_" + fromNodeName, 1, fbzmq::COUNT);
    // send an ack now
    sendDatagram(
        fromNodeName,
        srcAddr,
        thrift::HealthCheckerMessageType::ACK,
        healthCheckerMessage.seqNum);
    info.lastAckToNode = healthCheckerMessage.seqNum;
    break;
  }
  case thrift::HealthCheckerMessageType::ACK: {
    info.lastAckFromNode = healthCheckerMessage.seqNum;
    tData_.addStatValue(
        "health_checker.ack_from_" + fromNodeName, 1, fbzmq::COUNT);
    tData_.addStatValue(
        "health_checker.seq_num_diff_" + fromNodeName,
        info.lastValSent - info.lastAckFromNode,
        static_cast<fbzmq::ExportType>(fbzmq::SUM | fbzmq::AVG));
    break;
  }
  default: {
    LOG(ERROR) << "Received unexpected Message type from: " << fromNodeName;
    break;
  }
  }
}

void
HealthChecker::processRequest() {
  auto maybeThriftReq = repSock_.recvThriftObj<thrift::HealthCheckerRequest>(
      serializer_);
  if (maybeThriftReq.hasError()) {
    LOG(ERROR) << "HealthChecker: Error processing request on REP socket: "
               << maybeThriftReq.error();
    return;
  }

  auto thriftReq = maybeThriftReq.value();
  thrift::HealthCheckerPeekReply reply;
  switch (thriftReq.cmd) {
  case thrift::HealthCheckerCmd::PEEK: {
    for (auto const& kv : nodeInfo_) {
      // skip if not pinging the node
      auto const& nodeInfo = kv.second;
      if (nodeInfo.lastAckFromNode == 0 and nodeInfo.lastAckToNode == 0 and
          nodeInfo.lastValSent == 0) {
        continue;
      }
      reply.nodeInfo[kv.first] = nodeInfo;
    }
    break;
  }

  default: {
    LOG(ERROR) << "Health Checker received unknown command: "
               << static_cast<int>(thriftReq.cmd);
    return;
  }
  }

  auto sendRc = repSock_.sendThriftObj(reply, serializer_);
  if (sendRc.hasError()) {
    LOG(ERROR) << "Error sending response: " << sendRc.error();
  }
}

void
HealthChecker::printInfo() {
  VLOG(3) << "HEALTH CHECKER INFO";

  for (auto const& kv : nodeInfo_) {
    VLOG(3) << kv.first << " -->  Sent: " << kv.second.lastValSent
            << "  Ack from: " << kv.second.lastAckFromNode
            << "  Ack to: " << kv.second.lastAckToNode;
  }
}

void
HealthChecker::submitCounters() {
  VLOG(3) << "Submitting counters...";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();
  counters["health_checker.nodes_to_ping_size"] = nodesToPing_.size();
  counters["health_checker.nodes_info_size"] = nodeInfo_.size();

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
}

} // namespace openr
