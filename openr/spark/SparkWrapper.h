/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Spark.h"

namespace openr {

struct InterfaceEntry {
  std::string ifName;
  int ifIndex;
  folly::CIDRNetwork v4Network;
  folly::CIDRNetwork v6LinkLocalNetwork;
};

/**
 * A utility class to wrap and interact with Spark. It exposes the APIs to
 * send commands to and receive publications from Spark.
 * Mainly used for testing.
 *
 * This should be managed from only one thread. Otherwise behaviour will be
 * undesirable.
 */
class SparkWrapper {
 public:
  SparkWrapper(
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
      std::shared_ptr<IoProvider> ioProvider);

  ~SparkWrapper();

  // start spark
  void run();

  // stop spark
  void stop();

  // add interfaceDb for Spark to tracking
  // return true upon success and false otherwise
  bool updateInterfaceDb(const std::vector<InterfaceEntry>& interfaceEntries);

  // receive spark neighbor event
  folly::Expected<thrift::SparkNeighborEvent, fbzmq::Error> recvNeighborEvent(
      folly::Optional<std::chrono::milliseconds> timeout = folly::none);

  //
  // Private state
  //

 private:
  std::string myNodeName_{""};

  // io provider
  std::shared_ptr<IoProvider> ioProvider_;

  // this is used to communicate events to downstream consumer
  const std::string reportUrl_{""};

  // this is used to add/remove network interfaces for tracking
  const std::string cmdUrl_{""};

  // DEALER socket for submitting our monitor
  const std::string monitorCmdUrl_{""};

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  apache::thrift::CompactSerializer serializer_;

  // ZMQ request socket for interacting with Spark's command socket
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> reqSock_;

  // ZMQ pair socket for listening realtime updates from Spark
  fbzmq::Socket<ZMQ_PAIR, fbzmq::ZMQ_CLIENT> reportSock_;

  // Spark owned by this wrapper.
  std::shared_ptr<Spark> spark_{nullptr};

  // Thread in which Spark will be running.
  std::unique_ptr<std::thread> thread_{nullptr};
};

} // namespace openr
