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
#include <openr/common/MplsUtil.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/config/GflagConfig.h>
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

using namespace folly::gen;

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
    if (not FLAGS_config.empty()) {
      LOG(INFO) << "Reading config from " << FLAGS_config;
      config = std::make_shared<Config>(FLAGS_config);
    } else {
      LOG(INFO) << "Constructing config from GFLAG value.";
      config = GflagConfig::createConfigFromGflag();
    }
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

  // Reference to spark config
  const auto& sparkConf = config->getSparkConfig();

  //
  // Hold time for synchronizing adjacencies in KvStore. We expect all the
  // adjacencies to be fully established within hold time after Open/R starts
  //
  const std::chrono::seconds initialAdjHoldTime{
      *config->getConfig().adj_hold_time_s_ref()};

  //
  // Hold time for synchronizing prefixes in KvStore. We expect all the
  // prefixes to be recovered (Redistribute, Plugin etc.) within this time
  // window.
  // NOTE: Based on signals from sources that advertises the routes we can
  // synchronize prefixes earlier. This time provides worst case bound.
  //
  const std::chrono::seconds initialPrefixHoldTime{
      *config->getConfig().prefix_hold_time_s_ref()};

  // Set up the zmq context for this process.
  fbzmq::Context context;

  // Set main thread name
  folly::setThreadName("openr");

  // Queue for inter-module communication
  ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  ReplicateQueue<KvStoreSyncEvent> kvStoreSyncEventsQueue;
  ReplicateQueue<InterfaceDatabase> interfaceUpdatesQueue;
  ReplicateQueue<NeighborEvent> neighborUpdatesQueue;
  ReplicateQueue<PrefixEvent> prefixUpdatesQueue;
  ReplicateQueue<thrift::Publication> kvStoreUpdatesQueue;
  ReplicateQueue<PeerEvent> peerUpdatesQueue;
  ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
  ReplicateQueue<fbnl::NetlinkEvent> netlinkEventsQueue;
  ReplicateQueue<LogSample> logSampleQueue;

  // Create the readers in the first place to make sure they can receive every
  // messages from the writer(s)
  auto decisionStaticRouteUpdatesQueueReader =
      staticRouteUpdatesQueue.getReader();
  auto fibStaticRouteUpdatesQueueReader = staticRouteUpdatesQueue.getReader();

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

  std::shared_ptr<ThreadManager> thriftThreadMgr{nullptr};
  std::unique_ptr<openr::fbnl::NetlinkProtocolSocket> nlSock{nullptr};
  std::unique_ptr<apache::thrift::ThriftServer> netlinkFibServer{nullptr};
  std::unique_ptr<std::thread> netlinkFibServerThread{nullptr};

  // Create Netlink Protocol object in a new thread
  auto nlEvb = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "netlink",
      std::make_unique<OpenrEventBase>());

  nlSock = std::make_unique<openr::fbnl::NetlinkProtocolSocket>(
      nlEvb->getEvb(), netlinkEventsQueue);

  // Start NetlinkFibHandler if specified
  if (config->isNetlinkFibHandlerEnabled()) {
    // Create ThreadManager for thrift services
    thriftThreadMgr = ThreadManager::newPriorityQueueThreadManager(
        2 /* num of threads */, false /* task stats */);
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
          logSampleQueue.getReader()));

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
          peerUpdatesQueue.getReader(),
          logSampleQueue,
          KvStoreGlobalCmdUrl{fmt::format(
              "tcp://{}:{}",
              *config->getConfig().listen_addr_ref(),
              FLAGS_kvstore_rep_port)},
          config));

  // If FIB-ACK feature is enabled, Fib publishes routes to PrefixManager
  // after programming has completed; Otherwise, Decision publishes routes to
  // PrefixManager.
  auto routeUpdatesQueueReader =
      (config->getConfig().get_enable_fib_ack()
           ? fibRouteUpdatesQueue.getReader()
           : routeUpdatesQueue.getReader());
  auto prefixManager = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "prefix_manager",
      std::make_unique<PrefixManager>(
          staticRouteUpdatesQueue,
          prefixUpdatesQueue.getReader(),
          std::move(routeUpdatesQueueReader),
          config,
          kvStore,
          initialPrefixHoldTime));

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
            Constants::kPrefixAllocatorSyncInterval));
  }

  // Create Spark instance for neighbor discovery
  auto spark = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "spark",
      std::make_unique<Spark>(
          interfaceUpdatesQueue.getReader(),
          neighborUpdatesQueue,
          KvStoreCmdPort{static_cast<uint16_t>(FLAGS_kvstore_rep_port)},
          OpenrCtrlThriftPort{static_cast<uint16_t>(FLAGS_openr_ctrl_port)},
          std::make_shared<IoProvider>(),
          config));

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
          neighborUpdatesQueue.getReader(),
          kvStoreSyncEventsQueue.getReader(),
          netlinkEventsQueue.getReader(),
          FLAGS_override_drain_state,
          initialAdjHoldTime));

  // Setup the SSL policy
  std::shared_ptr<wangle::SSLContextConfig> sslContext;
  // Acceptable SSL peer names
  std::unordered_set<std::string> acceptableNamesSet; // empty set by default

  // OpenrCtrl thrift server
  auto thriftCtrlServer = std::make_unique<apache::thrift::ThriftServer>();
  // Set the port and interface for OpenrCtrl thrift server
  thriftCtrlServer->setPort(*config->getConfig().openr_ctrl_port_ref());

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

    // Start OpenrCtrl thrift server
    setupThriftServerTls(
        *thriftCtrlServer,
        config->getSSLThriftPolicy(),
        config->getSSLSeedPath(),
        sslContext);
  }

  auto pluginArgs = PluginArgs{
      prefixUpdatesQueue,
      staticRouteUpdatesQueue,
      routeUpdatesQueue.getReader(),
      config,
      sslContext};

  // Create bgp speaker module
  if (config->isBgpPeeringEnabled()) {
    pluginStart(pluginArgs);
  }

  // Create vip service module
  if (config->isVipServiceEnabled()) {
    vipPluginStart(pluginArgs);
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
          FLAGS_enable_bgp_route_programming,
          kvStoreUpdatesQueue.getReader(),
          std::move(decisionStaticRouteUpdatesQueueReader),
          routeUpdatesQueue));

  // Define and start Fib Module
  auto fib = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "fib",
      std::make_unique<Fib>(
          config,
          *config->getConfig().fib_port_ref(),
          std::chrono::seconds(3 * *sparkConf.keepalive_time_s_ref()),
          routeUpdatesQueue.getReader(),
          std::move(fibStaticRouteUpdatesQueueReader),
          fibRouteUpdatesQueue,
          logSampleQueue));

  // Create Open/R control handler
  auto ctrlEvb = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "ctrl_evb",
      std::make_unique<OpenrEventBase>());

  auto ctrlHandler = std::make_shared<openr::OpenrCtrlHandler>(
      config->getNodeName(),
      acceptableNamesSet,
      ctrlEvb,
      decision,
      fib,
      kvStore,
      linkMonitor,
      monitor,
      configStore,
      prefixManager,
      spark,
      config);

  CHECK(ctrlHandler);
  thriftCtrlServer->setInterface(ctrlHandler);
  thriftCtrlServer->setNumIOWorkerThreads(1);
  // Intentionally kept this as (1). If you're changing to higher number please
  // address thread safety for private member variables in OpenrCtrlHandler
  thriftCtrlServer->setNumCPUWorkerThreads(1);
  // Enable TOS reflection on the server socket
  thriftCtrlServer->setTosReflect(true);

  // serve
  std::thread thriftCtrlServerThread([&thriftCtrlServer]() noexcept {
    LOG(INFO) << "Starting ThriftCtrlServer thread ...";
    folly::setThreadName("openr-ThriftCtrlServer");
    thriftCtrlServer->serve();
    LOG(INFO) << "ThriftCtrlServer thread got stopped.";
  });
  // Wait until thrift server starts
  while (true) {
    auto evb = thriftCtrlServer->getServeEventBase();
    if (evb != nullptr and evb->isRunning()) {
      break;
    }
    std::this_thread::yield();
  }

  // Wait for main eventbase to stop
  mainEvbThread.join();

  // Close all queues for inter-module communications
  routeUpdatesQueue.close();
  interfaceUpdatesQueue.close();
  peerUpdatesQueue.close();
  neighborUpdatesQueue.close();
  kvStoreSyncEventsQueue.close();
  prefixUpdatesQueue.close();
  kvStoreUpdatesQueue.close();
  staticRouteUpdatesQueue.close();
  fibRouteUpdatesQueue.close();
  netlinkEventsQueue.close();
  logSampleQueue.close();

  // Stop & destroy thrift server. Will reduce ref-count on ctrlHandler
  thriftCtrlServer->stop();
  thriftCtrlServerThread.join();
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
