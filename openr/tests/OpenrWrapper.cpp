/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/OpenrWrapper.h>
#include <openr/config/tests/Utils.h>

namespace openr {

template <class Serializer>
OpenrWrapper<Serializer>::OpenrWrapper(
    fbzmq::Context& context,
    std::string nodeId,
    bool v4Enabled,
    std::chrono::seconds kvStoreDbSyncInterval,
    std::chrono::milliseconds spark2HelloTime,
    std::chrono::milliseconds spark2FastInitHelloTime,
    std::chrono::milliseconds spark2HandshakeTime,
    std::chrono::milliseconds spark2HeartbeatTime,
    std::chrono::milliseconds spark2HandshakeHoldTime,
    std::chrono::milliseconds spark2HeartbeatHoldTime,
    std::chrono::milliseconds spark2GRHoldTime,
    std::chrono::seconds linkMonitorAdjHoldTime,
    std::chrono::milliseconds linkFlapInitialBackoff,
    std::chrono::milliseconds linkFlapMaxBackoff,
    std::chrono::seconds fibColdStartDuration,
    std::shared_ptr<IoProvider> ioProvider,
    uint32_t memLimit)
    : context_(context),
      nodeId_(nodeId),
      ioProvider_(std::move(ioProvider)),
      kvStoreGlobalCmdUrl_(
          folly::sformat("inproc://{}-kvstore-cmd-global", nodeId_)) {
  // create config
  auto tConfig = getBasicOpenrConfig(
      nodeId_,
      "terragraph",
      {}, /* area config */
      v4Enabled,
      true /*enableSegmentRouting*/,
      false /*orderedFibProgramming*/,
      true /*dryrun*/);

  *tConfig.kvstore_config_ref()->sync_interval_s_ref() =
      kvStoreDbSyncInterval.count();

  // link monitor config
  auto& lmConf = *tConfig.link_monitor_config_ref();
  *lmConf.linkflap_initial_backoff_ms_ref() = linkFlapInitialBackoff.count();
  *lmConf.linkflap_max_backoff_ms_ref() = linkFlapMaxBackoff.count();
  *lmConf.use_rtt_metric_ref() = false;
  *lmConf.include_interface_regexes_ref() = {".*"};

  // prefix allocation config
  tConfig.enable_prefix_allocation_ref() = true;
  thrift::PrefixAllocationConfig pfxAllocationConf;
  *pfxAllocationConf.loopback_interface_ref() = "";
  pfxAllocationConf.prefix_allocation_mode_ref() =
      thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE;
  pfxAllocationConf.seed_prefix_ref() = "fc00:cafe:babe::/62";
  pfxAllocationConf.allocate_prefix_len_ref() = 64;
  tConfig.prefix_allocation_config_ref() = std::move(pfxAllocationConf);

  // watchdog
  tConfig.enable_watchdog_ref() = true;
  thrift::WatchdogConfig watchdogConf;
  watchdogConf.interval_s_ref() = 1;
  watchdogConf.thread_timeout_s_ref() = 60;
  watchdogConf.max_memory_mb_ref() = memLimit;
  tConfig.watchdog_config_ref() = std::move(watchdogConf);

  // monitor
  thrift::MonitorConfig monitorConf;
  // disable log submission for testing
  monitorConf.enable_event_log_submission_ref() = false;
  tConfig.monitor_config_ref() = std::move(monitorConf);

  config_ = std::make_shared<Config>(tConfig);

  // create fakeNetlinkProtocolSocket
  folly::EventBase evb;
  nlSock_ = std::make_unique<fbnl::MockNetlinkProtocolSocket>(&evb);

  // create netlinkEventInjector
  nlEventsInjector_ = std::make_shared<NetlinkEventsInjector>(nlSock_.get());

  // create and start config-store thread
  configStore_ = std::make_unique<PersistentStore>(
      folly::sformat("/tmp/{}_aq_config_store.bin", nodeId_));
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
      kvStoreSyncEventsQueue_,
      peerUpdatesQueue_.getReader(),
      logSampleQueue_,
      KvStoreGlobalCmdUrl{kvStoreGlobalCmdUrl_},
      config_,
      std::nullopt /* ip-tos */);
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
      kTestingAreaName,
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

          for (auto& prefix : *prefixDb.prefixEntries_ref()) {
            if (*prefix.type_ref() == thrift::PrefixType::PREFIX_ALLOCATOR) {
              received = true;
              ipPrefix_ = *prefix.prefix_ref();
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
      std::nullopt, // ip-tos
      interfaceUpdatesQueue_.getReader(),
      neighborUpdatesQueue_,
      KvStoreCmdPort{0},
      OpenrCtrlThriftPort{0},
      ioProvider_,
      config_);

  //
  // create link monitor
  //
  linkMonitor_ = std::make_unique<LinkMonitor>(
      config_,
      nlSock_.get(),
      kvStore_.get(),
      configStore_.get(),
      false /* enable perf measurement */,
      interfaceUpdatesQueue_,
      prefixUpdatesQueue_,
      peerUpdatesQueue_,
      logSampleQueue_,
      neighborUpdatesQueue_.getReader(),
      kvStoreSyncEventsQueue_.getReader(),
      nlSock_->getReader(),
      false, /* assumeDrained */
      false, /* overrideDrainState */
      linkMonitorAdjHoldTime);

  //
  // create monitor
  //
  monitor_ = std::make_unique<openr::Monitor>(
      config_,
      Constants::kEventLogCategory.toString(),
      logSampleQueue_.getReader());

  //
  // Create prefix manager
  //
  prefixManager_ = std::make_unique<PrefixManager>(
      staticRoutesQueue_,
      prefixUpdatesQueue_.getReader(),
      routeUpdatesQueue_.getReader(),
      config_,
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
      routeUpdatesQueue_);

  //
  // create FIB
  //
  fib_ = std::make_unique<Fib>(
      config_,
      Constants::kFibAgentPort,
      fibColdStartDuration,
      routeUpdatesQueue_.getReader(),
      staticRoutesQueue_.getReader(),
      fibUpdatesQueue_,
      logSampleQueue_,
      kvStore_.get());

  //
  // create PrefixAllocator
  //
  prefixAllocator_ = std::make_unique<PrefixAllocator>(
      kTestingAreaName,
      config_,
      nlSock_.get(),
      kvStore_.get(),
      configStore_.get(),
      prefixUpdatesQueue_,
      logSampleQueue_,
      Constants::kPrefixAllocatorSyncInterval);

  // Watchdog thread to monitor thread aliveness
  watchdog = std::make_unique<Watchdog>(config_);
}

template <class Serializer>
void
OpenrWrapper<Serializer>::run() {
  eventBase_.scheduleTimeout(std::chrono::milliseconds(100), [this]() {
    // mimick nlSock to generate LINK event
    // ATTN: LinkMonitor will be notified as it holds the reader queue
    //       from the same MockNetlinkProtocolSocket
    nlEventsInjector_->sendLinkEvent(
        "vethLMTest_" + nodeId_, /* ifName */
        5, /* ifIndex */
        true /* isUp */);
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
  kvStoreSyncEventsQueue_.close();
  nlSock_->closeQueue();
  prefixUpdatesQueue_.close();
  kvStoreUpdatesQueue_.close();
  staticRoutesQueue_.close();
  fibUpdatesQueue_.close();
  logSampleQueue_.close();

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

  // destroy netlink related objects
  nlEventsInjector_.reset();
  nlSock_.reset();
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

  std::optional<std::unordered_map<std::string, thrift::Value>> keys;
  eventBase_.getEvb()->runInEventBaseThreadAndWait([&]() {
    keys = kvStoreClient_->dumpAllWithPrefix(
        kTestingAreaName, folly::sformat("prefix:{}", nodeId_));
  });

  SYNCHRONIZED(ipPrefix_) {
    for (const auto& key : keys.value()) {
      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          key.second.value_ref().value(), serializer_);
      if (*prefixDb.deletePrefix_ref()) {
        // Skip prefixes which are about to be deleted
        continue;
      }

      for (auto& prefix : *prefixDb.prefixEntries_ref()) {
        if (*prefix.type_ref() == thrift::PrefixType::PREFIX_ALLOCATOR) {
          ipPrefix_ = *prefix.prefix_ref();
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
  std::optional<std::unordered_map<std::string, thrift::Value>> keys;
  eventBase_.getEvb()->runInEventBaseThreadAndWait([&]() {
    keys = kvStoreClient_->dumpAllWithPrefix(
        kTestingAreaName, folly::sformat("prefix:{}", nodeId_));
  });
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
    ifDb.interfaces_ref()->emplace(
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
  request.cmd_ref() = thrift::PrefixUpdateCommand::ADD_PREFIXES;
  *request.prefixes_ref() = prefixes;
  prefixUpdatesQueue_.push(std::move(request));
  return true;
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::withdrawPrefixEntries(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  thrift::PrefixUpdateRequest request;
  request.cmd_ref() = thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES;
  *request.prefixes_ref() = prefixes;
  prefixUpdatesQueue_.push(std::move(request));
  return true;
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::checkPrefixExists(
    const thrift::IpPrefix& prefix, const thrift::RouteDatabase& routeDb) {
  for (auto const& route : *routeDb.unicastRoutes_ref()) {
    if (prefix == *route.dest_ref()) {
      return true;
    }
  }
  return false;
}

template <class Serializer>
std::map<std::string, int64_t>
OpenrWrapper<Serializer>::getCounters() {
  return facebook::fb303::fbData->getCounters();
}

// define template instance for some common serializers
template class OpenrWrapper<apache::thrift::CompactSerializer>;
template class OpenrWrapper<apache::thrift::BinarySerializer>;
template class OpenrWrapper<apache::thrift::SimpleJSONSerializer>;

} // namespace openr
