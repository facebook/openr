/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/OpenrWrapper.h>
#include <openr/tests/utils/Utils.h>

namespace openr {

template <class Serializer>
OpenrWrapper<Serializer>::OpenrWrapper(
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
    uint32_t memLimit)
    : nodeId_(nodeId), ioProvider_(std::move(ioProvider)) {
  // create config
  auto tConfig = getBasicOpenrConfig(
      nodeId_,
      {}, /* area config */
      v4Enabled,
      true /* enableSegmentRouting */,
      true /* dryrun */);

  // link monitor config
  auto& lmConf = *tConfig.link_monitor_config();
  lmConf.linkflap_initial_backoff_ms() = linkFlapInitialBackoff.count();
  lmConf.linkflap_max_backoff_ms() = linkFlapMaxBackoff.count();
  lmConf.use_rtt_metric() = false;
  lmConf.enable_perf_measurement() = false;
  tConfig.assume_drained() = false;

  // decision config
  tConfig.decision_config()->debounce_min_ms() = 10;
  tConfig.decision_config()->debounce_max_ms() = 250;

  // watchdog
  tConfig.enable_watchdog() = true;
  thrift::WatchdogConfig watchdogConf;
  watchdogConf.interval_s() = 1;
  watchdogConf.thread_timeout_s() = 60;
  watchdogConf.max_memory_mb() = memLimit;
  tConfig.watchdog_config() = std::move(watchdogConf);

  // monitor
  thrift::MonitorConfig monitorConf;
  // disable log submission for testing
  monitorConf.enable_event_log_submission() = false;
  tConfig.monitor_config() = std::move(monitorConf);

  // persistent config-store config
  tConfig.persistent_config_store_path() =
      fmt::format("/tmp/{}_openr_config_store.bin", nodeId_);

  // spark
  tConfig.enable_neighbor_monitor() = true;

  config_ = std::make_shared<Config>(tConfig);

  // create MockNetlinkProtocolSocket
  folly::EventBase evb;
  nlSock_ = std::make_unique<fbnl::MockNetlinkProtocolSocket>(&evb);

  // create netlinkEventInjector
  nlEventsInjector_ = std::make_shared<NetlinkEventsInjector>(nlSock_.get());

  // create and start config-store thread
  configStore_ = std::make_unique<PersistentStore>(config_);
  std::thread configStoreThread([this]() noexcept {
    VLOG(1) << nodeId_ << " ConfigStore running.";
    configStore_->run();
    VLOG(1) << nodeId_ << " ConfigStore stopped.";
  });
  configStore_->waitUntilRunning();
  allThreads_.emplace_back(std::move(configStoreThread));

  // create and start kvstore thread
  kvStore_ = std::make_unique<KvStore<thrift::OpenrCtrlCppAsyncClient>>(
      kvStoreUpdatesQueue_,
      peerUpdatesQueue_.getReader(),
      kvRequestQueue_.getReader(),
      logSampleQueue_,
      config_->getAreaIds(),
      config_->toThriftKvStoreConfig());
  std::thread kvStoreThread([this]() noexcept {
    VLOG(1) << nodeId_ << " KvStore running.";
    kvStore_->run();
    VLOG(1) << nodeId_ << " KvStore stopped.";
  });
  kvStore_->waitUntilRunning();
  allThreads_.emplace_back(std::move(kvStoreThread));

  //
  // create spark
  //
  spark_ = std::make_unique<Spark>(
      interfaceUpdatesQueue_.getReader(),
      initializationEventQueue_.getReader(),
      addrEventQueue_.getReader(),
      neighborUpdatesQueue_,
      ioProvider_,
      config_);

  //
  // create link monitor
  //
  linkMonitor_ = std::make_unique<LinkMonitor>(
      config_,
      nlSock_.get(),
      configStore_.get(),
      interfaceUpdatesQueue_,
      prefixUpdatesQueue_,
      peerUpdatesQueue_,
      logSampleQueue_,
      kvRequestQueue_,
      neighborUpdatesQueue_.getReader(),
      nlSock_->getReader());

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
      kvRequestQueue_,
      initializationEventQueue_,
      kvStoreUpdatesQueue_.getReader(),
      prefixUpdatesQueue_.getReader(),
      fibRouteUpdatesQueue_.getReader(),
      config_);

  //
  // create decision
  //
  decision_ = std::make_unique<Decision>(
      config_,
      peerUpdatesQueue_.getReader(),
      kvStoreUpdatesQueue_.getReader(),
      staticRoutesQueue_.getReader(),
      routeUpdatesQueue_);

  //
  // create FIB
  //
  fib_ = std::make_unique<Fib>(
      config_, routeUpdatesQueue_.getReader(), fibRouteUpdatesQueue_);

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
  kvRequestQueue_.close();
  interfaceUpdatesQueue_.close();
  neighborUpdatesQueue_.close();
  nlSock_->closeQueue();
  prefixUpdatesQueue_.close();
  kvStoreUpdatesQueue_.close();
  initializationEventQueue_.close();
  addrEventQueue_.close();
  staticRoutesQueue_.close();
  fibRouteUpdatesQueue_.close();
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
void
OpenrWrapper<Serializer>::updateInterfaceDb(const InterfaceDatabase& ifDb) {
  interfaceUpdatesQueue_.push(ifDb);
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
    const thrift::PrefixType type,
    const std::vector<thrift::PrefixEntry>& prefixes) {
  PrefixEvent event(PrefixEventType::ADD_PREFIXES, type, prefixes);
  prefixUpdatesQueue_.push(std::move(event));
  return true;
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::withdrawPrefixEntries(
    const thrift::PrefixType type,
    const std::vector<thrift::PrefixEntry>& prefixes) {
  PrefixEvent event(PrefixEventType::WITHDRAW_PREFIXES, type, prefixes);
  prefixUpdatesQueue_.push(std::move(event));
  return true;
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
