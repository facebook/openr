/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SparkWrapper.h"

using namespace fbzmq;

namespace openr {

SparkWrapper::SparkWrapper(
    std::string const& myDomainName,
    std::string const& myNodeName,
    std::chrono::milliseconds myHoldTime,
    std::chrono::milliseconds myKeepAliveTime,
    std::chrono::milliseconds myFastInitKeepAliveTime,
    bool enableV4,
    bool enableSubnetValidation,
    SparkReportUrl const& reportUrl,
    MonitorSubmitUrl const& monitorCmdUrl,
    std::pair<uint32_t, uint32_t> version,
    fbzmq::Context& zmqContext,
    std::shared_ptr<IoProvider> ioProvider,
    folly::Optional<std::unordered_set<std::string>> areas,
    bool enableSpark2,
    std::chrono::milliseconds myHandshakeTime,
    std::chrono::milliseconds myHeartbeatTime,
    std::chrono::milliseconds myNegotiateHoldTime,
    std::chrono::milliseconds myHeartbeatHoldTime)
    : myNodeName_(myNodeName),
      ioProvider_(std::move(ioProvider)),
      reqSock_(zmqContext),
      reportSock_(
          zmqContext,
          fbzmq::IdentityString{Constants::kSparkReportClientId.toString()},
          folly::none,
          fbzmq::NonblockingFlag{false}) {
  spark_ = std::make_shared<Spark>(
      myDomainName,
      myNodeName,
      static_cast<uint16_t>(6666),
      myHoldTime,
      myKeepAliveTime,
      myFastInitKeepAliveTime, // fastInitKeepAliveTime
      myHandshakeTime, // spark2_handshake_time
      myHeartbeatTime, // spark2_heartbeat_time
      myNegotiateHoldTime, // spark2_negotiate_hold_time
      myHeartbeatHoldTime, // spark2_heartbeat_hold_time
      folly::none /* ip-tos */,
      enableV4,
      enableSubnetValidation,
      reportUrl,
      monitorCmdUrl,
      KvStorePubPort{10001},
      KvStoreCmdPort{10002},
      OpenrCtrlThriftPort{2018},
      version,
      zmqContext,
      true,
      enableSpark2,
      areas);

  // start spark
  run();

  reqSock_.connect(fbzmq::SocketUrl{spark_->inprocCmdUrl});

  reportSock_.connect(fbzmq::SocketUrl{reportUrl});
}

SparkWrapper::~SparkWrapper() {
  stop();
}

void
SparkWrapper::run() {
  thread_ = std::make_unique<std::thread>([this]() {
    VLOG(1) << "Spark running.";
    spark_->setIoProvider(ioProvider_);
    spark_->run();
    VLOG(1) << "Spark stopped.";
  });
  spark_->waitUntilRunning();
}

void
SparkWrapper::stop() {
  spark_->stop();
  spark_->waitUntilStopped();
  thread_->join();
}

bool
SparkWrapper::updateInterfaceDb(
    const std::vector<SparkInterfaceEntry>& interfaceEntries) {
  thrift::InterfaceDatabase ifDb(
      apache::thrift::FRAGILE, myNodeName_, {}, thrift::PerfEvents());
  ifDb.perfEvents = folly::none;

  for (const auto& interface : interfaceEntries) {
    ifDb.interfaces.emplace(
        interface.ifName,
        thrift::InterfaceInfo(
            apache::thrift::FRAGILE,
            true,
            interface.ifIndex,
            // TO BE DEPRECATED SOON
            {toBinaryAddress(interface.v4Network.first)},
            {toBinaryAddress(interface.v4Network.first)},
            {toIpPrefix(interface.v4Network),
             toIpPrefix(interface.v6LinkLocalNetwork)}));
  }

  reqSock_.sendThriftObj(ifDb, serializer_);

  auto maybeMsg =
      reqSock_.recvThriftObj<thrift::SparkIfDbUpdateResult>(serializer_);
  if (maybeMsg.hasError()) {
    LOG(ERROR) << "updateInterfaceDb recv SparkIfDbUpdateResult failed: "
               << maybeMsg.error();
    return false;
  }
  auto cmdResult = maybeMsg.value();

  return cmdResult.isSuccess;
}

folly::Expected<thrift::SparkNeighborEvent, Error>
SparkWrapper::recvNeighborEvent(
    folly::Optional<std::chrono::milliseconds> timeout) {
  fbzmq::Message requestIdMsg, delimMsg, thriftMsg;
  const auto ret = reportSock_.recvMultipleTimeout(
      timeout, requestIdMsg, delimMsg, thriftMsg);
  if (ret.hasError()) {
    return folly::makeUnexpected(ret.error());
  }
  const auto maybeMsg =
      thriftMsg.readThriftObj<thrift::SparkNeighborEvent>(serializer_);
  if (maybeMsg.hasError()) {
    return folly::makeUnexpected(maybeMsg.error());
  }
  return maybeMsg.value();
}

//
// Lame-ass attempt to skip unexpected messages, such as RTT
// change event. Trying 3 times is a wild guess, no logic.
//
folly::Optional<thrift::SparkNeighborEvent>
SparkWrapper::waitForEvent(
    const thrift::SparkNeighborEventType eventType,
    folly::Optional<std::chrono::milliseconds> timeout) noexcept {
  // TODO: remove this magic number case for stability
  for (auto i = 0; i < 3; i++) {
    auto maybeEvent = recvNeighborEvent(timeout);
    if (maybeEvent.hasError()) {
      LOG(ERROR) << "recvNeighborEvent failed: " << maybeEvent.error();
      continue;
    }
    auto& event = maybeEvent.value();
    if (eventType == event.eventType) {
      return event;
    }
  }
  return folly::none;
}

std::pair<folly::IPAddress, folly::IPAddress>
SparkWrapper::getTransportAddrs(const thrift::SparkNeighborEvent& event) {
  return {toIPAddress(event.neighbor.transportAddressV4),
          toIPAddress(event.neighbor.transportAddressV6)};
}

folly::Optional<SparkNeighState>
SparkWrapper::getSparkNeighState(
    std::string const& ifName, std::string const& neighborName) {
  return spark_->getSparkNeighState(ifName, neighborName);
}

} // namespace openr
