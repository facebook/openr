/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#if __has_include("filesystem")
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif
#include <syslog.h>
#include <fstream>
#include <stdexcept>

#include <fbzmq/zmq/Zmq.h>
#include <folly/FileUtil.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/gen/Base.h>
#include <folly/gen/String.h>
#include <folly/init/Init.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <re2/re2.h>
#include <re2/set.h>
#include <sodium.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/allocators/PrefixAllocator.h>
#include <openr/common/BuildInfo.h>
#include <openr/common/Constants.h>
#include <openr/common/Flags.h>
#include <openr/common/OpenrThriftCtrlServer.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/decision/Decision.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/fib/Fib.h>
#include <openr/kvstore/KvStore.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/Monitor.h>
#include <openr/nl/NetlinkProtocolSocket.h>
#include <openr/platform/NetlinkFibHandler.h>
#include <openr/plugin/Plugin.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/spark/IoProvider.h>
#include <openr/spark/Spark.h>
#include <openr/watchdog/Watchdog.h>

#ifndef NO_FOLLY_EXCEPTION_TRACER
#include <folly/experimental/exception_tracer/ExceptionTracer.h>
#endif

using namespace openr;

using apache::thrift::concurrency::ThreadManager;
using openr::messaging::ReplicateQueue;

namespace {
//
// Local constants
//

const std::string inet6Path = "/proc/net/if_inet6";
} // namespace

// jemalloc parameters - http://jemalloc.net/jemalloc.3.html
// background_thread:false - Disable background jemalloc background thread.
// prof:true - Memory profiling enabled.
// prof_active:false - Deactivate memory profiling by default.
//                     On-the-fly activation is available.
// prof_prefix - Filename prefix for profile dumps.
const char* malloc_conf =
    "background_thread:false,prof:true,prof_active:false,prof_prefix:/tmp/openr_heap";

void
checkIsIpv6Enabled() {
  // check if file path exists
  std::ifstream ifs(inet6Path);
  if (!ifs) {
    LOG(ERROR) << "File path: " << inet6Path << " doesn't exist!";
    return;
  }

  // check file size for if_inet6_path.
  // zero-size file means IPv6 is NOT enabled globally.
  if (ifs.peek() == std::ifstream::traits_type::eof()) {
    LOG(FATAL) << "IPv6 is NOT enabled. Pls check system config!";
  }
}

void
waitForFibService(const folly::EventBase& mainEvb, int port) {
  // TODO: handle case when openr received SIGTERM when waiting for fibService
  auto waitForFibStart = std::chrono::steady_clock::now();
  auto switchState = thrift::SwitchRunState::UNINITIALIZED;
  folly::EventBase evb;
  folly::AsyncSocket* socket;
  std::unique_ptr<openr::thrift::FibServiceAsyncClient> client;

  // Block until the Fib client is ready to accept route updates (aka, of
  // CONFIGURED state).
  while (mainEvb.isRunning() and
         thrift::SwitchRunState::CONFIGURED != switchState) {
    openr::Fib::createFibClient(evb, socket, client, port);
    try {
      switchState = client->sync_getSwitchRunState();
    } catch (const std::exception& e) {
    }
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(1));
    LOG(INFO) << "Waiting for FibService to come up and be ready to accept "
              << "route updates...";
  }

  auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - waitForFibStart)
                    .count();
  LOG(INFO) << "FibService up. Waited for " << waitMs << " ms.";
}

/**
 * Start an EventBase in a thread, maintain order of thread creation and
 * returns raw pointer of Derived class.
 */
template <typename T>
T*
startEventBase(
    std::vector<std::thread>& allThreads,
    std::vector<std::unique_ptr<OpenrEventBase>>& orderedEvbs,
    Watchdog* watchdog,
    const std::string& name,
    std::unique_ptr<T> evbT) {
  CHECK(evbT);
  auto t = evbT.get();
  auto evb = std::unique_ptr<OpenrEventBase>(
      reinterpret_cast<OpenrEventBase*>(evbT.release()));
  evb->setEvbName(name);

  // Start a thread
  allThreads.emplace_back(std::thread([evb = evb.get(), name]() noexcept {
    LOG(INFO) << "Starting " << name << " thread ...";
    folly::setThreadName(fmt::format("openr-{}", name));
    evb->run();
    LOG(INFO) << name << " thread got stopped.";
  }));
  evb->waitUntilRunning();

  // Add to watchdog
  if (watchdog) {
    watchdog->addEvb(evb.get());
  }

  // Emplace evb into ordered list of evbs. So that we can destroy
  // them in revserse order of their creation.
  orderedEvbs.emplace_back(std::move(evb));

  return t;
}

int
main(int argc, char** argv) {
  // Set version string to show when `openr --version` is invoked
  std::stringstream ss;
  BuildInfo::log(ss);
  gflags::SetVersionString(ss.str());

  // Initialize all params
  folly::init(&argc, &argv);

  // Register the signals to handle before anything else. This guarantees that
  // any threads created below will inherit the signal mask
  folly::EventBase mainEvb;
  EventBaseStopSignalHandler handler(&mainEvb);

  // Initialize syslog
  // We log all messages upto INFO level.
  // LOG_CONS => Log to console on error
  // LOG_PID => Log PID along with each message
  // LOG_NODELAY => Connect immediately
  setlogmask(LOG_UPTO(LOG_INFO));
  openlog("openr", LOG_CONS | LOG_PID | LOG_NDELAY | LOG_PERROR, LOG_LOCAL4);
  SYSLOG(INFO) << "Starting OpenR daemon: ppid = " << getpid();
  logInitializationEvent("Main", thrift::InitializationEvent::INITIALIZING);

  // Export and log build information
  BuildInfo::exportBuildInfo();
  LOG(INFO) << ss.str();

  // init sodium security library
  if (::sodium_init() == -1) {
    LOG(ERROR) << "Failed initializing sodium";
    return 1;
  }

  // Sanity check for IPv6 global environment
  checkIsIpv6Enabled();

  // start config module
  std::shared_ptr<Config> config;
  try {
    LOG(INFO) << "Reading config from " << FLAGS_config;
    config = std::make_shared<Config>(FLAGS_config);
  } catch (const thrift::ConfigError&) {
#ifndef NO_FOLLY_EXCEPTION_TRACER
    // collect stack strace then fail the process
    for (auto& exInfo : folly::exception_tracer::getCurrentExceptions()) {
      LOG(ERROR) << exInfo;
    }
#endif
    LOG(FATAL) << "Failed to start OpenR. Invalid configuration.";
  }

  SYSLOG(INFO) << config->getRunningConfig();

  // Set up the zmq context for this process.
  fbzmq::Context context;

  // Set main thread name
  folly::setThreadName("openr");

  // Queue for inter-module communication
  ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  ReplicateQueue<KvStoreSyncEvent> kvStoreSyncEventsQueue;
  ReplicateQueue<InterfaceEvent> interfaceUpdatesQueue;
  ReplicateQueue<NeighborEvents> neighborUpdatesQueue;
  ReplicateQueue<PrefixEvent> prefixUpdatesQueue;
  ReplicateQueue<Publication> kvStoreUpdatesQueue;
  ReplicateQueue<PeerEvent> peerUpdatesQueue;
  ReplicateQueue<KeyValueRequest> kvRequestQueue;
  ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  ReplicateQueue<DecisionRouteUpdate> prefixMgrRouteUpdatesQueue;
  ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
  ReplicateQueue<fbnl::NetlinkEvent> netlinkEventsQueue;
  ReplicateQueue<LogSample> logSampleQueue;

  // Create the readers in the first place to make sure they can receive every
  // messages from the writer(s)
  auto decisionStaticRouteUpdatesQueueReader =
      staticRouteUpdatesQueue.getReader("decision");
  auto fibStaticRouteUpdatesQueueReader =
      staticRouteUpdatesQueue.getReader("fib");
  auto fibDecisionRouteUpdatesQueueReader =
      routeUpdatesQueue.getReader("fibDecision");
  auto linkMonitorNeighborUpdatesQueueReader =
      neighborUpdatesQueue.getReader("linkMonitor");
  auto linkMonitorKvStoreSyncEventsQueueReader =
      kvStoreSyncEventsQueue.getReader("linkMonitor");
  auto linkMonitorNetlinkEventsQueueReader =
      netlinkEventsQueue.getReader("linkMonitor");
  auto decisionKvStoreUpdatesQueueReader =
      kvStoreUpdatesQueue.getReader("decision");
  auto PrefixManagerKvStoreUpdatesReader =
      kvStoreUpdatesQueue.getReader("prefixManager");
  auto pluginRouteReader =
      prefixMgrRouteUpdatesQueue.getReader("pluginRouteUpdates");

  // structures to organize our modules
  std::vector<std::thread> allThreads;
  std::vector<std::unique_ptr<OpenrEventBase>> orderedEvbs;
  Watchdog* watchdog{nullptr};

  // Watchdog thread to monitor thread aliveness
  if (config->isWatchdogEnabled()) {
    watchdog = startEventBase(
        allThreads,
        orderedEvbs,
        nullptr /* watchdog won't monitor itself */,
        "watchdog",
        std::make_unique<Watchdog>(config));
  }

  // Starting main event-loop
  std::thread mainEvbThread([&]() noexcept {
    LOG(INFO) << "Starting openr main event-base...";
    folly::setThreadName("openr-main");
    mainEvb.loopForever();
    LOG(INFO) << "Main event-base stopped...";
  });
  mainEvb.waitUntilRunning();

  if (config->isFibServiceWaitingEnabled() and
      (not config->isNetlinkFibHandlerEnabled())) {
    waitForFibService(mainEvb, *config->getConfig().fib_port_ref());
  }
  logInitializationEvent("Main", thrift::InitializationEvent::AGENT_CONFIGURED);

  std::shared_ptr<ThreadManager> thriftThreadMgr{nullptr};
  std::unique_ptr<apache::thrift::ThriftServer> netlinkFibServer{nullptr};
  std::unique_ptr<std::thread> netlinkFibServerThread{nullptr};

  // Create Netlink Protocol object in a new thread
  // NOTE: Start EventBase only after NetlinkProtocolSocket has been constructed
  auto nlOpenrEvb = std::make_unique<OpenrEventBase>();
  auto nlSock = std::make_unique<openr::fbnl::NetlinkProtocolSocket>(
      nlOpenrEvb->getEvb(), netlinkEventsQueue);
  startEventBase(
      allThreads, orderedEvbs, watchdog, "netlink", std::move(nlOpenrEvb));

  // Start NetlinkFibHandler if specified
  if (config->isNetlinkFibHandlerEnabled()) {
    // Create ThreadManager for thrift services
    thriftThreadMgr =
        ThreadManager::newPriorityQueueThreadManager(2 /* num of threads */);
    thriftThreadMgr->setNamePrefix("ThriftCpuPool");
    thriftThreadMgr->start();
    CHECK(thriftThreadMgr);

    netlinkFibServer = std::make_unique<apache::thrift::ThriftServer>();
    netlinkFibServer->setIdleTimeout(Constants::kPlatformThriftIdleTimeout);
    netlinkFibServer->setThreadManager(thriftThreadMgr);
    netlinkFibServer->setNumIOWorkerThreads(1);
    netlinkFibServer->setCpp2WorkerThreadName("FibTWorker");
    netlinkFibServer->setPort(*config->getConfig().fib_port_ref());

    netlinkFibServerThread =
        std::make_unique<std::thread>([&netlinkFibServer, &nlSock]() {
          folly::setThreadName("openr-fibService");
          auto fibHandler = std::make_shared<NetlinkFibHandler>(nlSock.get());
          netlinkFibServer->setInterface(std::move(fibHandler));

          LOG(INFO) << "Starting NetlinkFib server...";
          netlinkFibServer->serve();
          LOG(INFO) << "NetlinkFib server got stopped.";
        });
    watchdog->addQueue(netlinkEventsQueue, "netlinkEventsQueue");
  }

  // Start config-store URL
  auto configStore = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "config_store",
      std::make_unique<PersistentStore>(config));

  // Start monitor Module
  auto monitor = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "monitor",
      std::make_unique<openr::Monitor>(
          config,
          Constants::kEventLogCategory.toString(),
          logSampleQueue.getReader("monitor")));

  // Start KVStore
  auto kvStore = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "kvstore",
      std::make_unique<KvStore>(
          context,
          kvStoreUpdatesQueue,
          kvStoreSyncEventsQueue,
          peerUpdatesQueue.getReader("kvStore"),
          kvRequestQueue.getReader("kvStore"),
          logSampleQueue,
          KvStoreGlobalCmdUrl{fmt::format(
              "tcp://{}:{}",
              *config->getConfig().listen_addr_ref(),
              Constants::kKvStoreRepPort)},
          config));
  watchdog->addQueue(kvStoreSyncEventsQueue, "kvStoreSyncEventsQueue");
  watchdog->addQueue(kvStoreUpdatesQueue, "kvStoreUpdatesQueue");
  watchdog->addQueue(logSampleQueue, "logSampleQueue");

  // If FIB-ACK feature is enabled, Fib publishes routes to PrefixManager
  // after programming has completed; Otherwise, Decision publishes routes to
  // PrefixManager.
  auto routeUpdatesQueueReader =
      (config->getConfig().get_enable_fib_ack()
           ? fibRouteUpdatesQueue.getReader("routeUpdates")
           : routeUpdatesQueue.getReader("routeUpdates"));
  auto prefixManager = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "prefix_manager",
      std::make_unique<PrefixManager>(
          staticRouteUpdatesQueue,
          kvRequestQueue,
          prefixMgrRouteUpdatesQueue,
          PrefixManagerKvStoreUpdatesReader,
          prefixUpdatesQueue.getReader("prefixManager"),
          std::move(routeUpdatesQueueReader),
          config,
          kvStore));
  watchdog->addQueue(kvRequestQueue, "kvRequestQueue");
  watchdog->addQueue(staticRouteUpdatesQueue, "staticRouteUpdatesQueue");

  // Prefix Allocator to automatically allocate prefixes for nodes
  if (config->isPrefixAllocationEnabled()) {
    startEventBase(
        allThreads,
        orderedEvbs,
        watchdog,
        "prefix_allocator",
        std::make_unique<PrefixAllocator>(
            AreaId{*config->getAreaIds().begin()},
            config,
            nlSock.get(),
            kvStore,
            configStore,
            prefixUpdatesQueue,
            logSampleQueue,
            kvRequestQueue,
            Constants::kPrefixAllocatorSyncInterval));
  }
  watchdog->addQueue(prefixUpdatesQueue, "prefixUpdatesQueue");
  watchdog->addQueue(netlinkEventsQueue, "netlinkEventsQueue");

  // Create Spark instance for neighbor discovery
  auto spark = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "spark",
      std::make_unique<Spark>(
          interfaceUpdatesQueue.getReader("spark"),
          neighborUpdatesQueue,
          std::make_shared<IoProvider>(),
          config));
  watchdog->addQueue(neighborUpdatesQueue, "neighborUpdatesQueue");

  // Create link monitor instance.
  auto linkMonitor = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "link_monitor",
      std::make_unique<LinkMonitor>(
          config,
          nlSock.get(),
          kvStore,
          configStore,
          interfaceUpdatesQueue,
          prefixUpdatesQueue,
          peerUpdatesQueue,
          logSampleQueue,
          kvRequestQueue,
          std::move(linkMonitorNeighborUpdatesQueueReader),
          std::move(linkMonitorKvStoreSyncEventsQueueReader),
          std::move(linkMonitorNetlinkEventsQueueReader),
          FLAGS_override_drain_state));
  watchdog->addQueue(interfaceUpdatesQueue, "interfaceUpdatesQueue");
  watchdog->addQueue(peerUpdatesQueue, "peerUpdatesQueue");

  // Setup the SSL policy
  std::shared_ptr<wangle::SSLContextConfig> sslContext;
  // Acceptable SSL peer names
  std::unordered_set<std::string> acceptableNamesSet; // empty set by default

  if (config->isSecureThriftServerEnabled()) {
    sslContext = std::make_shared<wangle::SSLContextConfig>();
    sslContext->setCertificate(
        config->getSSLCertPath(), config->getSSLKeyPath(), "");
    sslContext->clientCAFile = config->getSSLCaPath();
    sslContext->sessionContext = Constants::kOpenrCtrlSessionContext.toString();
    sslContext->setNextProtocols(
        **apache::thrift::ThriftServer::defaultNextProtocols());
    sslContext->clientVerification = config->getSSLContextVerifyType();
    sslContext->eccCurveName = config->getSSLEccCurve();

    // Get the acceptable peer name set
    std::vector<std::string> acceptableNames;
    folly::split(",", config->getSSLAcceptablePeers(), acceptableNames, true);
    acceptableNamesSet.insert(acceptableNames.begin(), acceptableNames.end());
  }

  // Create bgp speaker module
  auto pluginArgs = PluginArgs{
      prefixUpdatesQueue,
      staticRouteUpdatesQueue,
      pluginRouteReader,
      config,
      sslContext};
  if (config->isBgpPeeringEnabled()) {
    pluginStart(pluginArgs);
  }

  // Create vip service module
  auto vipRouteEvb = std::make_unique<OpenrEventBase>();
  auto vipPluginArgs = VipPluginArgs{
      vipRouteEvb->getEvb(), prefixUpdatesQueue, config, sslContext};
  if (config->isVipServiceEnabled()) {
    vipPluginStart(vipPluginArgs);
    startEventBase(
        allThreads,
        orderedEvbs,
        watchdog,
        "vipRouteManager",
        std::move(vipRouteEvb));
  }

  // Wait for the above three modules to start and run before running
  // SPF in Decision module.  This is to make sure the Decision module
  // receives itself as one of the nodes before running the spf.

  // Start Decision Module
  auto decision = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "decision",
      std::make_unique<Decision>(
          config,
          std::move(decisionKvStoreUpdatesQueueReader),
          std::move(decisionStaticRouteUpdatesQueueReader),
          routeUpdatesQueue));
  watchdog->addQueue(routeUpdatesQueue, "routeUpdatesQueue");

  // Define and start Fib Module
  auto fib = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "fib",
      std::make_unique<Fib>(
          config,
          std::move(fibDecisionRouteUpdatesQueueReader),
          std::move(fibStaticRouteUpdatesQueueReader),
          fibRouteUpdatesQueue,
          logSampleQueue));
  watchdog->addQueue(fibRouteUpdatesQueue, "fibRouteUpdatesQueue");

  // Create Open/R control handler
  // NOTE: Start EventBase only after OpenrCtrlHandler has been constructed
  auto ctrlOpenrEvb = std::make_unique<OpenrEventBase>();
  auto ctrlHandler = std::make_shared<openr::OpenrCtrlHandler>(
      config->getNodeName(),
      acceptableNamesSet,
      ctrlOpenrEvb.get(),
      decision,
      fib,
      kvStore,
      linkMonitor,
      monitor,
      configStore,
      prefixManager,
      spark,
      config);
  startEventBase(
      allThreads, orderedEvbs, watchdog, "ctrl_evb", std::move(ctrlOpenrEvb));

  // Start the thrift server
  auto thriftCtrlServer =
      std::make_unique<OpenrThriftCtrlServer>(config, ctrlHandler, sslContext);
  thriftCtrlServer->start();

  // Wait for main eventbase to stop
  mainEvbThread.join();

  // Close all queues for inter-module communications
  routeUpdatesQueue.close();
  interfaceUpdatesQueue.close();
  peerUpdatesQueue.close();
  kvRequestQueue.close();
  neighborUpdatesQueue.close();
  kvStoreSyncEventsQueue.close();
  prefixUpdatesQueue.close();
  kvStoreUpdatesQueue.close();
  staticRouteUpdatesQueue.close();
  fibRouteUpdatesQueue.close();
  netlinkEventsQueue.close();
  prefixMgrRouteUpdatesQueue.close();
  logSampleQueue.close();

  // Stop & destroy thrift server. Will reduce ref-count on ctrlHandler
  thriftCtrlServer->stop();
  thriftCtrlServer.reset();
  // Destroy ctrlHandler
  CHECK(ctrlHandler.unique()) << "Unexpected ownership of ctrlHandler pointer";
  ctrlHandler.reset();

  // Stop all threads (in reverse order of their creation)
  for (auto riter = orderedEvbs.rbegin(); orderedEvbs.rend() != riter;
       ++riter) {
    LOG(INFO) << "Stopping " << (*riter)->getEvbName();
    (*riter)->stop();
    (*riter)->waitUntilStopped();
    LOG(INFO) << "Finally stopped " << (*riter)->getEvbName();
  }

  // stop bgp speaker
  if (config->isBgpPeeringEnabled()) {
    pluginStop();
  }

  if (config->isVipServiceEnabled()) {
    vipPluginStop();
  }

  if (netlinkFibServer) {
    CHECK(netlinkFibServerThread);
    netlinkFibServer->stop();
    netlinkFibServerThread->join();
    netlinkFibServerThread.reset();
    netlinkFibServer.reset();
  }

  if (thriftThreadMgr) {
    thriftThreadMgr->stop();
  }

  nlSock.reset();

  // Wait for all threads
  for (auto& t : allThreads) {
    t.join();
  }

  // Close syslog connection (this is optional)
  SYSLOG(INFO) << "Stopping OpenR daemon: ppid = " << getpid();
  closelog();

  return 0;
}
