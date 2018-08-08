/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/zmq/Zmq.h>

#include <openr/allocators/PrefixAllocator.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/kvstore/KvStore.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/prefix-manager/PrefixManagerClient.h>
#include <openr/spark/Spark.h>
#include <openr/spark/SparkWrapper.h>
#include <openr/watchdog/Watchdog.h>

namespace openr {
// memory limit for watchdog checks
const uint32_t memLimitMB{5000};

/**
 * A utility class to wrap OpenR's Main.cpp
 * when run() got called, internally will run
 * monitor, kvstore, spark, link-monitor, decision, fib in order
 *
 * Not thread-safe, use from the same thread only
 */
template <class Serializer>
class OpenrWrapper {
 public:
  OpenrWrapper(
      fbzmq::Context& context,
      std::string nodeId,
      bool v4Enabled,
      std::chrono::seconds kvStoreDbSyncInterval,
      std::chrono::seconds kvStoreMonitorSubmitInterval,
      std::chrono::milliseconds sparkHoldTime,
      std::chrono::milliseconds sparkKeepAliveTime,
      std::chrono::milliseconds sparkFastInitKeepAliveTime,
      bool enableFullMeshReduction,
      std::chrono::seconds linkMonitorAdjHoldTime,
      std::chrono::milliseconds linkFlapInitialBackoff,
      std::chrono::milliseconds linkFlapMaxBackoff,
      std::chrono::seconds fibColdStartDuration,
      std::shared_ptr<IoProvider> ioProvider,
      int32_t systemPort,
      uint32_t memLimit = openr::memLimitMB);

  ~OpenrWrapper() {
    stop();
  }

  // getter for allocated prefix
  folly::Optional<thrift::IpPrefix> getIpPrefix();

  // checks if the given key exists in the kvstore
  bool checkKeyExists(std::string key);

  // start openr
  void run();

  // stop openr
  void stop();

  /**
   * API to get dump from KvStore.
   * if we pass a prefix, only return keys that match it
   */
  std::unordered_map<std::string /* key */, thrift::Value> kvStoreDumpAll(
      std::string const& prefix = "");

  /**
   * APIs to get existing peers of a KvStore.
   */
  folly::
      Expected<std::unordered_map<std::string, thrift::PeerSpec>, fbzmq::Error>
      getKvStorePeers();

  /**
   * add interfaceDb for spark to tracking
   * return true upon success and fasle otherwise
   */
  bool sparkUpdateInterfaceDb(
      const std::vector<InterfaceEntry>& interfaceEntries);

  /**
   * get route databse from fib
   */
  thrift::RouteDatabase fibDumpRouteDatabase();

  /*
   * to get counters
   */
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient{nullptr};

  /*
   * watchdog thread (used for checking memory limit exceeded)
   */
  std::unique_ptr<Watchdog> watchdog;

 private:
  // Disable copy constructor
  OpenrWrapper(OpenrWrapper const&) = delete;
  OpenrWrapper& operator=(OpenrWrapper const&) = delete;

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  Serializer serializer_;

  // ZmqContext to use for IO Processing
  fbzmq::Context& context_;

  // container of all threads
  std::vector<std::thread> allThreads_{};

  // node id
  const std::string nodeId_;

  // io provider
  std::shared_ptr<IoProvider> ioProvider_;

  // IpPrefix
  folly::Synchronized<folly::Optional<thrift::IpPrefix>> ipPrefix_;

  // event loop to use with KvStoreClient
  fbzmq::ZmqEventLoop eventLoop_;

  // sub modules owned by this wrapper
  std::unique_ptr<PersistentStore> configStore_;
  std::unique_ptr<fbzmq::ZmqMonitor> monitor_;
  std::unique_ptr<KvStore> kvStore_;
  std::unique_ptr<KvStoreClient> kvStoreClient_;
  std::unique_ptr<Spark> spark_;
  std::unique_ptr<LinkMonitor> linkMonitor_;
  std::unique_ptr<Decision> decision_;
  std::unique_ptr<Fib> fib_;
  std::unique_ptr<PrefixAllocator> prefixAllocator_;
  std::unique_ptr<PrefixManager> prefixManager_;
  std::unique_ptr<PrefixManagerClient> prefixManagerClient_;

  // sub module communication zmq urls and ports
  int kvStoreGlobalCmdPort_{0};
  int kvStoreGlobalPubPort_{0};
  const std::string configStoreUrl_;
  const std::string monitorSubmitUrl_;
  const std::string monitorPubUrl_;
  const std::string kvStoreLocalCmdUrl_;
  const std::string kvStoreLocalPubUrl_;
  const std::string kvStoreGlobalCmdUrl_;
  const std::string kvStoreGlobalPubUrl_;
  const std::string prefixManagerLocalCmdUrl_;
  const std::string prefixManagerGlobalCmdUrl_;
  const std::string sparkCmdUrl_;
  const std::string sparkReportUrl_;
  const std::string platformPubUrl_;
  const std::string linkMonitorGlobalCmdUrl_;
  const std::string linkMonitorGlobalPubUrl_;
  const std::string decisionCmdUrl_;
  const std::string decisionPubUrl_;
  const std::string fibCmdUrl_;

  // client sockets mainly for tests
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> kvStoreReqSock_;
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> sparkReqSock_;
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> fibReqSock_;

  // socket to publish platform events
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> platformPubSock_;

  int32_t systemPort_;
};

} // namespace openr
