/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <signal.h>
#ifdef ENABLE_SYSTEMD_NOTIFY
#include <systemd/sd-daemon.h> // @manual
#endif

#include <thread>

#include <fbzmq/async/ZmqEventLoop.h>
#include <folly/SocketAddress.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/Constants.h>
#include <openr/fbmeshd/802.11s/AuthsaeCallbackHelpers.h>
#include <openr/fbmeshd/802.11s/Nl80211Handler.h>
#include <openr/fbmeshd/802.11s/PeerSelector.h>
#include <openr/fbmeshd/MeshServiceHandler.h>
#include <openr/fbmeshd/SignalHandler.h>
#include <openr/fbmeshd/common/Constants.h>
#include <openr/fbmeshd/gateway-11s-root-route-programmer/Gateway11sRootRouteProgrammer.h>
#include <openr/fbmeshd/gateway-connectivity-monitor/GatewayConnectivityMonitor.h>
#include <openr/fbmeshd/gateway-connectivity-monitor/RouteDampener.h>
#include <openr/fbmeshd/mesh-spark/MeshSpark.h>
#include <openr/fbmeshd/openr-metric-manager/OpenRMetricManager.h>
#include <openr/fbmeshd/route-update-monitor/RouteUpdateMonitor.h>
#include <openr/fbmeshd/routing/Routing.h>
#include <openr/fbmeshd/separa/Separa.h>
#include <openr/watchdog/Watchdog.h>

using namespace openr::fbmeshd;

DEFINE_int32(fbmeshd_service_port, 30303, "fbmeshd thrift service port");

DEFINE_int32(
    kvstore_pub_port,
    openr::Constants::kKvStorePubPort,
    "The port where KvStore publishes updates");
DEFINE_int32(
    kvstore_cmd_port,
    openr::Constants::kKvStoreRepPort,
    "The port where KvStore listens for commands");
DEFINE_string(node_name, "node1", "The name of current node");

DEFINE_bool(
    enable_openr_metric_manager,
    true,
    "If set, manages metric information and sends it to OpenR");

DEFINE_int32(
    link_monitor_cmd_port,
    openr::Constants::kLinkMonitorCmdPort,
    "The port link monitor listens for commands on");

DEFINE_bool(
    enable_mesh_spark,
    false,
    "If set, enables OpenR neighbor discovery using 11s");

DEFINE_bool(
    userspace_mesh_peering,
    true,
    "If set, mesh peering management handshake will be done in userspace");

DEFINE_bool(
    event_based_peer_selector,
    false,
    "If set, PeerSelector will use event-based mode instead of polling mode");

DEFINE_bool(
    enable_separa,
    false,
    "If set, Separa algorithm will be enabled to manage mesh partitions. "
    "Implies enable_separa_broadcasts=true");
DEFINE_int32(
    separa_hello_port, 6667, "The port used to send separa hello packets");
DEFINE_int32(
    separa_broadcast_interval_s,
    1,
    "how often to send separa broadcasts in seconds");
DEFINE_int32(
    separa_domain_lockdown_period_s,
    60,
    "how long to lockdown domains in seconds");
DEFINE_double(
    separa_domain_change_threshold_factor,
    1,
    "threshold factor for doing a separa domain change");
DEFINE_bool(
    enable_separa_broadcasts,
    true,
    "If set, Separa broadcasts will be enabled");

DEFINE_int32(
    decision_rep_port,
    openr::Constants::kDecisionRepPort,
    "The port Decision replier listens on");

DEFINE_int32(
    prefix_manager_cmd_port,
    openr::Constants::kPrefixManagerCmdPort,
    "The port prefix manager receives commands on");

// Gateway Connectivity Monitor configs
DEFINE_string(
    gateway_connectivity_monitor_interface,
    "eth0",
    "The interface that the gateway connectivity monitor runs on");
DEFINE_string(
    gateway_connectivity_monitor_address,
    "8.8.4.4:443",
    "The address that the gateway connectivity monitor connects to check "
    "connectivity (host:port)");
DEFINE_uint32(
    gateway_connectivity_monitor_interval_s,
    1,
    "Interval in seconds to check for connectivity by the gateway "
    "connectivity monitor");
DEFINE_uint32(
    gateway_connectivity_monitor_socket_timeout_s,
    5,
    "How long to wait until timing out the socket when checking for "
    "connectivity by the gateway connectivity monitor");
DEFINE_uint32(
    gateway_connectivity_monitor_robustness,
    openr::fbmeshd::Constants::kDefaultRobustness,
    "The number of times to attempt to connect to the monitor address before "
    "declaring connectivity down");
DEFINE_uint32(
    gateway_connectivity_monitor_set_root_mode,
    0,
    "The value for root mode that should be set if we are a gate");

DEFINE_int32(
    monitor_rep_port,
    openr::Constants::kMonitorRepPort,
    "The port monitor replies on");

DEFINE_bool(
    enable_watchdog,
    true,
    "Enable watchdog thread to periodically check aliveness counters from each "
    "fbmeshd thread, if unhealthy thread is detected, force crash fbmeshd");
DEFINE_int32(watchdog_interval_s, 20, "Watchdog thread healthcheck interval");
DEFINE_int32(watchdog_threshold_s, 300, "Watchdog thread aliveness threshold");
DEFINE_int32(memory_limit_mb, 300, "Memory limit in MB");

DEFINE_uint32(
    route_dampener_penalty,
    openr::fbmeshd::Constants::kDefaultPenalty,
    "The route dampener penalty assigned to default route flaps.");

DEFINE_uint32(
    route_dampener_suppress_limit,
    openr::fbmeshd::Constants::kDefaultSuppressLimit,
    "The route dampener limit which when reached will suppress the advertisement"
    " of a default route.");

DEFINE_uint32(
    route_dampener_reuse_limit,
    openr::fbmeshd::Constants::kDefaultReuseLimit,
    "The route dampener limit which when passed on the way down will unsuppress"
    " a suppressed default route.");

DEFINE_uint32(
    route_dampener_halflife,
    openr::fbmeshd::Constants::kDefaultHalfLife.count(),
    "The route dampener halflife in seconds in which the history penalty value"
    " will be reduced by half.");

DEFINE_uint32(
    route_dampener_max_suppress_limit,
    openr::fbmeshd::Constants::kDefaultMaxSuppressLimit.count(),
    "The route dampener maximum time a default route can be suppressed before"
    " it will be automatically unsupressed.");

DEFINE_bool(
    enable_gateway_11s_root_route_programmer,
    false,
    "If set, enables creating routes to gate based on 11s root announcements");
DEFINE_double(
    gateway_11s_root_route_programmer_gateway_change_threshold_factor,
    2,
    "threshold factor for doing a gateway change");
DEFINE_uint32(
    gateway_11s_root_route_programmer_interval_s,
    1,
    "how often to sync routes with the fib");
DEFINE_bool(
    enable_routing, false, "If set, enables experimental routing module");
DEFINE_uint32(routing_ttl, 3, "TTL for routing elements");

DEFINE_bool(
    is_openr_enabled,
    true,
    "If set, considers openr to be enabled and enables functionality relating"
    " to openr");

namespace {
constexpr folly::StringPiece kHostName{"localhost"};

} // namespace

void
monitorEventLoopWithWatchdog(
    fbzmq::ZmqEventLoop* eventLoop,
    const std::string& eventLoopName,
    openr::Watchdog* watchdog) {
  if (watchdog != nullptr) {
    watchdog->addEvl(eventLoop, eventLoopName);
  }
}

std::unique_ptr<openr::Watchdog>
makeWatchdog() {
  return std::make_unique<openr::Watchdog>(
      FLAGS_node_name,
      std::chrono::seconds{FLAGS_watchdog_interval_s},
      std::chrono::seconds{FLAGS_watchdog_threshold_s},
      FLAGS_memory_limit_mb);
}

void
startWatchdog(openr::Watchdog* watchdog, std::vector<std::thread>& allThreads) {
  CHECK_NOTNULL(watchdog);

  // Spawn a watchdog thread
  allThreads.emplace_back(std::thread([watchdog]() noexcept {
    LOG(INFO) << "Starting Watchdog thread ...";
    folly::setThreadName("Watchdog");
    watchdog->run();
    LOG(INFO) << "Watchdog thread got stopped.";
  }));
  watchdog->waitUntilRunning();
}

std::unique_ptr<fbzmq::ZmqTimeout>
makeSystemdWatchdogNotifier(openr::Watchdog* watchdog) {
  CHECK_NOTNULL(watchdog);

  std::unique_ptr<fbzmq::ZmqTimeout> notifier{nullptr};

#ifdef ENABLE_SYSTEMD_NOTIFY
  notifier = fbzmq::ZmqTimeout::make(watchdog, []() noexcept {
    VLOG(2) << "Systemd watchdog notify";
    sd_notify(0, "WATCHDOG=1");
  });

  uint64_t watchdogEnv{0};
  int status{0};
  // Always expect the watchdog to be set if systemd is here.
  CHECK_GE((status = sd_watchdog_enabled(0, &watchdogEnv)), 0)
      << "Problem when fetching systemd watchdog";
  if (status == 0) {
    return nullptr;
  }

  std::chrono::microseconds usec{watchdogEnv / 2};
  auto watchdogNotifyInterval{
      std::chrono::duration_cast<std::chrono::milliseconds>(usec)};

  static constexpr bool isPeriodic{true};
  notifier->scheduleTimeout(watchdogNotifyInterval, isPeriodic);
  LOG(INFO) << folly::sformat(
      "Started timer to notify systemd every {} ms.",
      watchdogNotifyInterval.count());
#endif

  return notifier;
}

int
main(int argc, char* argv[]) {
  folly::init(&argc, &argv);

  // Set stdout to be line-buffered, to assist with integration testing that
  // depends on log output
  setvbuf(stdout, nullptr /*buffer*/, _IOLBF, 0);

  int rssiThreshold =
      std::max(Constants::kMinRssiThreshold, FLAGS_mesh_rssi_threshold);

  LOG(INFO) << "Starting fbmesh daemon...";

  std::vector<std::thread> allThreads{};

  std::unique_ptr<openr::Watchdog> watchdog{nullptr};
  std::unique_ptr<fbzmq::ZmqTimeout> systemWatchdogNotifier{nullptr};
  if (FLAGS_enable_watchdog) {
    watchdog = makeWatchdog();
    systemWatchdogNotifier = makeSystemdWatchdogNotifier(watchdog.get());
    startWatchdog(watchdog.get(), allThreads);
  }

  fbzmq::ZmqEventLoop evl;

  monitorEventLoopWithWatchdog(
      &evl, "fbmeshd_shared_event_loop", watchdog.get());
  AuthsaeCallbackHelpers::init(evl);
  Nl80211Handler nlHandler{evl, FLAGS_userspace_mesh_peering};
  SignalHandler signalHandler{evl, nlHandler};
  signalHandler.registerSignalHandler(SIGINT);
  signalHandler.registerSignalHandler(SIGTERM);
  signalHandler.registerSignalHandler(SIGABRT);

  auto returnValue = nlHandler.joinMeshes();
  if (returnValue != R_SUCCESS) {
    return returnValue;
  }

  // Set up the zmq context for this process.
  fbzmq::Context zmqContext;

  const openr::KvStoreLocalPubUrl kvStoreLocalPubUrl{
      folly::sformat("tcp://{}:{}", kHostName, FLAGS_kvstore_pub_port)};
  const openr::KvStoreLocalCmdUrl kvStoreLocalCmdUrl{
      folly::sformat("tcp://{}:{}", kHostName, FLAGS_kvstore_cmd_port)};
  const std::string linkMonitorGlobalCmdUrl{
      folly::sformat("tcp://{}:{}", kHostName, FLAGS_link_monitor_cmd_port)};
  const openr::PrefixManagerLocalCmdUrl prefixManagerLocalCmdUrl{
      folly::sformat("tcp://{}:{}", kHostName, FLAGS_prefix_manager_cmd_port)};
  const openr::DecisionCmdUrl decisionCmdUrl{
      folly::sformat("tcp://{}:{}", kHostName, FLAGS_decision_rep_port)};
  const openr::MonitorSubmitUrl monitorSubmitUrl{
      folly::sformat("tcp://{}:{}", kHostName, FLAGS_monitor_rep_port)};

  RouteUpdateMonitor routeMonitor{evl, nlHandler};

  std::unique_ptr<MeshSpark> meshSpark{nullptr};
  if (FLAGS_enable_mesh_spark && FLAGS_is_openr_enabled) {
    meshSpark = std::make_unique<MeshSpark>(
        evl,
        nlHandler,
        FLAGS_mesh_ifname,
        kvStoreLocalCmdUrl,
        kvStoreLocalPubUrl,
        FLAGS_enable_separa,
        zmqContext);
  }

  std::unique_ptr<OpenRMetricManager> openRMetricManager;
  if (FLAGS_enable_openr_metric_manager && FLAGS_is_openr_enabled) {
    openRMetricManager = std::make_unique<OpenRMetricManager>(
        evl,
        &nlHandler,
        FLAGS_mesh_ifname,
        linkMonitorGlobalCmdUrl,
        monitorSubmitUrl,
        zmqContext);
  }

  PeerSelector peerSelector{
      evl,
      nlHandler,
      rssiThreshold,
      !(FLAGS_event_based_peer_selector && FLAGS_userspace_mesh_peering)};

  std::unique_ptr<Separa> separa;
  if (meshSpark != nullptr && FLAGS_is_openr_enabled &&
      (FLAGS_enable_separa_broadcasts || FLAGS_enable_separa)) {
    separa = std::make_unique<Separa>(
        FLAGS_separa_hello_port,
        std::chrono::seconds{FLAGS_separa_broadcast_interval_s},
        std::chrono::seconds{FLAGS_separa_domain_lockdown_period_s},
        FLAGS_separa_domain_change_threshold_factor,
        !FLAGS_enable_separa,
        nlHandler,
        *meshSpark,
        prefixManagerLocalCmdUrl,
        decisionCmdUrl,
        kvStoreLocalCmdUrl,
        kvStoreLocalPubUrl,
        monitorSubmitUrl,
        zmqContext);

    static constexpr auto separaId{"Separa"};
    monitorEventLoopWithWatchdog(separa.get(), separaId, watchdog.get());

    allThreads.emplace_back(std::thread([&separa]() noexcept {
      LOG(INFO) << "Starting the Separa thread...";
      folly::setThreadName(separaId);
      separa->run();
      LOG(INFO) << "Separa thread got stopped.";
    }));
    separa->waitUntilRunning();
  }

  std::unique_ptr<Gateway11sRootRouteProgrammer> gateway11sRootRouteProgrammer;
  static constexpr auto gateway11sRootRouteProgrammerId{
      "Gateway11sRootRouteProgrammer"};
  if (FLAGS_enable_gateway_11s_root_route_programmer) {
    gateway11sRootRouteProgrammer = std::make_unique<
        Gateway11sRootRouteProgrammer>(
        nlHandler,
        std::chrono::seconds{
            FLAGS_gateway_11s_root_route_programmer_interval_s},
        FLAGS_gateway_11s_root_route_programmer_gateway_change_threshold_factor);
    allThreads.emplace_back(
        std::thread([&gateway11sRootRouteProgrammer]() noexcept {
          LOG(INFO) << "Starting the Gateway 11s root route programmer";
          folly::setThreadName(gateway11sRootRouteProgrammerId);
          gateway11sRootRouteProgrammer->run();
          LOG(INFO) << "Gateway 11s root route programmer thread stopped.";
        }));
  }

  std::unique_ptr<Routing> routing;
  static constexpr auto routingId{"Routing"};
  if (FLAGS_enable_routing) {
    routing = std::make_unique<Routing>(
        nlHandler, folly::SocketAddress{"::", 6668}, FLAGS_routing_ttl);
    allThreads.emplace_back(std::thread([&routing]() noexcept {
      LOG(INFO) << "Starting Routing";
      folly::setThreadName(routingId);
      routing->loopForever();
      LOG(INFO) << "Routing thread stopped.";
    }));
  }

  folly::SocketAddress gatewayConnectivityMonitorAddress;
  gatewayConnectivityMonitorAddress.setFromIpPort(
      FLAGS_gateway_connectivity_monitor_address);
  GatewayConnectivityMonitor gatewayConnectivityMonitor{
      nlHandler,
      prefixManagerLocalCmdUrl,
      FLAGS_gateway_connectivity_monitor_interface,
      gatewayConnectivityMonitorAddress,
      std::chrono::seconds{FLAGS_gateway_connectivity_monitor_interval_s},
      std::chrono::seconds{FLAGS_gateway_connectivity_monitor_socket_timeout_s},
      monitorSubmitUrl,
      zmqContext,
      FLAGS_route_dampener_penalty,
      FLAGS_route_dampener_suppress_limit,
      FLAGS_route_dampener_reuse_limit,
      std::chrono::seconds{FLAGS_route_dampener_halflife},
      std::chrono::seconds{FLAGS_route_dampener_max_suppress_limit},
      FLAGS_gateway_connectivity_monitor_robustness,
      static_cast<uint8_t>(FLAGS_gateway_connectivity_monitor_set_root_mode),
      gateway11sRootRouteProgrammer.get(),
      routing.get(),
      FLAGS_is_openr_enabled};

  static constexpr auto gwConnectivityMonitorId{"GatewayConnectivityMonitor"};
  monitorEventLoopWithWatchdog(
      &gatewayConnectivityMonitor, gwConnectivityMonitorId, watchdog.get());
  allThreads.emplace_back(std::thread([&gatewayConnectivityMonitor]() noexcept {
    LOG(INFO) << "Starting the Gateway connectivity monitor thread...";
    folly::setThreadName(gwConnectivityMonitorId);
    gatewayConnectivityMonitor.run();
    LOG(INFO) << "Gateway connectivity monitor thread stopped.";
  }));

  // create fbmeshd thrift server
  auto server = std::make_unique<apache::thrift::ThriftServer>();
  allThreads.emplace_back(
      std::thread([&server, &nlHandler, &evl, &openRMetricManager, &routing]() {
        folly::EventBase evb;
        server->setInterface(std::make_unique<MeshServiceHandler>(
            evl, nlHandler, openRMetricManager.get(), routing.get()));
        server->getEventBaseManager()->setEventBase(&evb, false);
        server->setPort(FLAGS_fbmeshd_service_port);

        LOG(INFO) << "starting fbmeshd server...";
        server->serve();
        LOG(INFO) << "fbmeshd server got stopped...";
      }));

#ifdef ENABLE_SYSTEMD_NOTIFY
  // Notify systemd that this service is ready
  sd_notify(0, "READY=1");
#endif

  evl.run();

#ifdef ENABLE_SYSTEMD_NOTIFY
  sd_notify(0, "STOPPING=1");
#endif

  if (watchdog) {
    watchdog->stop();
    watchdog->waitUntilStopped();
  }

  LOG(INFO) << "Reclaiming thrift server thread";
  server->stop();

  gatewayConnectivityMonitor.stop();
  gatewayConnectivityMonitor.waitUntilStopped();

  if (separa) {
    separa->stop();
    separa->waitUntilStopped();
  }

  // Wait for all threads to finish
  for (auto& t : allThreads) {
    t.join();
  }

  LOG(INFO) << "Stopping fbmesh daemon";
  return 0;
}
