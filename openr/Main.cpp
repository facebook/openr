/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <filesystem>
namespace fs = std::filesystem;
#include <syslog.h>
#include <fstream>
#include <stdexcept>

#include <folly/container/F14Set.h>
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <sodium.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/BuildInfo.h>
#include <openr/common/Flags.h>
#include <openr/common/MainUtil.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/decision/Decision.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/dispatcher/Dispatcher.h>
#include <openr/fib/Fib.h>
#include <openr/kvstore/KvStore.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/Monitor.h>
#include <openr/neighbor-monitor/NeighborMonitor.h>
#include <openr/nl/NetlinkProtocolSocket.h>
#include <openr/platform/NetlinkFibHandler.h>
#include <openr/plugin/Plugin.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/spark/IoProvider.h>
#include <openr/spark/Spark.h>
#include <openr/watchdog/Watchdog.h>

#ifndef NO_FOLLY_EXCEPTION_TRACER
#include <folly/debugging/exception_tracer/ExceptionTracer.h>
#endif

using namespace openr;

using openr::messaging::ReplicateQueue;

// jemalloc parameters - http://jemalloc.net/jemalloc.3.html
// background_thread:false - Disable background jemalloc background thread.
// prof:true - Memory profiling enabled.
// prof_active:false - Deactivate memory profiling by default.
//                     On-the-fly activation is available.
// prof_prefix - Filename prefix for profile dumps.
const char* malloc_conf =
    "background_thread:false,prof:true,prof_active:false,prof_prefix:/tmp/openr_heap";

int
main(int argc, char** argv) {
  /*
   * [Version]
   *
   * Set version string for `openr --version`
   */
  std::stringstream ss;
  BuildInfo::log(ss);
  gflags::SetVersionString(ss.str());

  /*
   * [Logging]
   *
   * Initialize logging and other params.
   *
   * ATTN: log all messages upto INFO level.
   *
   * LOG_CONS => Log to console on error
   * LOG_PID => Log PID along with each message
   * LOG_NODELAY => Connect immediately
   */
  const folly::Init init(&argc, &argv);
  folly::setThreadName("openr");
  setlogmask(LOG_UPTO(LOG_INFO));
  openlog("openr", LOG_CONS | LOG_PID | LOG_NDELAY | LOG_PERROR, LOG_LOCAL4);
  SYSLOG(INFO) << "Starting OpenR daemon: ppid = " << getpid();
  logInitializationEvent("Main", thrift::InitializationEvent::INITIALIZING);

  // Export and log build information
  BuildInfo::exportBuildInfo();
  XLOG(INFO) << ss.str();

  // init sodium security library
  if (::sodium_init() == -1) {
    XLOG(ERR) << "Failed initializing sodium";
    return 1;
  }

  // start config module
  std::shared_ptr<Config> config;
  try {
    XLOG(INFO) << "Reading config from " << FLAGS_config;
    config = std::make_shared<Config>(FLAGS_config);
  } catch (const thrift::ConfigError&) {
#ifndef NO_FOLLY_EXCEPTION_TRACER
    // collect stack strace then fail the process
    for (auto& exInfo : folly::exception_tracer::getCurrentExceptions()) {
      XLOG(ERR) << exInfo;
    }
#endif
    XLOG(FATAL) << "Failed to start OpenR. Invalid configuration.";
  }
  SYSLOG(INFO) << config->getRunningConfig();

  /*
   * [Signal Handler]
   *
   * Register the signals to handle before anything else. This guarantees that
   * any threads created below will inherit the signal mask.
   */
  folly::EventBase signalHandlerEvb;
  EventBaseStopSignalHandler handler(&signalHandlerEvb);

  // Starting signalHandler eventbase to receive system signal
  std::thread signalHandlerEvbThread([&]() noexcept {
    XLOG(INFO) << "Starting openr signal handler evb...";
    folly::setThreadName("openr-signal");
    signalHandlerEvb.loopForever();
    XLOG(INFO) << "Signal handler evb stopped.";
  });
  signalHandlerEvb.waitUntilRunning();

  /*
   * [Fib Service Waiting]
   */
  if (not config->isNetlinkFibHandlerEnabled() and not config->isDryrun()) {
    if (not waitForFibService(
            signalHandlerEvb, *config->getConfig().fib_port())) {
      signalHandlerEvbThread.join();

      // Close syslog connection (this is optional)
      SYSLOG(INFO) << "Stopping OpenR daemon: ppid = " << getpid();
      closelog();
      return 0;
    }
  }
  logInitializationEvent("Main", thrift::InitializationEvent::AGENT_CONFIGURED);

  /*
   * [Queue] messaging bus for inter-thread communication
   *
   * ATTN:
   *  - Producer will be marked and passed in with messaging::ReplicateQueue
   *  - Consumer will be marked and passed in with messaging::RQueue
   */
  // DispatcherQueue is the KvStore -> subscribers queue with filtering enabled
  auto kvStorePublicationsDispatcherQueue = std::make_unique<DispatcherQueue>();

  // Decision -> Fib
  ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  auto fibDecisionRouteUpdatesQueueReader =
      routeUpdatesQueue.getReader("fibDecision");

  // PrefixManager -> Decision
  ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  auto decisionStaticRouteUpdatesQueueReader =
      staticRouteUpdatesQueue.getReader("decision");

  // Fib -> PrefixManager
  ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
  auto fibRoutesUpdateQueueReader =
      fibRouteUpdatesQueue.getReader("routeUpdates");

  // PrefixManager -> Spark
  ReplicateQueue<thrift::InitializationEvent>
      prefixMgrInitializationEventsQueue;
  auto sparkInitializationEventsQueueReader =
      prefixMgrInitializationEventsQueue.getReader("spark");

  // LinkMonitor -> Spark
  ReplicateQueue<InterfaceDatabase> interfaceUpdatesQueue;
  auto sparkInterfaceUpdatesQueueReader =
      interfaceUpdatesQueue.getReader("spark");

  // Spark -> LinkMonitor
  ReplicateQueue<NeighborInitEvent> neighborUpdatesQueue;
  auto linkMonitorNeighborUpdatesQueueReader =
      neighborUpdatesQueue.getReader("linkMonitor");

  // Anyone -> PrefixManager
  ReplicateQueue<PrefixEvent> prefixUpdatesQueue;
  auto prefixMgrPrefixUpdatesQueueReader =
      prefixUpdatesQueue.getReader("prefixManager");

  // KvStore -> Subscribers
  ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue;

  // LinkMonitor -> KvStore/Decision
  ReplicateQueue<PeerEvent> peerUpdatesQueue;
  auto kvStorePeerUpdatesQueueReader = peerUpdatesQueue.getReader("kvstore");
  auto decisionPeerUpdatesQueueReader = peerUpdatesQueue.getReader("decision");

  // PrefixManager/LinkMonitor -> KvStore
  ReplicateQueue<KeyValueRequest> kvRequestQueue;
  auto kvStoreRequestQueueReader = kvRequestQueue.getReader("kvStore");

  // Netlink -> LinkMonitor
  ReplicateQueue<fbnl::NetlinkEvent> netlinkEventsQueue;
  auto linkMonitorNetlinkEventsQueueReader =
      netlinkEventsQueue.getReader("linkMonitor");

  // NeighborMonitor -> Spark
  ReplicateQueue<AddressEvent> addrEventsQueue;
  auto sparkAddrEventsQueueReader = addrEventsQueue.getReader("spark");

  // Anyone -> Monitor
  ReplicateQueue<LogSample> logSampleQueue;

  // structures to organize our modules
  std::unique_ptr<apache::thrift::ThriftServer> netlinkFibServer;
  std::unique_ptr<std::thread> netlinkFibServerThread;
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

  // Create Netlink Protocol object in a new thread
  // NOTE: Start EventBase only after NetlinkProtocolSocket has been constructed
  auto nlOpenrEvb = std::make_unique<OpenrEventBase>();
  auto nlSock = std::make_unique<openr::fbnl::NetlinkProtocolSocket>(
      nlOpenrEvb->getEvb(), netlinkEventsQueue);
  startEventBase(
      allThreads, orderedEvbs, watchdog, "netlink", std::move(nlOpenrEvb));
  watchdog->addQueue(netlinkEventsQueue, "netlinkEventsQueue");

  // Start NetlinkFibHandler if specified
  if (config->isNetlinkFibHandlerEnabled()) {
    netlinkFibServer = std::make_unique<apache::thrift::ThriftServer>();
    netlinkFibServer->setThreadManagerType(
        apache::thrift::ThriftServer::ThreadManagerType::PRIORITY_QUEUE);
    netlinkFibServer->setNumCPUWorkerThreads(2 /* num of threads */);
    netlinkFibServer->setCPUWorkerThreadName("ThriftCpuPool");

    netlinkFibServer->setIdleTimeout(Constants::kPlatformThriftIdleTimeout);
    netlinkFibServer->setNumIOWorkerThreads(1);
    netlinkFibServer->setCpp2WorkerThreadName("FibTWorker");
    netlinkFibServer->setPort(*config->getConfig().fib_port());

    netlinkFibServerThread =
        std::make_unique<std::thread>([&netlinkFibServer, &nlSock]() {
          folly::setThreadName("openr-fibService");
          auto fibHandler = std::make_shared<NetlinkFibHandler>(nlSock.get());
          netlinkFibServer->setInterface(std::move(fibHandler));

          XLOG(INFO) << "Starting NetlinkFib server...";
          netlinkFibServer->serve();
          XLOG(INFO) << "NetlinkFib server got stopped.";
        });
  }

  // Start Config-store
  auto configStore = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "config_store",
      std::make_unique<PersistentStore>(config));

  // Start Monitor
  auto monitor = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "monitor",
      std::make_unique<openr::Monitor>(
          config,
          Constants::kEventLogCategory.toString(),
          logSampleQueue.getReader("monitor")));
  watchdog->addQueue(logSampleQueue, "logSampleQueue");

  // Start KvStore
  auto kvStore = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "kvstore",
      std::make_unique<KvStore<thrift::OpenrCtrlCppAsyncClient>>(
          kvStoreUpdatesQueue,
          std::move(kvStorePeerUpdatesQueueReader),
          std::move(kvStoreRequestQueueReader),
          logSampleQueue,
          config->getAreaIds(),
          config->toThriftKvStoreConfig()));
  watchdog->addQueue(kvStoreUpdatesQueue, "kvStoreUpdatesQueue");

  // Start Dispatcher
  auto dispatcher = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "dispatcher",
      std::make_unique<Dispatcher>(
          kvStoreUpdatesQueue.getReader("dispatcher"),
          *kvStorePublicationsDispatcherQueue));

  // make Decision/Prefix Manager subscribers of Dispatcher
  auto decisionKvStoreUpdatesQueueReader = dispatcher->getReader(
      {Constants::kAdjDbMarker.toString(),
       Constants::kPrefixDbMarker.toString()});

  auto prefixMgrKvStoreUpdatesReader =
      dispatcher->getReader({Constants::kPrefixDbMarker.toString()});

  auto linkMonitorKvStoreUpdatesReader =
      dispatcher->getReader({"link-monitor"});

  watchdog->addQueue(
      *kvStorePublicationsDispatcherQueue, "kvStorePublicationsQueue");

  // PrefixManager will wait for Fib programming and publishing updates
  auto prefixManager = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "prefix_manager",
      std::make_unique<PrefixManager>(
          staticRouteUpdatesQueue,
          kvRequestQueue,
          prefixMgrInitializationEventsQueue,
          prefixMgrKvStoreUpdatesReader,
          prefixMgrPrefixUpdatesQueueReader,
          fibRoutesUpdateQueueReader,
          config));
  watchdog->addQueue(kvRequestQueue, "kvRequestQueue");
  watchdog->addQueue(staticRouteUpdatesQueue, "staticRouteUpdatesQueue");
  watchdog->addQueue(prefixUpdatesQueue, "prefixUpdatesQueue");

  // Start NeighborMonitor
  if (config->isNeighborMonitorEnabled()) {
    startEventBase(
        allThreads,
        orderedEvbs,
        watchdog,
        "neighbor-monitor",
        std::make_unique<NeighborMonitor>(addrEventsQueue));
    watchdog->addQueue(addrEventsQueue, "addrEventsQueue");
  }

  // Start Spark
  auto spark = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "spark",
      std::make_unique<Spark>(
          std::move(sparkInterfaceUpdatesQueueReader),
          std::move(sparkInitializationEventsQueueReader),
          std::move(sparkAddrEventsQueueReader),
          neighborUpdatesQueue,
          std::make_shared<IoProvider>(),
          config));
  watchdog->addQueue(neighborUpdatesQueue, "neighborUpdatesQueue");

  // Start LinkMonitor
  auto linkMonitor = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "link_monitor",
      std::make_unique<LinkMonitor>(
          config,
          nlSock.get(),
          configStore,
          interfaceUpdatesQueue,
          prefixUpdatesQueue,
          peerUpdatesQueue,
          logSampleQueue,
          kvRequestQueue,
          std::move(linkMonitorKvStoreUpdatesReader),
          std::move(linkMonitorNeighborUpdatesQueueReader),
          std::move(linkMonitorNetlinkEventsQueueReader)));
  watchdog->addQueue(interfaceUpdatesQueue, "interfaceUpdatesQueue");
  watchdog->addQueue(peerUpdatesQueue, "peerUpdatesQueue");

  // Setup the SSL policy
  std::shared_ptr<wangle::SSLContextConfig> sslContext;
  // Acceptable SSL peer names
  folly::F14FastSet<std::string> acceptableNamesSet; // empty set by default

  if (config->isSecureThriftServerEnabled()) {
    sslContext = std::make_shared<wangle::SSLContextConfig>();
    sslContext->setCertificate(
        config->getSSLCertPath(), config->getSSLKeyPath(), "");
    sslContext->clientCAFiles =
        std::vector<std::string>{config->getSSLCaPath()};
    sslContext->sessionContext = Constants::kOpenrCtrlSessionContext.toString();
    sslContext->setNextProtocols(
        **apache::thrift::ThriftServer::defaultNextProtocols());
    sslContext->clientVerification = config->getSSLContextVerifyType();
    sslContext->eccCurveName = config->getSSLEccCurve();

    // Get the acceptable peer name set
    std::vector<std::string> acceptableNames;
    folly::split(',', config->getSSLAcceptablePeers(), acceptableNames, true);
    acceptableNamesSet.insert(acceptableNames.begin(), acceptableNames.end());
  }

  // Create vip service module
  if (config->isVipServiceEnabled()) {
    auto vipRouteEvb = std::make_unique<OpenrEventBase>();
    auto vipPluginArgs = VipPluginArgs{
        vipRouteEvb->getEvb(), prefixUpdatesQueue, config, sslContext};
    startEventBase(
        allThreads,
        orderedEvbs,
        watchdog,
        "vipRouteManager",
        std::move(vipRouteEvb));
    vipPluginStart(vipPluginArgs);
  }

  // Wait for the above three modules to start and run before running
  // SPF in Decision module.  This is to make sure the Decision module
  // receives itself as one of the nodes before running the spf.

  // Start Decision
  auto decision = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "decision",
      std::make_unique<Decision>(
          config,
          std::move(decisionPeerUpdatesQueueReader),
          std::move(decisionKvStoreUpdatesQueueReader),
          std::move(decisionStaticRouteUpdatesQueueReader),
          routeUpdatesQueue));
  watchdog->addQueue(routeUpdatesQueue, "routeUpdatesQueue");

  // Start Fib
  auto fib = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "fib",
      std::make_unique<Fib>(
          config,
          std::move(fibDecisionRouteUpdatesQueueReader),
          fibRouteUpdatesQueue));
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
      config,
      dispatcher);
  startEventBase(
      allThreads, orderedEvbs, watchdog, "ctrl_evb", std::move(ctrlOpenrEvb));

  // Start the thrift server
  auto server = setUpThriftServer(config, ctrlHandler, sslContext);
  std::thread serverThread = std::thread([&]() {
    XLOG(INFO) << "Starting ThriftCtrlServer thread ...";
    folly::setThreadName("openr-ThriftCtrlServer");
    server->serve();
    XLOG(INFO) << "ThriftCtrlServer thread got stopped.";
  });

  waitTillStart(server);

  // Wait for main eventbase to stop
  signalHandlerEvbThread.join();

  // Close all queues for inter-module communications
  routeUpdatesQueue.close();
  interfaceUpdatesQueue.close();
  peerUpdatesQueue.close();
  kvRequestQueue.close();
  neighborUpdatesQueue.close();
  prefixUpdatesQueue.close();
  prefixMgrInitializationEventsQueue.close();
  kvStoreUpdatesQueue.close();
  staticRouteUpdatesQueue.close();
  fibRouteUpdatesQueue.close();
  netlinkEventsQueue.close();
  logSampleQueue.close();
  addrEventsQueue.close();
  kvStorePublicationsDispatcherQueue->close();

  // Stop & destroy thrift server. Will reduce ref-count on ctrlHandler
  server->stop();
  serverThread.join();
  server.reset();
  // Destroy ctrlHandler
  CHECK(ctrlHandler.unique()) << "Unexpected ownership of ctrlHandler pointer";
  ctrlHandler.reset();

  // Stop all threads (in reverse order of their creation)
  for (auto riter = orderedEvbs.rbegin(); orderedEvbs.rend() != riter;
       ++riter) {
    (*riter)->stop();
    (*riter)->waitUntilStopped();
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

  nlSock.reset();

  // Wait for all threads
  for (auto& t : allThreads) {
    t.join();
  }

  // We're about to delete VipRouteManager object. The vipRouteManager
  // EventBase has already stopped and the event thread has also joined.
  // However, when an EventBase is stopped, there could still be queued
  // functions. During EventBase destruction, these oustanding functions will
  // be executed in main thread. These outstanding functions access
  // VipRouteManager object state, that is deleted in vipPluginDestroy().
  // Thus, we explicitly destruct the EventBase before the VIP route manager
  // object is deleted.
  orderedEvbs.clear();
  if (config->isVipServiceEnabled()) {
    vipPluginDestroy();
  }

  // Close syslog connection (this is optional)
  SYSLOG(INFO) << "[Exit] Stopping OpenR daemon: ppid = " << getpid();
  closelog();

  return 0;
}
