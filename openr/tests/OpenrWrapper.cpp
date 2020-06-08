/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OpenrWrapper.h"

#include <folly/MapUtil.h>
#include <re2/re2.h>
#include <re2/set.h>

#include <openr/config/tests/Utils.h>

namespace openr {

template <class Serializer>
OpenrWrapper<Serializer>::OpenrWrapper(
    fbzmq::Context& context,
    std::string nodeId,
    bool v4Enabled,
    std::chrono::seconds kvStoreDbSyncInterval,
    std::chrono::milliseconds sparkHoldTime, /*TO BE DEPRECATED*/
    std::chrono::milliseconds sparkKeepAliveTime, /*TO BE DEPRECATED*/
    std::chrono::milliseconds spark2HelloTime,
    std::chrono::milliseconds spark2FastInitHelloTime,
    std::chrono::milliseconds spark2HandshakeTime,
    std::chrono::milliseconds spark2HeartbeatTime,
    std::chrono::milliseconds spark2HandshakeHoldTime,
    std::chrono::milliseconds spark2HeartbeatHoldTime,
    std::chrono::seconds linkMonitorAdjHoldTime,
    std::chrono::milliseconds linkFlapInitialBackoff,
    std::chrono::milliseconds linkFlapMaxBackoff,
    std::chrono::seconds fibColdStartDuration,
    std::shared_ptr<IoProvider> ioProvider,
    int32_t systemPort,
    uint32_t memLimit,
    bool per_prefix_keys)
    : context_(context),
      nodeId_(nodeId),
      ioProvider_(std::move(ioProvider)),
      monitorSubmitUrl_(folly::sformat("inproc://{}-monitor-submit", nodeId_)),
      monitorPubUrl_(folly::sformat("inproc://{}-monitor-pub", nodeId_)),
      kvStoreGlobalCmdUrl_(
          folly::sformat("inproc://{}-kvstore-cmd-global", nodeId_)),
      platformPubUrl_(folly::sformat("inproc://{}-platform-pub", nodeId_)),
      platformPubSock_(context),
      systemPort_(systemPort),
      per_prefix_keys_(per_prefix_keys) {
  // create config
  auto tConfig = getBasicOpenrConfig(
      nodeId_,
      v4Enabled,
      true /*enableSegmentRouting*/,
      false /*orderedFibProgramming*/,
      true /*dryrun*/);

  tConfig.kvstore_config.sync_interval_s = kvStoreDbSyncInterval.count();

  // link monitor config
  auto& lmConf = tConfig.link_monitor_config;
  lmConf.linkflap_initial_backoff_ms = linkFlapInitialBackoff.count();
  lmConf.linkflap_max_backoff_ms = linkFlapMaxBackoff.count();
  lmConf.use_rtt_metric = false;
  lmConf.include_interface_regexes = {"vethLMTest_" + nodeId_ + ".*"};

  // prefix allocation config
  tConfig.enable_prefix_allocation_ref() = true;
  thrift::PrefixAllocationConfig pfxAllocationConf;
  pfxAllocationConf.loopback_interface = "";
  pfxAllocationConf.prefix_allocation_mode =
      thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE;
  pfxAllocationConf.seed_prefix_ref() = "fc00:cafe:babe::/62";
  pfxAllocationConf.allocate_prefix_len_ref() = 64;
  tConfig.prefix_allocation_config_ref() = std::move(pfxAllocationConf);

  // watchdog
  tConfig.enable_watchdog_ref() = true;
  thrift::WatchdogConfig watchdogConf;
  watchdogConf.interval_s = 1;
  watchdogConf.thread_timeout_s = 60;
  watchdogConf.max_memory_mb = memLimit;
  tConfig.watchdog_config_ref() = std::move(watchdogConf);

  config_ = std::make_shared<Config>(tConfig);

  // create zmq monitor
  monitor_ = std::make_unique<fbzmq::ZmqMonitor>(
      MonitorSubmitUrl{monitorSubmitUrl_},
      MonitorPubUrl{monitorPubUrl_},
      context_);

  // create and start config-store thread
  configStore_ = std::make_unique<PersistentStore>(
      nodeId_,
      folly::sformat("/tmp/{}_aq_config_store.bin", nodeId_),
      context_);
  std::thread configStoreThread([this]() noexcept {
    VLOG(1) << nodeId_ << " ConfigStore running.";
    configStore_->run();
    VLOG(1) << nodeId_ << " ConfigStore stopped.";
  });
  configStore_->waitUntilRunning();
  allThreads_.emplace_back(std::move(configStoreThread));

  // create and start kvstore thread
  kvStore_ = std::make_unique<KvStore>(
      context_,
      kvStoreUpdatesQueue_,
      peerUpdatesQueue_.getReader(),
      KvStoreGlobalCmdUrl{kvStoreGlobalCmdUrl_},
      MonitorSubmitUrl{monitorSubmitUrl_},
      config_,
      std::nullopt /* ip-tos */,
      std::unordered_map<std::string, thrift::PeerSpec>{});
  std::thread kvStoreThread([this]() noexcept {
    VLOG(1) << nodeId_ << " KvStore running.";
    kvStore_->run();
    VLOG(1) << nodeId_ << " KvStore stopped.";
  });
  kvStore_->waitUntilRunning();
  allThreads_.emplace_back(std::move(kvStoreThread));

  // kvstore client
  kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
      &eventBase_, nodeId_, kvStore_.get());

  // Subscribe our own prefixDb
  kvStoreClient_->subscribeKey(
      folly::sformat("prefix:{}", nodeId_),
      [&](const std::string& /* key */,
          std::optional<thrift::Value> value) noexcept {
        if (!value.has_value()) {
          return;
        }
        // Parse PrefixDb
        auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            value.value().value_ref().value(), serializer_);

        SYNCHRONIZED(ipPrefix_) {
          bool received = false;

          for (auto& prefix : prefixDb.prefixEntries) {
            if (prefix.type == thrift::PrefixType::PREFIX_ALLOCATOR) {
              received = true;
              ipPrefix_ = prefix.prefix;
              break;
            }
          }
          if (!received) {
            ipPrefix_ = std::nullopt;
          }
        }
      },
      false);

  //
  // create spark
  //
  spark_ = std::make_unique<Spark>(
      "terragraph", // domain name
      nodeId_, // node name
      static_cast<uint16_t>(6666), // multicast port
      sparkHoldTime, // hold time ms
      sparkKeepAliveTime, // keep alive ms
      spark2HelloTime,
      spark2FastInitHelloTime,
      spark2HandshakeTime,
      spark2HeartbeatTime,
      spark2HandshakeHoldTime,
      spark2HeartbeatHoldTime,
      std::nullopt, // ip-tos
      v4Enabled, // enable v4
      interfaceUpdatesQueue_.getReader(),
      neighborUpdatesQueue_,
      KvStoreCmdPort{0},
      OpenrCtrlThriftPort{0},
      std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion),
      ioProvider_);

  //
  // create link monitor
  //
  std::vector<thrift::IpPrefix> networks;
  networks.emplace_back(toIpPrefix(folly::IPAddress::createNetwork("::/0")));
  linkMonitor_ = std::make_unique<LinkMonitor>(
      context_,
      config_,
      static_cast<int32_t>(60099), // platfrom pub port
      kvStore_.get(),
      networks,
      false /* enable perf measurement */,
      interfaceUpdatesQueue_,
      peerUpdatesQueue_,
      neighborUpdatesQueue_.getReader(),
      MonitorSubmitUrl{monitorSubmitUrl_},
      configStore_.get(),
      false, /* assumeDrained */
      prefixUpdatesQueue_,
      PlatformPublisherUrl{platformPubUrl_},
      linkMonitorAdjHoldTime);

  //
  // Create prefix manager
  //
  prefixManager_ = std::make_unique<PrefixManager>(
      prefixUpdatesQueue_.getReader(),
      config_,
      configStore_.get(),
      kvStore_.get(),
      false /* prefix-mananger perf measurement */,
      std::chrono::seconds(0));

  //
  // create decision
  //
  decision_ = std::make_unique<Decision>(
      config_,
      true, // computeLfaPaths
      false, // bgpDryRun
      std::chrono::milliseconds(10),
      std::chrono::milliseconds(250),
      kvStoreUpdatesQueue_.getReader(),
      staticRoutesQueue_.getReader(),
      routeUpdatesQueue_,
      context_);

  //
  // create FIB
  //
  fib_ = std::make_unique<Fib>(
      config_,
      Constants::kFibAgentPort,
      fibColdStartDuration,
      routeUpdatesQueue_.getReader(),
      interfaceUpdatesQueue_.getReader(),
      MonitorSubmitUrl{monitorSubmitUrl_},
      kvStore_.get(),
      context_);

  //
  // create PrefixAllocator
  //
  prefixAllocator_ = std::make_unique<PrefixAllocator>(
      config_,
      kvStore_.get(),
      prefixUpdatesQueue_,
      MonitorSubmitUrl{monitorSubmitUrl_},
      configStore_.get(),
      context_,
      systemPort_ /* system agent port*/,
      Constants::kPrefixAllocatorSyncInterval);

  // Watchdog thread to monitor thread aliveness
  watchdog = std::make_unique<Watchdog>(config_);

  // Zmq monitor client to get counters
  zmqMonitorClient = std::make_unique<fbzmq::ZmqMonitorClient>(
      context_,
      MonitorSubmitUrl{folly::sformat("inproc://{}-monitor-submit", nodeId_)});
}

template <class Serializer>
void
OpenrWrapper<Serializer>::run() {
  try {
    // bind out publisher socket
    VLOG(2) << "Platform Publisher: Binding pub url '" << platformPubUrl_
            << "'";
    platformPubSock_.bind(fbzmq::SocketUrl{platformPubUrl_}).value();
  } catch (std::exception const& e) {
    LOG(FATAL) << "Platform Publisher: could not bind to '" << platformPubUrl_
               << "'" << folly::exceptionStr(e);
  }

  eventBase_.scheduleTimeout(std::chrono::milliseconds(100), [this]() {
    auto link = thrift::LinkEntry(
        apache::thrift::FRAGILE, "vethLMTest_" + nodeId_, 5, true, 1);

    thrift::PlatformEvent msgLink;
    msgLink.eventType = thrift::PlatformEventType::LINK_EVENT;
    msgLink.eventData = fbzmq::util::writeThriftObjStr(link, serializer_);

    // send header of event in the first 2 byte
    platformPubSock_.sendMore(
        fbzmq::Message::from(static_cast<uint16_t>(msgLink.eventType)).value());
    const auto sendNeighEntryLink =
        platformPubSock_.sendThriftObj(msgLink, serializer_);
    if (sendNeighEntryLink.hasError()) {
      LOG(ERROR) << "Error in sending PlatformEventType Entry, event Type: "
                 << folly::get_default(
                        thrift::_PlatformEventType_VALUES_TO_NAMES,
                        msgLink.eventType,
                        "UNKNOWN");
    }
  });

  // start monitor thread
  std::thread monitorThread([this]() noexcept {
    VLOG(1) << nodeId_ << " Monitor running.";
    monitor_->run();
    VLOG(1) << nodeId_ << " Monitor stopped.";
  });
  monitor_->waitUntilRunning();
  allThreads_.emplace_back(std::move(monitorThread));

  // Spawn a PrefixManager thread
  std::thread prefixManagerThread([this]() noexcept {
    VLOG(1) << nodeId_ << " PrefixManager running.";
    prefixManager_->run();
    VLOG(1) << nodeId_ << " PrefixManager stopped.";
  });
  prefixManager_->waitUntilRunning();
  allThreads_.emplace_back(std::move(prefixManagerThread));

  // Spawn a PrefixAllocator thread
  std::thread prefixAllocatorThread([this]() noexcept {
    VLOG(1) << nodeId_ << " PrefixAllocator running.";
    prefixAllocator_->run();
    VLOG(1) << nodeId_ << " PrefixAllocator stopped.";
  });
  prefixAllocator_->waitUntilRunning();
  allThreads_.emplace_back(std::move(prefixAllocatorThread));

  // start spark thread
  std::thread sparkThread([this]() {
    VLOG(1) << nodeId_ << " Spark running.";
    spark_->run();
    VLOG(1) << nodeId_ << " Spark stopped.";
  });
  spark_->waitUntilRunning();
  allThreads_.emplace_back(std::move(sparkThread));

  // start link monitor
  std::thread linkMonitorThread([this]() noexcept {
    VLOG(1) << nodeId_ << " LinkMonitor running.";
    linkMonitor_->setAsMockMode();
    linkMonitor_->run();
    VLOG(1) << nodeId_ << " LinkMonitor stopped.";
  });
  linkMonitor_->waitUntilRunning();
  allThreads_.emplace_back(std::move(linkMonitorThread));

  // start decision
  std::thread decisionThread([this]() noexcept {
    VLOG(1) << nodeId_ << " Decision running.";
    decision_->run();
    VLOG(1) << nodeId_ << " Decision stopped.";
  });
  decision_->waitUntilRunning();
  allThreads_.emplace_back(std::move(decisionThread));

  // start fib
  std::thread fibThread([this]() noexcept {
    VLOG(1) << nodeId_ << " FIB running.";
    fib_->run();
    VLOG(1) << nodeId_ << " FIB stopped.";
  });
  fib_->waitUntilRunning();
  allThreads_.emplace_back(std::move(fibThread));

  // start watchdog
  std::thread watchdogThread([this]() noexcept {
    VLOG(1) << nodeId_ << " watchdog running.";
    watchdog->run();
    VLOG(1) << nodeId_ << " watchdog stopped.";
  });
  watchdog->waitUntilRunning();
  allThreads_.emplace_back(std::move(watchdogThread));

  // start eventBase_
  allThreads_.emplace_back([&]() {
    VLOG(1) << nodeId_ << " Starting eventBase_";
    eventBase_.run();
    VLOG(1) << nodeId_ << " Stopping eventBase_";
  });
}

template <class Serializer>
void
OpenrWrapper<Serializer>::stop() {
  // Close all queues
  routeUpdatesQueue_.close();
  peerUpdatesQueue_.close();
  interfaceUpdatesQueue_.close();
  neighborUpdatesQueue_.close();
  prefixUpdatesQueue_.close();
  kvStoreUpdatesQueue_.close();
  staticRoutesQueue_.close();

  // stop all modules in reverse order
  eventBase_.stop();
  eventBase_.waitUntilStopped();
  watchdog->stop();
  watchdog->waitUntilStopped();
  fib_->stop();
  fib_->waitUntilStopped();
  decision_->stop();
  decision_->waitUntilStopped();
  linkMonitor_->stop();
  linkMonitor_->waitUntilStopped();
  spark_->stop();
  spark_->waitUntilStopped();
  prefixAllocator_->stop();
  prefixAllocator_->waitUntilStopped();
  prefixManager_->stop();
  prefixManager_->waitUntilStopped();
  monitor_->stop();
  monitor_->waitUntilStopped();
  kvStore_->stop();
  kvStore_->waitUntilStopped();
  configStore_->stop();
  configStore_->waitUntilStopped();

  // wait for all threads to finish
  for (auto& t : allThreads_) {
    t.join();
  }

  LOG(INFO) << "OpenR with nodeId: " << nodeId_ << " stopped";
}

template <class Serializer>
std::optional<thrift::IpPrefix>
OpenrWrapper<Serializer>::getIpPrefix() {
  SYNCHRONIZED(ipPrefix_) {
    if (ipPrefix_.has_value()) {
      return ipPrefix_;
    }
  }
  auto keys =
      kvStoreClient_->dumpAllWithPrefix(folly::sformat("prefix:{}", nodeId_));

  SYNCHRONIZED(ipPrefix_) {
    for (const auto& key : keys.value()) {
      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          key.second.value_ref().value(), serializer_);
      if (prefixDb.deletePrefix) {
        // Skip prefixes which are about to be deleted
        continue;
      }

      for (auto& prefix : prefixDb.prefixEntries) {
        if (prefix.type == thrift::PrefixType::PREFIX_ALLOCATOR) {
          ipPrefix_ = prefix.prefix;
          break;
        }
      }
    }
  }
  return ipPrefix_.copy();
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::checkKeyExists(std::string key) {
  auto keys = kvStoreClient_->dumpAllWithPrefix(key);
  return keys.has_value();
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::sparkUpdateInterfaceDb(
    const std::vector<SparkInterfaceEntry>& interfaceEntries) {
  thrift::InterfaceDatabase ifDb(
      apache::thrift::FRAGILE, nodeId_, {}, thrift::PerfEvents());
  ifDb.perfEvents_ref().reset();

  for (const auto& interface : interfaceEntries) {
    ifDb.interfaces.emplace(
        interface.ifName,
        createThriftInterfaceInfo(
            true,
            interface.ifIndex,
            {toIpPrefix(interface.v4Network),
             toIpPrefix(interface.v6LinkLocalNetwork)}));
  }

  interfaceUpdatesQueue_.push(std::move(ifDb));
  return true;
}

template <class Serializer>
thrift::RouteDatabase
OpenrWrapper<Serializer>::fibDumpRouteDatabase() {
  auto routes = fib_->getRouteDb().get();
  return std::move(*routes);
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::addPrefixEntries(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  thrift::PrefixUpdateRequest request;
  request.cmd = thrift::PrefixUpdateCommand::ADD_PREFIXES;
  request.prefixes = prefixes;
  prefixUpdatesQueue_.push(std::move(request));
  return true;
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::withdrawPrefixEntries(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  thrift::PrefixUpdateRequest request;
  request.cmd = thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES;
  request.prefixes = prefixes;
  prefixUpdatesQueue_.push(std::move(request));
  return true;
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::checkPrefixExists(
    const thrift::IpPrefix& prefix, const thrift::RouteDatabase& routeDb) {
  for (auto const& route : routeDb.unicastRoutes) {
    if (prefix == route.dest) {
      return true;
    }
  }
  return false;
}

// define template instance for some common serializers
template class OpenrWrapper<apache::thrift::CompactSerializer>;
template class OpenrWrapper<apache::thrift::BinarySerializer>;
template class OpenrWrapper<apache::thrift::SimpleJSONSerializer>;

} // namespace openr
