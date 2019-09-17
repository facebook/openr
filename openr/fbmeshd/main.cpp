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

#include <openr/fbmeshd/802.11s/AuthsaeCallbackHelpers.h>
#include <openr/fbmeshd/802.11s/Nl80211Handler.h>
#include <openr/fbmeshd/FollySignalHandler.h>
#include <openr/fbmeshd/MeshServiceHandler.h>
#include <openr/fbmeshd/SignalHandler.h>
#include <openr/fbmeshd/common/Constants.h>
#include <openr/fbmeshd/common/Util.h>
#include <openr/fbmeshd/gateway-11s-root-route-programmer/Gateway11sRootRouteProgrammer.h>
#include <openr/fbmeshd/gateway-connectivity-monitor/GatewayConnectivityMonitor.h>
#include <openr/fbmeshd/gateway-connectivity-monitor/RouteDampener.h>
#include <openr/fbmeshd/route-update-monitor/RouteUpdateMonitor.h>
#include <openr/fbmeshd/routing/MetricManager80211s.h>
#include <openr/fbmeshd/routing/PeriodicPinger.h>
#include <openr/fbmeshd/routing/Routing.h>
#include <openr/fbmeshd/routing/SyncRoutes80211s.h>
#include <openr/fbmeshd/routing/UDPRoutingPacketTransport.h>

using namespace openr::fbmeshd;

using namespace std::chrono_literals;

DEFINE_int32(fbmeshd_service_port, 30303, "fbmeshd thrift service port");

DEFINE_string(node_name, "node1", "The name of current node");

DEFINE_string(mesh_ifname, "mesh0", "Mesh interface name");

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

// TODO: The following flags are deprecated and should not be used.
//
// They will be removed in a future version of fbmeshd, at which time using them
// will result in fbmeshd not starting (as they will not be parsed).
// TODO T53288272
DEFINE_uint32(
    peer_selector_max_allowed, 0, "DEPRECATED on 2019-08-30, do not use");
DEFINE_uint32(
    peer_selector_eviction_period_s, 0, "DEPRECATED on 2019-08-30, do not use");
DEFINE_uint32(
    peer_selector_min_gate_connections,
    0,
    "DEPRECATED on 2019-08-30, do not use");
DEFINE_int32(
    peer_selector_poll_interval_s, 0, "DEPRECATED on 2019-08-30, do not use");
// TODO T53487225
DEFINE_int32(monitor_rep_port, 0, "DEPRECATED on 2019-09-04, do not use");
// TODO T54087050
DEFINE_bool(enable_watchdog, false, "DEPRECATED on 2019-09-16, do not use");
DEFINE_int32(watchdog_interval_s, 0, "DEPRECATED on 2019-09-16, do not use");
DEFINE_int32(watchdog_threshold_s, 0, "DEPRECATED on 2019-09-16, do not use");
DEFINE_int32(memory_limit_mb, 0, "DEPRECATED on 2019-09-16, do not use");

namespace {
constexpr folly::StringPiece kHostName{"localhost"};

const auto kMetricManagerInterval{3s};
const auto kMetricManagerHysteresisFactorLog2{2};
const auto kMetricManagerBaseBitrate{60};
const auto kPeriodicPingerInterval{10s};

} // namespace

int
main(int argc, char* argv[]) {
  folly::init(&argc, &argv);

  // Set stdout to be line-buffered, to assist with integration testing that
  // depends on log output
  setvbuf(stdout, nullptr /*buffer*/, _IOLBF, 0);

  LOG(INFO) << "Starting fbmesh daemon...";

  std::vector<std::thread> allThreads{};

  fbzmq::ZmqEventLoop evl;

  SignalHandler signalHandler{evl};
  signalHandler.registerSignalHandler(SIGABRT);
  signalHandler.registerSignalHandler(SIGINT);
  signalHandler.registerSignalHandler(SIGTERM);

  AuthsaeCallbackHelpers::init(evl);

  Nl80211Handler nlHandler{
      evl, FLAGS_mesh_ifname, FLAGS_enable_userspace_mesh_peering};
  auto returnValue = nlHandler.joinMeshes();
  if (returnValue != R_SUCCESS) {
    return returnValue;
  }

  RouteUpdateMonitor routeMonitor{evl, nlHandler};

  std::unique_ptr<Gateway11sRootRouteProgrammer> gateway11sRootRouteProgrammer;
  static constexpr auto gateway11sRootRouteProgrammerId{
      "Gateway11sRootRouteProgrammer"};
  if (FLAGS_enable_gateway_11s_root_route_programmer) {
    gateway11sRootRouteProgrammer = std::make_unique<
        Gateway11sRootRouteProgrammer>(
        nlHandler,
        FLAGS_mesh_ifname,
        std::chrono::seconds{
            FLAGS_gateway_11s_root_route_programmer_interval_s},
        FLAGS_gateway_11s_root_route_programmer_gateway_change_threshold_factor);
    allThreads.emplace_back(
        std::thread([&gateway11sRootRouteProgrammer]() noexcept {
          LOG(INFO) << "Starting Gateway11sRootRouteProgrammer thread...";
          folly::setThreadName(gateway11sRootRouteProgrammerId);
          gateway11sRootRouteProgrammer->run();
          LOG(INFO) << "Gateway11sRootRouteProgrammer thread stopped.";
        }));
  }

  folly::EventBase routingEventLoop;

  FollySignalHandler follySignalHandler{routingEventLoop, evl};
  follySignalHandler.registerSignalHandler(SIGABRT);
  follySignalHandler.registerSignalHandler(SIGINT);
  follySignalHandler.registerSignalHandler(SIGTERM);

  std::unique_ptr<MetricManager80211s> metricManager80211s =
      std::make_unique<MetricManager80211s>(
          kMetricManagerInterval,
          nlHandler,
          FLAGS_routing_metric_manager_ewma_factor_log2,
          kMetricManagerHysteresisFactorLog2,
          kMetricManagerBaseBitrate,
          FLAGS_routing_metric_manager_rssi_weight);

  static constexpr auto metricManager80211sId{"MetricManager80211s"};
  allThreads.emplace_back(std::thread([&metricManager80211s]() noexcept {
    LOG(INFO) << "Starting MetricManager80211s thread...";
    folly::setThreadName(metricManager80211sId);
    metricManager80211s->run();
    LOG(INFO) << "MetricManager80211s thread stopped.";
  }));

  std::unique_ptr<Routing> routing = std::make_unique<Routing>(
      &routingEventLoop,
      metricManager80211s.get(),
      nlHandler.lookupMeshNetif().maybeMacAddress.value(),
      FLAGS_routing_ttl,
      std::chrono::milliseconds{FLAGS_routing_active_path_timeout_ms},
      std::chrono::milliseconds{FLAGS_routing_root_pann_interval_ms});
  std::unique_ptr<UDPRoutingPacketTransport> routingPacketTransport =
      std::make_unique<UDPRoutingPacketTransport>(
          &routingEventLoop, FLAGS_mesh_ifname, 6668, FLAGS_routing_tos);

  // set up NetlinkProtocolSocket in a new thread to program the linux kernel
  auto nlProtocolSocketEventLoop = std::make_unique<fbzmq::ZmqEventLoop>();
  std::unique_ptr<openr::rnl::NetlinkProtocolSocket> nlProtocolSocket;
  nlProtocolSocket = std::make_unique<openr::rnl::NetlinkProtocolSocket>(
      nlProtocolSocketEventLoop.get());
  allThreads.emplace_back(
      std::thread([&nlProtocolSocket, &nlProtocolSocketEventLoop]() {
        LOG(INFO) << "Starting NetlinkProtolSocketEvl thread...";
        folly::setThreadName("NetlinkProtolSocketEvl");
        nlProtocolSocket->init();
        nlProtocolSocketEventLoop->run();
        LOG(INFO) << "NetlinkProtolSocketEvl thread stopped.";
      }));
  nlProtocolSocketEventLoop->waitUntilRunning();

  LOG(INFO) << "Creating PeriodicPinger...";
  std::unique_ptr<PeriodicPinger> periodicPinger =
      std::make_unique<PeriodicPinger>(
          &routingEventLoop,
          folly::IPAddressV6{folly::sformat("ff02::1%{}", FLAGS_mesh_ifname)},
          folly::IPAddressV6{
              folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
              nlHandler.lookupMeshNetif().maybeMacAddress.value()},
          kPeriodicPingerInterval,
          FLAGS_mesh_ifname);

  std::unique_ptr<SyncRoutes80211s> syncRoutes80211s =
      std::make_unique<SyncRoutes80211s>(
          routing.get(),
          std::move(nlProtocolSocket),
          nlHandler.lookupMeshNetif().maybeMacAddress.value(),
          FLAGS_mesh_ifname);

  static constexpr auto syncRoutes80211sId{"SyncRoutes80211s"};
  allThreads.emplace_back(std::thread([&syncRoutes80211s]() noexcept {
    LOG(INFO) << "Starting SyncRoutes80211s thread...";
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
    LOG(INFO) << "Starting Routing thread...";
    folly::setThreadName(routingId);
    routingEventLoop.loopForever();
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
  allThreads.emplace_back(std::thread([&gatewayConnectivityMonitor]() noexcept {
    LOG(INFO) << "Starting GatewayConnectivityMonitor thread...";
    folly::setThreadName(gwConnectivityMonitorId);
    gatewayConnectivityMonitor.run();
    LOG(INFO) << "GatewayConnectivityMonitor thread stopped.";
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

        LOG(INFO) << "Starting fbmeshd server thread ...";
        server->serve();
        LOG(INFO) << "fbmeshd server thread stopped.";
      }));

#ifdef ENABLE_SYSTEMD_NOTIFY
  // Notify systemd that this service is ready
  sd_notify(0, "READY=1");
#endif

  evl.run();

#ifdef ENABLE_SYSTEMD_NOTIFY
  sd_notify(0, "STOPPING=1");
#endif

  LOG(INFO) << "Leaving mesh...";
  nlHandler.leaveMeshes();

  LOG(INFO) << "Reclaiming thrift server thread";
  server->stop();

  gatewayConnectivityMonitor.stop();
  gatewayConnectivityMonitor.waitUntilStopped();

  metricManager80211s->stop();
  metricManager80211s->waitUntilStopped();

  syncRoutes80211s->stop();
  syncRoutes80211s->waitUntilStopped();

  nlProtocolSocket.reset();
  if (nlProtocolSocketEventLoop) {
    nlProtocolSocketEventLoop->stop();
    nlProtocolSocketEventLoop->waitUntilStopped();
  }

  routing->resetSendPacketCallback();
  routingEventLoop.terminateLoopSoon();

  if (gateway11sRootRouteProgrammer) {
    gateway11sRootRouteProgrammer->stop();
    gateway11sRootRouteProgrammer->waitUntilStopped();
  }

  // Wait for all threads to finish
  for (auto& t : allThreads) {
    t.join();
  }

  LOG(INFO) << "Stopping fbmesh daemon...";
  return 0;
}
