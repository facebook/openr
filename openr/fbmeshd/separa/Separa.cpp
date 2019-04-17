/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Separa.h"

#include <fcntl.h>
#include <sys/socket.h>

#include <fbzmq/zmq/Common.h>
#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>

#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/fbmeshd/common/Util.h>
#include <openr/if/gen-cpp2/Decision_types.h>
#include <openr/spark/IoProvider.h>

using namespace openr::fbmeshd;

Separa::Separa(
    uint16_t const udpHelloPort,
    std::chrono::seconds const broadcastInterval,
    std::chrono::seconds const domainLockdownPeriod,
    double const domainChangeThresholdFactor,
    const bool backwardsCompatibilityMode,
    Nl80211Handler& nlHandler,
    MeshSpark& meshSpark,
    const openr::PrefixManagerLocalCmdUrl& prefixManagerCmdUrl,
    const openr::DecisionCmdUrl& decisionCmdUrl,
    const openr::KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
    const openr::KvStoreLocalPubUrl& kvStoreLocalPubUrl,
    const openr::MonitorSubmitUrl& monitorSubmitUrl,
    fbzmq::Context& zmqContext)
    : udpHelloPort_(udpHelloPort),
      broadcastInterval_(broadcastInterval),
      domainLockdownPeriod_{domainLockdownPeriod},
      domainChangeThresholdFactor_{domainChangeThresholdFactor},
      backwardsCompatibilityMode_{backwardsCompatibilityMode},
      nlHandler_{nlHandler},
      meshSpark_{meshSpark},
      prefixManagerClient_{prefixManagerCmdUrl, zmqContext, 3000ms},
      decisionCmdUrl_{decisionCmdUrl},
      kvStoreClient_(
          zmqContext,
          this,
          "unused", /* nodeId is used for writing to kvstore. not used here */
          kvStoreLocalCmdUrl,
          kvStoreLocalPubUrl),
      zmqMonitorClient_{zmqContext, monitorSubmitUrl},
      zmqContext_{zmqContext} {
  // Initialize ZMQ sockets
  scheduleTimeout(std::chrono::seconds(0), [this]() { prepare(); });

  // Schedule periodic timer for submission to monitor
  monitorTimer_ = fbzmq::ZmqTimeout::make(this, [this]() noexcept {
    zmqMonitorClient_.setCounters(
        openr::prepareSubmitCounters(tData_.getCounters()));
  });
  monitorTimer_->scheduleTimeout(
      openr::Constants::kMonitorSubmitInterval, true);
}

void
Separa::prepare() noexcept {
  socketFd_ = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  CHECK_NE(socketFd_, -1) << folly::errnoStr(errno);

  // make socket non-blocking
  CHECK_EQ(fcntl(socketFd_, F_SETFL, O_NONBLOCK), 0) << folly::errnoStr(errno);

  // make v6 only
  {
    int v6Only = 1;
    CHECK_EQ(
        setsockopt(
            socketFd_, IPPROTO_IPV6, IPV6_V6ONLY, &v6Only, sizeof(v6Only)),
        0)
        << folly::errnoStr(errno);
  }

  // not really needed, but helps us use same port with other listeners, if
  // any
  {
    int reuseAddr = 1;
    CHECK_EQ(
        setsockopt(
            socketFd_, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr)),
        0)
        << folly::errnoStr(errno);
  }

  if (!backwardsCompatibilityMode_) {
    // request additional packet info, e.g. input iface index and sender address
    {
      int recvPktInfo = 1;
      CHECK_NE(
          setsockopt(
              socketFd_,
              IPPROTO_IPV6,
              IPV6_RECVPKTINFO,
              &recvPktInfo,
              sizeof(recvPktInfo)),
          -1)
          << folly::errnoStr(errno);
    }

    //
    // bind the socket to receive any separa packet
    //
    {
      auto mcastSockAddr =
          folly::SocketAddress(folly::IPAddress("::"), udpHelloPort_);

      sockaddr_storage addrStorage;
      mcastSockAddr.getAddress(&addrStorage);
      sockaddr* saddr = reinterpret_cast<sockaddr*>(&addrStorage);

      CHECK_EQ(bind(socketFd_, saddr, mcastSockAddr.getActualSize()), 0)
          << folly::errnoStr(errno);
    }

    // allow reporting the packet TTL to user space
    {
      int recvHopLimit = 1;
      if (::setsockopt(
              socketFd_,
              IPPROTO_IPV6,
              IPV6_RECVHOPLIMIT,
              &recvHopLimit,
              sizeof(recvHopLimit)) != 0) {
        LOG(FATAL) << "Failed enabling TTL receive on socket. Error: "
                   << folly::errnoStr(errno);
      }
    }

    // Listen for incoming messages on socket FD
    addSocketFd(socketFd_, ZMQ_POLLIN, [this](int) noexcept {
      try {
        processHelloPacket();
      } catch (std::exception const& err) {
        LOG(ERROR) << "Separa: error processing hello packet "
                   << folly::exceptionStr(err);
      }
    });
  }

  broadcastTimer_ = fbzmq::ZmqTimeout::make(
      this, [this]() mutable noexcept { sendHelloPacket(); });
  broadcastTimer_->scheduleTimeout(broadcastInterval_, true);
}

void
Separa::prepareDecisionCmdSocket() noexcept {
  if (decisionCmdSock_) {
    return;
  }
  decisionCmdSock_ =
      std::make_unique<fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>>(zmqContext_);
  const auto decisionCmd =
      decisionCmdSock_->connect(fbzmq::SocketUrl{decisionCmdUrl_});
  if (decisionCmd.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << decisionCmdUrl_ << "' "
               << decisionCmd.error();
  }
}

void
Separa::processHelloPacket() {
  // the read buffer
  uint8_t buf[1280];

  ssize_t bytesRead;
  int ifIndex;
  folly::SocketAddress clientAddr;
  int hopLimit;
  std::chrono::microseconds myRecvTime;
  IoProvider ioProvider;

  std::tie(bytesRead, ifIndex, clientAddr, hopLimit, myRecvTime) =
      IoProvider::recvMessage(socketFd_, buf, 1280, &ioProvider);

  VLOG(9) << "Received message from " << clientAddr.getAddressStr();
  VLOG(9) << "Read a total of " << bytesRead << " bytes from fd " << socketFd_;

  if (static_cast<size_t>(bytesRead) > 1280) {
    LOG(ERROR) << "Message from " << clientAddr.getAddressStr()
               << " has been truncated";
    return;
  }

  tData_.addStatValue("fbmeshd.separa.hello_packet_recv", 1, fbzmq::SUM);

  if (domainLockUntil_.hasValue() &&
      std::chrono::steady_clock::now() < *domainLockUntil_) {
    VLOG(9) << "Domain is locked, ignoring hello packet";
    tData_.addStatValue("fbmeshd.separa.hello_packet_dropped", 1, fbzmq::SUM);
    return;
  }

  tData_.addStatValue("fbmeshd.separa.hello_packet_processed", 1, fbzmq::SUM);

  // Copy buffer into string object and parse it into helloPacket.
  std::string readBuf(reinterpret_cast<const char*>(&buf[0]), bytesRead);
  thrift::SeparaPayload helloPacket;
  try {
    helloPacket = fbzmq::util::readThriftObjStr<thrift::SeparaPayload>(
        readBuf, serializer_);
  } catch (std::exception const& err) {
    LOG(ERROR) << "Failed parsing hello packet " << folly::exceptionStr(err);
    return;
  }

  const auto& netif = nlHandler_.lookupMeshNetif();
  const auto helloPacketDomain = folly::MacAddress::fromNBO(helloPacket.domain);
  folly::Optional<folly::MacAddress> helloPacketDesiredDomain =
      helloPacket.desiredDomain == 0
      ? folly::Optional<folly::MacAddress>{folly::none}
      : folly::MacAddress::fromNBO(helloPacket.desiredDomain);
  const auto maybeFromNodeName =
      folly::IPAddressV6{clientAddr.getAddressStr()}.getMacAddressFromEUI64();
  if (!maybeFromNodeName.hasValue()) {
    LOG(ERROR) << "Received Separa hello from unexpected source";
    return;
  }
  const auto fromNodeName{*maybeFromNodeName};
  const auto currentDomain = meshSpark_.getDomain();

  VLOG(9) << "Received Separa Hello: " << helloPacketDomain << ", "
          << helloPacket.metricToGate << ", "
          << helloPacketDesiredDomain.value_or(folly::MacAddress::fromNBO(0));

  meshSpark_.updateCache(
      fromNodeName,
      std::make_pair(
          helloPacketDesiredDomain, helloPacket.enabled.value_or(true)));

  if (!helloPacket.enabled) {
    return;
  }
  auto newDomain{currentDomain};
  if (helloPacketDomain == *netif.maybeMacAddress) {
    // The packet says that it belongs to self domain.
    if (helloPacket.metricToGate == 0) {
      // we are ourselves a gate
      newDomain = *netif.maybeMacAddress;
    } else if (helloPacket.metricToGate == std::numeric_limits<int>::max()) {
      // we don't have default route, set our domain to null
      newDomain = folly::none;
    }
  } else if (
      helloPacketDomain != currentDomain &&
      helloPacket.metricToGate != std::numeric_limits<int>::max()) {
    // the packet is from another domain
    auto myGate = getGateway();
    if (myGate.hasError()) {
      LOG(ERROR) << "Failed getting gateway";
      return;
    }

    const auto metrics = nlHandler_.getMetrics();

    if (metrics.count(fromNodeName) == 0) {
      LOG(ERROR) << "Failed to get 11s metric for the source node";
      return;
    }

    if ((helloPacket.metricToGate + metrics.at(fromNodeName)) *
            domainChangeThresholdFactor_ <
        myGate->second) {
      if (myGate->second != std::numeric_limits<int>::max()) {
        tData_.addStatValue(
            "fbmeshd.separa.domain_change_mygate_metric",
            myGate->second,
            fbzmq::AVG);
        tData_.addStatValue(
            "fbmeshd.separa.domain_change_trigger_factor",
            100.0 * myGate->second /
                (helloPacket.metricToGate + metrics.at(fromNodeName)),
            fbzmq::AVG);
      }
      tData_.addStatValue(
          "fbmeshd.separa.domain_change_new_metric",
          helloPacket.metricToGate + metrics.at(fromNodeName),
          fbzmq::AVG);
      newDomain = helloPacketDomain;
    }
  }

  if (newDomain != currentDomain) {
    tData_.addStatValue("fbmeshd.separa.num_domain_change", 1, fbzmq::SUM);
    meshSpark_.setDomain(newDomain);
    // lock domain for domainLockdownPeriod_
    domainLockUntil_ = std::chrono::steady_clock::now() + domainLockdownPeriod_;
  }
}

void
Separa::sendHelloPacket() {
  const auto myGate = getGateway();
  if (myGate.hasError()) {
    LOG(ERROR) << "Failed getting gateway";
    tData_.addStatValue(
        "fbmeshd.separa.hello_packet_send_gateway_retrieval_failed",
        1,
        fbzmq::SUM);
    return;
  }

  const auto myDomain = meshSpark_.getDomain();
  const std::string packet{fbzmq::util::writeThriftObjStr(
      thrift::SeparaPayload{apache::thrift::FRAGILE,
                            myGate->first.u64NBO(),
                            myGate->second,
                            myDomain.hasValue() ? myDomain->u64NBO() : 0,
                            !backwardsCompatibilityMode_},
      serializer_)};

  VLOG(9) << "Sending Separa hello packet: " << myGate->first.u64NBO() << ", "
          << myGate->second;

  const auto& netif = nlHandler_.lookupMeshNetif();
  const folly::IPAddressV6 srcAddr{folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
                                   netif.maybeMacAddress.value()};
  openr::IoProvider ioProvider{};

  // Send hello packets to ourselves, and all our neighbors
  std::vector<folly::MacAddress> stations;
  for (const auto& station : nlHandler_.getStationsInfo()) {
    stations.push_back(station.macAddress);
  }
  stations.push_back(*netif.maybeMacAddress);

  for (const auto& station : stations) {
    const folly::SocketAddress dstAddr{
        folly::IPAddressV6{folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
                           station},
        udpHelloPort_};

    auto bytesSent = openr::IoProvider::sendMessage(
        socketFd_,
        netif.maybeIfIndex.value(),
        srcAddr,
        dstAddr,
        packet,
        &ioProvider);

    if ((bytesSent < 0) || (static_cast<size_t>(bytesSent) != packet.size())) {
      LOG(ERROR) << "Sending hello to " << dstAddr.getAddressStr()
                 << "failed due to error " << folly::errnoStr(errno);
      tData_.addStatValue(
          "fbmeshd.separa.hello_packet_send_failed", 1, fbzmq::SUM);
      return;
    }
    tData_.addStatValue("fbmeshd.separa.hello_packet_sent", 1, fbzmq::SUM);
  }
}

folly::Expected<std::pair<folly::MacAddress, int>, folly::Unit>
Separa::getGateway() {
  // Check if we ourselves are a gate.
  auto ret = prefixManagerClient_.getPrefixes();
  if (ret.hasError()) {
    LOG(ERROR) << "Failed getting prefixes from OpenR: " << ret.error();
    tData_.addStatValue(
        "fbmeshd.separa.get_gateway.openr_prefix_retrieval_zmq_error",
        1,
        fbzmq::SUM);
    return folly::makeUnexpected(folly::unit);
  }
  if (!ret->success) {
    tData_.addStatValue(
        "fbmeshd.separa.get_gateway.openr_prefix_retrieval_failure",
        1,
        fbzmq::SUM);
    return folly::makeUnexpected(folly::unit);
  }
  if (std::any_of(
          ret->prefixes.begin(),
          ret->prefixes.end(),
          [](const auto& prefixEntry) {
            return prefixEntry.prefix == openr::toIpPrefix("0.0.0.0/0");
          })) {
    // We are a gate
    tData_.addStatValue("fbmeshd.separa.get_gateway.is_gate", 1, fbzmq::SUM);
    return std::make_pair(
        nlHandler_.lookupMeshNetif().maybeMacAddress.value(), 0);
  }

  // Check if we can reach to a gate
  prepareDecisionCmdSocket();

  folly::MacAddress currentNode =
      nlHandler_.lookupMeshNetif().maybeMacAddress.value();
  std::unordered_set<folly::MacAddress> visitedNodes;
  folly::Optional<int> metric;
  while (true) {
    {
      const auto res = decisionCmdSock_->sendThriftObj(
          openr::thrift::DecisionRequest(
              apache::thrift::FRAGILE,
              openr::thrift::DecisionCommand::ROUTE_DB_GET,
              macAddrToNodeName(currentNode)),
          serializer_);
      if (res.hasError()) {
        LOG(ERROR) << "Exception in sending ROUTE_DB_GET command to Decision. "
                   << " exception: " << res.error();
        decisionCmdSock_.reset();
        tData_.addStatValue(
            "fbmeshd.separa.get_gateway.decision_socket_send_zmq_error",
            1,
            fbzmq::SUM);
        return folly::makeUnexpected(folly::unit);
      }
    }

    const auto res =
        decisionCmdSock_->recvThriftObj<openr::thrift::DecisionReply>(
            serializer_, openr::Constants::kReadTimeout);
    if (res.hasError()) {
      LOG(ERROR) << "Exception in recieving response from Decision. "
                 << " exception: " << res.error();
      decisionCmdSock_.reset();
      tData_.addStatValue(
          "fbmeshd.separa.get_gateway.decision_socket_receive_zmq_error",
          1,
          fbzmq::SUM);
      return folly::makeUnexpected(folly::unit);
    }

    const auto route = std::find_if(
        res->routeDb.unicastRoutes.begin(),
        res->routeDb.unicastRoutes.end(),
        [](const auto& route) {
          return route.dest == openr::toIpPrefix("0.0.0.0/0");
        });
    if (route == res->routeDb.unicastRoutes.end()) {
      tData_.addStatValue(
          "fbmeshd.separa.get_gateway.is_non_gate", 1, fbzmq::SUM);
      const auto returnValue = metric.value_or(std::numeric_limits<int>::max());
      tData_.addStatValue(
          "fbmeshd.separa.get_gateway.metric", returnValue, fbzmq::AVG);
      return std::make_pair(currentNode, returnValue);
    }

    const auto ip = openr::toIPAddress(route->nextHops.front().address);
    if (!ip.isV4()) {
      tData_.addStatValue(
          "fbmeshd.separa.get_gateway.not_ipv4_error", 1, fbzmq::SUM);
      return folly::makeUnexpected(folly::unit);
    }

    const auto node = kvStoreClient_.getKey(folly::sformat(
        "{}{}",
        openr::Constants::kPrefixAllocMarker,
        openr::toIPAddress(route->nextHops.front().address)
            .asV4()
            .getNthLSByte(0)));
    if (!node.hasValue()) {
      tData_.addStatValue(
          "fbmeshd.separa.get_gateway.node_not_in_kvstore_error",
          1,
          fbzmq::SUM);
      return folly::makeUnexpected(folly::unit);
    }

    const auto maybeNode = nodeNameToMacAddr(node->originatorId);
    if (!maybeNode.hasValue()) {
      tData_.addStatValue(
          "fbmeshd.separa.get_gateway.node_name_malformed", 1, fbzmq::SUM);
      return folly::makeUnexpected(folly::unit);
    }

    metric = metric.value_or(0) + route->nextHops.front().metric;

    visitedNodes.emplace(currentNode);
    currentNode = *maybeNode;
    if (visitedNodes.count(currentNode) > 0) {
      tData_.addStatValue(
          "fbmeshd.separa.get_gateway.routing_loop", 1, fbzmq::SUM);
      return folly::makeUnexpected(folly::unit);
    }
  }
}
