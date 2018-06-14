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
    SparkCmdUrl const& cmdUrl,
    MonitorSubmitUrl const& monitorCmdUrl,
    std::pair<uint32_t, uint32_t> version,
    fbzmq::Context& zmqContext,
    std::shared_ptr<IoProvider> ioProvider)
    : myNodeName_(myNodeName),
      ioProvider_(std::move(ioProvider)),
      reqSock_(zmqContext),
      reportSock_(zmqContext) {
  spark_ = std::make_shared<Spark>(
      myDomainName,
      myNodeName,
      static_cast<uint16_t>(6666),
      myHoldTime,
      myKeepAliveTime,
      myFastInitKeepAliveTime /* fastInitKeepAliveTime */,
      folly::none /* ip-tos */,
      enableV4,
      enableSubnetValidation,
      reportUrl,
      cmdUrl,
      monitorCmdUrl,
      KvStorePubPort{10001},
      KvStoreCmdPort{10002},
      version,
      zmqContext);

  // start spark
  run();

  reqSock_.connect(fbzmq::SocketUrl{cmdUrl});

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
    const std::vector<InterfaceEntry>& interfaceEntries) {
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
  auto maybeMsg = reportSock_.recvThriftObj<thrift::SparkNeighborEvent>(
      serializer_, timeout);
  if (maybeMsg.hasError()) {
    return folly::makeUnexpected(maybeMsg.error());
  }
  return maybeMsg.value();
}

} // namespace openr
