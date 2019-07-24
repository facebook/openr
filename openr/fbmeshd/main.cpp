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

#include <chrono>
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
#include <openr/fbmeshd/common/Util.h>
#include <openr/fbmeshd/gateway-11s-root-route-programmer/Gateway11sRootRouteProgrammer.h>
#include <openr/fbmeshd/gateway-connectivity-monitor/GatewayConnectivityMonitor.h>
#include <openr/fbmeshd/gateway-connectivity-monitor/RouteDampener.h>
#include <openr/fbmeshd/pinger/PeerPinger.h>
#include <openr/fbmeshd/route-update-monitor/RouteUpdateMonitor.h>
#include <openr/fbmeshd/routing/MetricManager80211s.h>
#include <openr/fbmeshd/routing/PeriodicPinger.h>
#include <openr/fbmeshd/routing/Routing.h>
#include <openr/fbmeshd/routing/SyncRoutes80211s.h>
#include <openr/fbmeshd/routing/UDPRoutingPacketTransport.h>
#include <openr/watchdog/Watchdog.h>

using namespace openr::fbmeshd;

using namespace std::chrono_literals;

DEFINE_int32(fbmeshd_service_port, 30303, "fbmeshd thrift service port");

DEFINE_string(node_name, "node1", "The name of current node");

DEFINE_bool(
    enable_userspace_mesh_peering,
    true,
    "If set, mesh peering management handshake will be done in userspace");

// Gateway Connectivity Monitor configs
DEFINE_string(
    gateway_connectivity_monitor_interface,
    "eth0",
    "The interface that the gateway connectivity monitor runs on");
DEFINE_string(
    gateway_connectivity_monitor_addresses,
    "8.8.4.4:443,1.1.1.1:443",
    "A comma-separated list of addresses that the gateway connectivity monitor "
    "connects to to check WAN connectivity (host:port)");
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

DEFINE_uint32(routing_ttl, 32, "TTL for routing elements");
DEFINE_int32(routing_tos, 192, "ToS value for routing messages");
DEFINE_uint32(
    routing_active_path_timeout_ms, 30000, "Routing active path timeout (ms)");
DEFINE_uint32(
    routing_root_pann_interval_ms, 5000, "Routing PANN interval (ms)");
DEFINE_uint32(
    routing_metric_manager_ewma_factor_log2,
    7,
    "Routing metric manager EWMA log2 factor (e.g. value of 7 here implies"
    " factor of 2^7=128)");
DEFINE_double(
    routing_metric_manager_rssi_weight,
    0.0,
    "Weight of the RSSI based metric (vs. bitrate) in the combined metric");

// TODO T47794858:  The following flags are deprecated and should not be used.
//
// They will be removed in a future version of fbmeshd, at which time anyone
// using them will result in fbmeshd not starting (as they will not be parsed).
DEFINE_bool(enable_short_names, false, "DEPRECATED on 2019-07-24, do not use");

namespace {
constexpr folly::StringPiece kHostName{"localhost"};

const auto kMetricManagerInterval{3s};
const auto kMetricManagerHysteresisFactorLog2{2};
const auto kMetricManagerBaseBitrate{60};
const auto kPeriodicPingerInterval{10s};

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

  SignalHandler signalHandler{evl};
  signalHandler.registerSignalHandler(SIGINT);
  signalHandler.registerSignalHandler(SIGTERM);
  signalHandler.registerSignalHandler(SIGABRT);

  monitorEventLoopWithWatchdog(
      &evl, "fbmeshd_shared_event_loop", watchdog.get());
  AuthsaeCallbackHelpers::init(evl);

  Nl80211Handler nlHandler{evl, FLAGS_enable_userspace_mesh_peering};
  auto returnValue = nlHandler.joinMeshes();
  if (returnValue != R_SUCCESS) {
    return returnValue;
  }

  // Set up the zmq context for this process.
  fbzmq::Context zmqContext;

  RouteUpdateMonitor routeMonitor{evl, nlHandler};

  std::unique_ptr<PeerPinger> peerPinger(nullptr);
  if (FLAGS_enable_peer_pinger) {
    allThreads.emplace_back(std::thread([&peerPinger, &nlHandler]() {
      folly::EventBase evb;
      peerPinger = std::make_unique<PeerPinger>(&evb, nlHandler);
      peerPinger->run();
    }));
  }

  PeerSelector peerSelector{evl, nlHandler, rssiThreshold};

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

  std::unique_ptr<folly::EventBase> routingEventLoop =
      std::make_unique<folly::EventBase>();
  std::unique_ptr<MetricManager80211s> metricManager80211s =
      std::make_unique<MetricManager80211s>(
          routingEventLoop.get(),
          kMetricManagerInterval,
          nlHandler,
          FLAGS_routing_metric_manager_ewma_factor_log2,
          kMetricManagerHysteresisFactorLog2,
          kMetricManagerBaseBitrate,
          FLAGS_routing_metric_manager_rssi_weight);
  std::unique_ptr<Routing> routing = std::make_unique<Routing>(
      routingEventLoop.get(),
      metricManager80211s.get(),
      nlHandler.lookupMeshNetif().maybeMacAddress.value(),
      FLAGS_routing_ttl,
      std::chrono::milliseconds{FLAGS_routing_active_path_timeout_ms},
      std::chrono::milliseconds{FLAGS_routing_root_pann_interval_ms});
  std::unique_ptr<UDPRoutingPacketTransport> routingPacketTransport =
      std::make_unique<UDPRoutingPacketTransport>(
          routingEventLoop.get(), 6668, FLAGS_routing_tos);
  std::unique_ptr<PeriodicPinger> periodicPinger =
      std::make_unique<PeriodicPinger>(
          routingEventLoop.get(),
          folly::IPAddressV6{"ff02::1%mesh0"},
          folly::IPAddressV6{
              folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
              nlHandler.lookupMeshNetif().maybeMacAddress.value()},
          kPeriodicPingerInterval,
          "mesh0");
  periodicPinger->scheduleTimeout(1s);

  std::unique_ptr<SyncRoutes80211s> syncRoutes80211s =
      std::make_unique<SyncRoutes80211s>(
          routing.get(), nlHandler.lookupMeshNetif().maybeMacAddress.value());

  static constexpr auto syncRoutes80211sId{"SyncRoutes80211s"};
  monitorEventLoopWithWatchdog(
      syncRoutes80211s.get(), syncRoutes80211sId, watchdog.get());
  allThreads.emplace_back(std::thread([&syncRoutes80211s]() noexcept {
    LOG(INFO) << "Starting the SyncRoutes80211s thread...";
    folly::setThreadName(syncRoutes80211sId);
    syncRoutes80211s->run();
    LOG(INFO) << "SyncRoutes80211s thread stopped.";
  }));

  routing->setSendPacketCallback(
      [&routingPacketTransport](
          folly::MacAddress da, std::unique_ptr<folly::IOBuf> buf) {
        routingPacketTransport->sendPacket(da, std::move(buf));
      });

  routingPacketTransport->setReceivePacketCallback(
      [&routing](folly::MacAddress sa, std::unique_ptr<folly::IOBuf> buf) {
        routing->receivePacket(sa, std::move(buf));
      });

  static constexpr auto routingId{"Routing"};
  allThreads.emplace_back(std::thread([&routingEventLoop]() noexcept {
    LOG(INFO) << "Starting Routing";
    folly::setThreadName(routingId);
    routingEventLoop->loopForever();
    LOG(INFO) << "Routing thread stopped.";
  }));

  auto gatewayConnectivityMonitorAddresses{parseCsvFlag<folly::SocketAddress>(
      FLAGS_gateway_connectivity_monitor_addresses, [](const std::string& str) {
        folly::SocketAddress address;
        address.setFromIpPort(str);
        return address;
      })};

  StatsClient statsClient{};

  GatewayConnectivityMonitor gatewayConnectivityMonitor{
      nlHandler,
      FLAGS_gateway_connectivity_monitor_interface,
      std::move(gatewayConnectivityMonitorAddresses),
      std::chrono::seconds{FLAGS_gateway_connectivity_monitor_interval_s},
      std::chrono::seconds{FLAGS_gateway_connectivity_monitor_socket_timeout_s},
      FLAGS_route_dampener_penalty,
      FLAGS_route_dampener_suppress_limit,
      FLAGS_route_dampener_reuse_limit,
      std::chrono::seconds{FLAGS_route_dampener_halflife},
      std::chrono::seconds{FLAGS_route_dampener_max_suppress_limit},
      FLAGS_gateway_connectivity_monitor_robustness,
      static_cast<uint8_t>(FLAGS_gateway_connectivity_monitor_set_root_mode),
      gateway11sRootRouteProgrammer.get(),
      routing.get(),
      statsClient};

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
      std::thread([&server, &nlHandler, &evl, &routing, &statsClient]() {
        folly::EventBase evb;
        server->setInterface(std::make_unique<MeshServiceHandler>(
            evl, nlHandler, routing.get(), statsClient));
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

  if (routingEventLoop) {
    routing->resetSendPacketCallback();
    routingEventLoop->terminateLoopSoon();
  }

  // Wait for all threads to finish
  for (auto& t : allThreads) {
    t.join();
  }

  LOG(INFO) << "Stopping fbmesh daemon";
  return 0;
}
