/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fb303/BaseService.h>
#include <fbzmq/zmq/Zmq.h>

#include <openr/allocators/PrefixAllocator.h>
#include <openr/config/Config.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/kvstore/KvStore.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/monitor/LogSample.h>
#include <openr/monitor/Monitor.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/spark/Spark.h>
#include <openr/spark/SparkWrapper.h>
#include <openr/tests/OpenrThriftServerWrapper.h>
#include <openr/tests/mocks/NetlinkEventsInjector.h>
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
      std::chrono::milliseconds spark2HelloTime,
      std::chrono::milliseconds spark2FastInitHelloTime,
      std::chrono::milliseconds spark2HandshakeTime,
      std::chrono::milliseconds spark2HeartbeatTime,
      std::chrono::milliseconds spark2HandshakeHoldTime,
      std::chrono::milliseconds spark2HeartbeatHoldTime,
      std::chrono::milliseconds spark2GRHoldTime,
      std::chrono::milliseconds linkFlapInitialBackoff,
      std::chrono::milliseconds linkFlapMaxBackoff,
      std::shared_ptr<IoProvider> ioProvider,
      uint32_t memLimit = openr::memLimitMB);

  ~OpenrWrapper() {
    stop();
  }

  // getter for allocated prefix
  std::optional<thrift::IpPrefix> getIpPrefix();

  // checks if the given key exists in the kvstore
  bool checkKeyExists(std::string key);

  // start openr
  void run();

  // stop openr
  void stop();

  /**
   * add interfaceDb for spark to tracking
   * return true upon success and fasle otherwise
   */
  void updateInterfaceDb(const InterfaceDatabase& ifDb);

  /**
   * get route databse from fib
   */
  thrift::RouteDatabase fibDumpRouteDatabase();

  /**
   * add prefix entries into prefix manager using prefix manager client
   */
  bool addPrefixEntries(
      const thrift::PrefixType type,
      const std::vector<thrift::PrefixEntry>& prefixes);

  /**
   * withdraw prefix entries into prefix manager using prefix manager client
   */
  bool withdrawPrefixEntries(
      const thrift::PrefixType type,
      const std::vector<thrift::PrefixEntry>& prefixes);

  /**
   * check if a given prefix exists in routeDb
   */
  static bool checkPrefixExists(
      const thrift::IpPrefix& prefix, const thrift::RouteDatabase& routeDb);

  /*
   * return all counters
   */
  std::map<std::string, int64_t> getCounters();

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
  std::shared_ptr<IoProvider> ioProvider_{nullptr};

  // mocked version of netlinkProtocol socket
  std::unique_ptr<fbnl::MockNetlinkProtocolSocket> nlSock_{nullptr};

  // mocked version of netlink system handler
  std::shared_ptr<NetlinkEventsInjector> nlEventsInjector_{nullptr};

  // IpPrefix
  folly::Synchronized<std::optional<thrift::IpPrefix>> ipPrefix_;

  // event loop to use with KvStoreClientInternal
  OpenrEventBase eventBase_;

  // sub modules owned by this wrapper
  std::shared_ptr<Config> config_;
  std::unique_ptr<PersistentStore> configStore_;
  std::unique_ptr<KvStore> kvStore_;
  std::unique_ptr<KvStoreClientInternal> kvStoreClient_;
  std::unique_ptr<Spark> spark_;
  std::unique_ptr<LinkMonitor> linkMonitor_;
  std::unique_ptr<Monitor> monitor_;
  std::unique_ptr<Decision> decision_;
  std::unique_ptr<Fib> fib_;
  std::unique_ptr<PrefixAllocator> prefixAllocator_;
  std::unique_ptr<PrefixManager> prefixManager_;

  // thrift server for inter-node communication
  std::unique_ptr<OpenrThriftServerWrapper> thriftServer_;

  // sub module communication queues
  const std::string kvStoreGlobalCmdUrl_;
  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue_;
  messaging::ReplicateQueue<InterfaceEvent> interfaceUpdatesQueue_;
  messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue_;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue_;
  messaging::ReplicateQueue<NeighborEvents> neighborUpdatesQueue_;
  messaging::ReplicateQueue<KvStoreSyncEvent> kvStoreSyncEventsQueue_;
  messaging::ReplicateQueue<PrefixEvent> prefixUpdatesQueue_;
  messaging::ReplicateQueue<Publication> kvStoreUpdatesQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRoutesQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> prefixMgrRoutesQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue_;
  messaging::ReplicateQueue<LogSample> logSampleQueue_;
};

} // namespace openr
