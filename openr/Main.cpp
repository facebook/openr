/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <syslog.h>
#include <fstream>
#include <stdexcept>

#include <fbzmq/async/StopEventLoopSignalHandler.h>
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
#include <openr/common/ThriftUtil.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/config/GflagConfig.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/kvstore/KvStore.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/platform/NetlinkFibHandler.h>
#include <openr/platform/NetlinkSystemHandler.h>
#include <openr/platform/PlatformPublisher.h>
#include <openr/plugin/Plugin.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/spark/IoProvider.h>
#include <openr/spark/Spark.h>
#include <openr/watchdog/Watchdog.h>

using namespace fbzmq;
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

// Disable background jemalloc background thread => new jemalloc-5 feature
const char* malloc_conf = "background_thread:false";

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
waitForFibService(const fbzmq::ZmqEventLoop& evl, int port) {
  auto waitForFibStart = std::chrono::steady_clock::now();

  auto fibStatus = facebook::fb303::cpp2::fb303_status::DEAD;
  folly::EventBase evb;
  std::shared_ptr<folly::AsyncSocket> socket;
  std::unique_ptr<openr::thrift::FibServiceAsyncClient> client;
  while (evl.isRunning() &&
         facebook::fb303::cpp2::fb303_status::ALIVE != fibStatus) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    LOG(INFO) << "Waiting for FibService to come up...";
    openr::Fib::createFibClient(evb, socket, client, port);
    try {
      fibStatus = client->sync_getStatus();
    } catch (const std::exception& e) {
    }
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

  // Start a thread
  allThreads.emplace_back(std::thread([evb = evb.get(), name]() noexcept {
    LOG(INFO) << "Starting " << name << " thread ...";
    folly::setThreadName(name);
    evb->run();
    LOG(INFO) << name << " thread got stopped.";
  }));
  evb->waitUntilRunning();

  // Add to watchdog
  if (watchdog) {
    watchdog->addEvb(evb.get(), name);
  }

  // Emplace evb into ordered list of evbs. So that we can destroy
  // them in revserse order of their creation.
  orderedEvbs.emplace_back(std::move(evb));

  return t;
}

int
main(int argc, char** argv) {
  // Register the signals to handle before anything else. This guarantees that
  // any threads created below will inherit the signal mask
  ZmqEventLoop mainEventLoop;
  StopEventLoopSignalHandler handler(&mainEventLoop);
  handler.registerSignalHandler(SIGINT);
  handler.registerSignalHandler(SIGQUIT);
  handler.registerSignalHandler(SIGTERM);

  // Set version string to show when `openr --version` is invoked
  std::stringstream ss;
  BuildInfo::log(ss);
  gflags::SetVersionString(ss.str());

  // Initialize syslog
  // We log all messages upto INFO level.
  // LOG_CONS => Log to console on error
  // LOG_PID => Log PID along with each message
  // LOG_NODELAY => Connect immediately
  setlogmask(LOG_UPTO(LOG_INFO));
  openlog("openr", LOG_CONS | LOG_PID | LOG_NDELAY | LOG_PERROR, LOG_LOCAL4);
  SYSLOG(INFO) << "Starting OpenR daemon.";

  LOG(INFO) << "With args: ";
  for (int i = 0; i < argc; ++i) {
    LOG(INFO) << argv[i];
  }

  // Initialize all params
  folly::init(&argc, &argv);

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
  if (not FLAGS_config.empty()) {
    LOG(INFO) << "Reading config from " << FLAGS_config;
    config = std::make_shared<Config>(FLAGS_config);
  } else {
    LOG(INFO) << "Constructing config from GFLAG value.";
    config = GflagConfig::createConfigFromGflag();
  }
  LOG(INFO) << config->getRunningConfig();

  // Sanity checks on Segment Routing labels
  const int32_t maxLabel = Constants::kMaxSrLabel;
  CHECK(Constants::kSrGlobalRange.first > 0);
  CHECK(Constants::kSrGlobalRange.second < maxLabel);
  CHECK(Constants::kSrLocalRange.first > 0);
  CHECK(Constants::kSrLocalRange.second < maxLabel);
  CHECK(Constants::kSrGlobalRange.first < Constants::kSrGlobalRange.second);
  CHECK(Constants::kSrLocalRange.first < Constants::kSrLocalRange.second);

  // Local and Global range must be exclusive of each other
  CHECK(
      (Constants::kSrGlobalRange.second < Constants::kSrLocalRange.first) ||
      (Constants::kSrGlobalRange.first > Constants::kSrLocalRange.second))
      << "Overlapping global/local segment routing label space.";

  // Prepare IP-TOS value from flag and do sanity checks
  std::optional<int> maybeIpTos{0};
  if (FLAGS_ip_tos != 0) {
    CHECK_LE(0, FLAGS_ip_tos) << "ip_tos must be greater than 0";
    CHECK_GE(256, FLAGS_ip_tos) << "ip_tos must be less than 256";
    maybeIpTos = FLAGS_ip_tos;
  }

  // Hold time for advertising Prefix/Adj keys into KvStore
  const auto& sparkConf = config->getSparkConfig();
  const std::chrono::seconds initialDumpTime{2 * sparkConf.keepalive_time_s};

  // Set up the zmq context for this process.
  Context context;

  // Set main thread name
  folly::setThreadName("openr");

  // Queue for inter-module communication
  ReplicateQueue<openr::thrift::RouteDatabaseDelta> routeUpdatesQueue;
  ReplicateQueue<openr::thrift::InterfaceDatabase> interfaceUpdatesQueue;
  ReplicateQueue<openr::thrift::SparkNeighborEvent> neighborUpdatesQueue;
  ReplicateQueue<openr::thrift::PrefixUpdateRequest> prefixUpdateRequestQueue;
  ReplicateQueue<openr::thrift::Publication> kvStoreUpdatesQueue;
  ReplicateQueue<openr::thrift::PeerUpdateRequest> peerUpdatesQueue;
  ReplicateQueue<openr::thrift::RouteDatabaseDelta> staticRoutesUpdateQueue;

  // structures to organize our modules
  std::vector<std::thread> allThreads;
  std::vector<std::unique_ptr<OpenrEventBase>> orderedEvbs;
  Watchdog* watchdog{nullptr};

  // Watchdog thread to monitor thread aliveness
  if (FLAGS_enable_watchdog) {
    watchdog = startEventBase(
        allThreads,
        orderedEvbs,
        nullptr /* watchdog won't monitor itself */,
        "Watchdog",
        std::make_unique<Watchdog>(config));
  }

  // Create ThreadManager for thrift services
  std::shared_ptr<ThreadManager> thriftThreadMgr{nullptr};

  std::unique_ptr<OpenrEventBase> nlEvb{nullptr};
  std::unique_ptr<openr::fbnl::NetlinkProtocolSocket> nlSock{nullptr};
  std::unique_ptr<apache::thrift::ThriftServer> netlinkFibServer{nullptr};
  std::unique_ptr<apache::thrift::ThriftServer> netlinkSystemServer{nullptr};
  std::unique_ptr<std::thread> netlinkFibServerThread{nullptr};
  std::unique_ptr<std::thread> netlinkSystemServerThread{nullptr};
  std::unique_ptr<PlatformPublisher> eventPublisher{nullptr};

  if (config->isNetlinkFibHandlerEnabled() or
      config->isNetlinkSystemHandlerEnabled()) {
    thriftThreadMgr = ThreadManager::newPriorityQueueThreadManager(
        2 /* num of threads */, false /* task stats */);
    thriftThreadMgr->setNamePrefix("ThriftCpuPool");
    thriftThreadMgr->start();

    // Create Netlink Protocol object in a new thread
    nlEvb = std::make_unique<OpenrEventBase>();
    nlSock =
        std::make_unique<openr::fbnl::NetlinkProtocolSocket>(nlEvb->getEvb());
    allThreads.emplace_back([&]() {
      LOG(INFO) << "Starting NetlinkEvb thread ...";
      folly::setThreadName("NetlinkEvb");
      nlEvb->getEvb()->loopForever();
      LOG(INFO) << "NetlinkEvb thread got stopped.";
    });
    nlEvb->getEvb()->waitUntilRunning();

    // Add netlink eventbase to watchdog
    if (watchdog) {
      watchdog->addEvb(nlEvb.get(), "NetlinkEvb");
    }

    // Create event publisher to handle event subscription
    eventPublisher = std::make_unique<PlatformPublisher>(
        context, PlatformPublisherUrl{FLAGS_platform_pub_url}, nlSock.get());

    // ATTN: intentionally set evl capacity to be 1e5 instead of default 1e2
    if (config->isNetlinkFibHandlerEnabled()) {
      CHECK(thriftThreadMgr);

      // Start NetlinkFibHandler if specified
      netlinkFibServer = std::make_unique<apache::thrift::ThriftServer>();
      netlinkFibServer->setIdleTimeout(Constants::kPlatformThriftIdleTimeout);
      netlinkFibServer->setThreadManager(thriftThreadMgr);
      netlinkFibServer->setNumIOWorkerThreads(1);
      netlinkFibServer->setCpp2WorkerThreadName("FibTWorker");
      netlinkFibServer->setPort(config->getConfig().fib_port);

      netlinkFibServerThread =
          std::make_unique<std::thread>([&netlinkFibServer, &nlSock]() {
            folly::setThreadName("FibService");
            auto fibHandler = std::make_shared<NetlinkFibHandler>(nlSock.get());
            netlinkFibServer->setInterface(std::move(fibHandler));

            LOG(INFO) << "Starting NetlinkFib server...";
            netlinkFibServer->serve();
            LOG(INFO) << "NetlinkFib server got stopped.";
          });
    }

    // Start NetlinkSystemHandler if specified
    if (config->isNetlinkSystemHandlerEnabled()) {
      CHECK(thriftThreadMgr);
      netlinkSystemServer = std::make_unique<apache::thrift::ThriftServer>();
      netlinkSystemServer->setIdleTimeout(
          Constants::kPlatformThriftIdleTimeout);
      netlinkSystemServer->setThreadManager(thriftThreadMgr);
      netlinkSystemServer->setNumIOWorkerThreads(1);
      netlinkSystemServer->setCpp2WorkerThreadName("SystemTWorker");
      netlinkSystemServer->setPort(FLAGS_system_agent_port);

      netlinkSystemServerThread =
          std::make_unique<std::thread>([&netlinkSystemServer, &nlSock]() {
            folly::setThreadName("SystemService");
            auto systemHandler =
                std::make_unique<NetlinkSystemHandler>(nlSock.get());
            netlinkSystemServer->setInterface(std::move(systemHandler));

            LOG(INFO) << "Starting NetlinkSystem server...";
            netlinkSystemServer->serve();
            LOG(INFO) << "NetlinkSystem server got stopped.";
          });
    }
  }

  const MonitorSubmitUrl monitorSubmitUrl{
      folly::sformat("tcp://[::1]:{}", FLAGS_monitor_rep_port)};

  // Starting main event-loop
  std::thread mainEventLoopThread([&]() noexcept {
    LOG(INFO) << "Starting main event loop...";
    folly::setThreadName("MainLoop");
    mainEventLoop.run();
    LOG(INFO) << "Main event loop got stopped";
  });
  mainEventLoop.waitUntilRunning();

  if (FLAGS_enable_fib_service_waiting) {
    waitForFibService(mainEventLoop, config->getConfig().fib_port);
  }

  // Starting openrCtrlEvb for thrift handler
  OpenrEventBase ctrlEvb;
  std::thread ctrlEvbThread([&]() noexcept {
    LOG(INFO) << "Starting openrCtrl eventbase...";
    folly::setThreadName("openrCtrl");
    ctrlEvb.run();
    LOG(INFO) << "OpenrCtrl eventbase stopped...";
  });
  ctrlEvb.waitUntilRunning();

  // Start config-store URL
  auto configStore = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "ConfigStore",
      std::make_unique<PersistentStore>(
          config->getNodeName(), FLAGS_config_store_filepath, context));

  // Start monitor Module
  // for each log message it receives, we want to add the openr domain
  fbzmq::LogSample sampleToMerge;
  sampleToMerge.addString("domain", config->getConfig().domain);
  ZmqMonitor monitor(
      MonitorSubmitUrl{folly::sformat(
          "tcp://{}:{}",
          config->getConfig().listen_addr,
          FLAGS_monitor_rep_port)},
      MonitorPubUrl{folly::sformat(
          "tcp://{}:{}",
          config->getConfig().listen_addr,
          FLAGS_monitor_pub_port)},
      context,
      sampleToMerge);
  std::thread monitorThread([&monitor]() noexcept {
    LOG(INFO) << "Starting ZmqMonitor thread...";
    folly::setThreadName("ZmqMonitor");
    monitor.run();
    LOG(INFO) << "ZmqMonitor thread got stopped.";
  });
  monitor.waitUntilRunning();
  allThreads.emplace_back(std::move(monitorThread));

  // Start KVStore
  auto kvStore = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "KvStore",
      std::make_unique<KvStore>(
          context,
          kvStoreUpdatesQueue,
          peerUpdatesQueue.getReader(),
          KvStoreGlobalCmdUrl{folly::sformat(
              "tcp://{}:{}",
              config->getConfig().listen_addr,
              FLAGS_kvstore_rep_port)},
          monitorSubmitUrl,
          config,
          maybeIpTos,
          std::unordered_map<std::string, openr::thrift::PeerSpec>{},
          FLAGS_kvstore_zmq_hwm,
          FLAGS_enable_kvstore_thrift));

  auto prefixManager = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "PrefixManager",
      std::make_unique<PrefixManager>(
          prefixUpdateRequestQueue.getReader(),
          config,
          configStore,
          kvStore,
          FLAGS_enable_perf_measurement,
          initialDumpTime,
          FLAGS_per_prefix_keys));

  // Prefix Allocator to automatically allocate prefixes for nodes
  if (config->isPrefixAllocationEnabled()) {
    startEventBase(
        allThreads,
        orderedEvbs,
        watchdog,
        "PrefixAllocator",
        std::make_unique<PrefixAllocator>(
            config,
            kvStore,
            prefixUpdateRequestQueue,
            monitorSubmitUrl,
            configStore,
            context,
            FLAGS_system_agent_port,
            Constants::kPrefixAllocatorSyncInterval));
  }

  // Create Spark instance for neighbor discovery
  startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "Spark",
      std::make_unique<Spark>(
          config->getConfig().domain, // My domain
          config->getNodeName(), // myNodeName
          static_cast<uint16_t>(sparkConf.neighbor_discovery_port),
          std::chrono::seconds(sparkConf.hello_time_s),
          std::chrono::milliseconds(sparkConf.fastinit_hello_time_ms),
          std::chrono::milliseconds(
              sparkConf.fastinit_hello_time_ms), // spark2_handshake_time_ms
          std::chrono::seconds(
              sparkConf.keepalive_time_s), // spark2_heartbeat_time_s
          std::chrono::seconds(
              sparkConf.keepalive_time_s), // spark2_negotiate_hold_time_s
          std::chrono::seconds(
              sparkConf.hold_time_s), // spark2_heartbeat_hold_time_s
          std::chrono::seconds(
              sparkConf.graceful_restart_time_s), // spark2_gr_hold_time_s
          maybeIpTos,
          config->isV4Enabled(),
          interfaceUpdatesQueue.getReader(),
          neighborUpdatesQueue,
          KvStoreCmdPort{static_cast<uint16_t>(FLAGS_kvstore_rep_port)},
          OpenrCtrlThriftPort{static_cast<uint16_t>(FLAGS_openr_ctrl_port)},
          std::make_pair(
              Constants::kOpenrVersion, Constants::kOpenrSupportedVersion),
          std::make_shared<IoProvider>(),
          config->isFloodOptimizationEnabled()));

  // Static list of prefixes to announce into the network as long as OpenR is
  // running.
  std::vector<openr::thrift::IpPrefix> networks;
  try {
    std::vector<std::string> prefixes;
    folly::split(",", FLAGS_prefixes, prefixes, true /* ignore empty */);
    for (auto const& prefix : prefixes) {
      // Perform some sanity checks before announcing the list of prefixes
      auto network = folly::IPAddress::createNetwork(prefix);
      if (network.first.isLoopback()) {
        LOG(FATAL) << "Default loopback addresses can't be announced "
                   << prefix;
      }
      if (network.first.isLinkLocal()) {
        LOG(FATAL) << "Link local addresses can't be announced " << prefix;
      }
      if (network.first.isMulticast()) {
        LOG(FATAL) << "Multicast addresses can't be annouced " << prefix;
      }
      networks.emplace_back(toIpPrefix(network));
    }
  } catch (std::exception const& err) {
    LOG(ERROR) << "Invalid Prefix string specified. Expeted comma separated "
               << "list of IP/CIDR format, got '" << FLAGS_prefixes << "'";
    return -1;
  }

  // Create link monitor instance.
  auto linkMonitor = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "LinkMonitor",
      std::make_unique<LinkMonitor>(
          context,
          config,
          FLAGS_system_agent_port,
          kvStore,
          networks,
          FLAGS_enable_perf_measurement,
          interfaceUpdatesQueue,
          peerUpdatesQueue,
          neighborUpdatesQueue.getReader(),
          monitorSubmitUrl,
          configStore,
          FLAGS_assume_drained,
          prefixUpdateRequestQueue,
          PlatformPublisherUrl{FLAGS_platform_pub_url},
          initialDumpTime));

  // Wait for the above two threads to start and run before running
  // SPF in Decision module.  This is to make sure the Decision module
  // receives itself as one of the nodes before running the spf.

  // Start Decision Module
  auto decision = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "Decision",
      std::make_unique<Decision>(
          config,
          FLAGS_enable_lfa,
          not FLAGS_enable_bgp_route_programming,
          std::chrono::milliseconds(FLAGS_decision_debounce_min_ms),
          std::chrono::milliseconds(FLAGS_decision_debounce_max_ms),
          kvStoreUpdatesQueue.getReader(),
          staticRoutesUpdateQueue.getReader(),
          routeUpdatesQueue,
          context));

  // Define and start Fib Module
  auto fib = startEventBase(
      allThreads,
      orderedEvbs,
      watchdog,
      "Fib",
      std::make_unique<Fib>(
          config,
          config->getConfig().fib_port,
          std::chrono::seconds(3 * sparkConf.keepalive_time_s),
          routeUpdatesQueue.getReader(),
          interfaceUpdatesQueue.getReader(),
          monitorSubmitUrl,
          kvStore,
          context));

  // Start OpenrCtrl thrift server
  apache::thrift::ThriftServer thriftCtrlServer;

  // setup the SSL policy
  std::shared_ptr<wangle::SSLContextConfig> sslContext;
  if (FLAGS_enable_secure_thrift_server) {
    CHECK(fileExists(FLAGS_x509_ca_path));
    CHECK(fileExists(FLAGS_x509_cert_path));
    auto& keyPath = FLAGS_x509_key_path;
    if (!keyPath.empty()) {
      CHECK(fileExists(keyPath));
    } else {
      keyPath = FLAGS_x509_cert_path;
    }
    sslContext = std::make_shared<wangle::SSLContextConfig>();
    sslContext->setCertificate(FLAGS_x509_cert_path, keyPath, "");
    sslContext->clientCAFile = FLAGS_x509_ca_path;
    sslContext->sessionContext = Constants::kOpenrCtrlSessionContext.toString();
    sslContext->setNextProtocols(Constants::getNextProtocolsForThriftServers());
    // TODO Change to VERIFY_REQ_CLIENT_CERT after we have everyone using certs
    sslContext->clientVerification =
        folly::SSLContext::SSLVerifyPeerEnum::VERIFY;
    sslContext->eccCurveName = FLAGS_tls_ecc_curve_name;
    setupThriftServerTls(
        thriftCtrlServer,
        // TODO Change to REQUIRED after we have everyone using certs
        apache::thrift::SSLPolicy::PERMITTED,
        FLAGS_tls_ticket_seed_path,
        sslContext);
  }
  // set the port and interface
  thriftCtrlServer.setPort(config->getConfig().openr_ctrl_port);

  std::unordered_set<std::string> acceptableNamesSet; // empty set by default
  if (FLAGS_enable_secure_thrift_server) {
    std::vector<std::string> acceptableNames;
    folly::split(",", FLAGS_tls_acceptable_peers, acceptableNames, true);
    acceptableNamesSet.insert(acceptableNames.begin(), acceptableNames.end());
  }

  std::shared_ptr<openr::OpenrCtrlHandler> ctrlHandler{nullptr};
  ctrlEvb.getEvb()->runInEventBaseThreadAndWait([&]() {
    ctrlHandler = std::make_shared<openr::OpenrCtrlHandler>(
        config->getNodeName(),
        acceptableNamesSet,
        &ctrlEvb,
        decision,
        fib,
        kvStore,
        linkMonitor,
        configStore,
        prefixManager,
        config,
        monitorSubmitUrl,
        context);
  });

  CHECK(ctrlHandler);
  thriftCtrlServer.setInterface(ctrlHandler);
  thriftCtrlServer.setNumIOWorkerThreads(1);
  // Intentionally kept this as (1). If you're changing to higher number please
  // address thread safety for private member variables in OpenrCtrlHandler
  thriftCtrlServer.setNumCPUWorkerThreads(1);
  // Enable TOS reflection on the server socket
  thriftCtrlServer.setTosReflect(true);

  // serve
  allThreads.emplace_back(std::thread([&thriftCtrlServer]() noexcept {
    LOG(INFO) << "Starting thriftCtrlServer thread ...";
    folly::setThreadName("thriftCtrlServer");
    thriftCtrlServer.serve();
    LOG(INFO) << "thriftCtrlServer thread got stopped.";
  }));

  // Call external module for platform specific implementations
  if (config->isBgpPeeringEnabled()) {
    pluginStart(PluginArgs{prefixUpdateRequestQueue,
                           staticRoutesUpdateQueue,
                           routeUpdatesQueue.getReader(),
                           config,
                           sslContext});
  }

  // Wait for main-event loop to return
  mainEventLoopThread.join();

  // Stop all threads (in reverse order of their creation)
  routeUpdatesQueue.close();
  interfaceUpdatesQueue.close();
  peerUpdatesQueue.close();
  neighborUpdatesQueue.close();
  prefixUpdateRequestQueue.close();
  kvStoreUpdatesQueue.close();
  staticRoutesUpdateQueue.close();

  thriftCtrlServer.stop();
  ctrlHandler.reset();
  ctrlEvb.stop();
  ctrlEvb.waitUntilStopped();
  ctrlEvbThread.join();

  for (auto riter = orderedEvbs.rbegin(); orderedEvbs.rend() != riter;
       ++riter) {
    (*riter)->stop();
    (*riter)->waitUntilStopped();
  }
  monitor.stop();
  monitor.waitUntilStopped();

  if (nlEvb) {
    nlEvb->getEvb()->terminateLoopSoon();
  }

  if (netlinkFibServer) {
    CHECK(netlinkFibServerThread);
    netlinkFibServer->stop();
    netlinkFibServerThread->join();
    netlinkFibServerThread.reset();
    netlinkFibServer.reset();
  }
  if (netlinkSystemServer) {
    CHECK(netlinkSystemServerThread);
    netlinkSystemServer->stop();
    netlinkSystemServerThread->join();
    netlinkSystemServerThread.reset();
    netlinkSystemServer.reset();
  }

  if (thriftThreadMgr) {
    thriftThreadMgr->stop();
  }

  if (nlSock) {
    nlSock.reset();
  }

  if (eventPublisher) {
    eventPublisher.reset();
  }

  // Wait for all threads to finish
  for (auto& t : allThreads) {
    t.join();
  }

  // Call external module for platform specific implementations
  if (config->isBgpPeeringEnabled()) {
    pluginStop();
  }

  // Close syslog connection (this is optional)
  SYSLOG(INFO) << "Stopping OpenR daemon.";
  closelog();

  return 0;
}
