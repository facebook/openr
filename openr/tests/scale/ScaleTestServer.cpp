/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * ScaleTestServer - Real network scale testing against a DUT
 *
 * This binary connects to a real OpenR switch (FBOSS/EOS) via VLAN trunk
 * and runs scale tests using SparkFaker (for neighbor simulation) and
 * KvStoreThriftInjector (for topology injection).
 *
 * Usage:
 *   buck2 run fbcode//openr/tests/scale:scale_test_server -- \
 *     --dut_host=192.168.1.1 \
 *     --interfaces=eth0.1,eth0.2,eth0.3,eth0.4 \
 *     --num_spines=64 \
 *     --num_leaves=252
 */

#include <net/if.h>
#include <chrono>
#include <csignal>
#include <thread>

#include <fmt/format.h>
#include <folly/init/Init.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <openr/tests/scale/KvStoreThriftInjector.h>
#include <openr/tests/scale/RealSparkIo.h>
#include <openr/tests/scale/SparkFaker.h>
#include <openr/tests/scale/TopologyGenerator.h>

DEFINE_string(dut_host, "192.168.1.1", "DUT hostname or IP address");

DEFINE_int32(dut_port, 2018, "DUT OpenR thrift port");

DEFINE_string(
    interfaces,
    "",
    "Comma-separated list of VLAN interfaces (e.g., eth0.1,eth0.2,eth0.3,eth0.4)");

DEFINE_int32(num_spines, 64, "Number of spine neighbors to simulate");

DEFINE_int32(num_leaves, 252, "Number of leaf nodes in the topology");

DEFINE_int32(
    num_super_spines,
    0,
    "Number of super-spine nodes (0 for 2-tier, >0 for 3-tier)");

DEFINE_int32(num_pods, 8, "Number of pods in the topology");

DEFINE_string(
    topology_type,
    "bbf-simple",
    "Topology type: bbf-simple, bbf-full, or custom");

DEFINE_bool(
    inject_topology, true, "Inject the full topology into DUT's KvStore");

DEFINE_bool(
    simulate_neighbors, true, "Simulate Spark neighbors via real UDP packets");

DEFINE_int32(
    run_duration_sec,
    0,
    "Duration to run in seconds (0 = run until interrupted)");

DEFINE_bool(verify_routes, true, "Verify routes are computed after injection");

namespace {

std::atomic<bool> g_running{true};

void
signalHandler(int /* signal */) {
  LOG(INFO) << "Received interrupt signal, shutting down...";
  g_running.store(false);
}

/*
 * Parse comma-separated interface list into vector
 */
std::vector<std::string>
parseInterfaces(const std::string& interfaces) {
  std::vector<std::string> result;
  std::stringstream ss(interfaces);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      result.push_back(item);
    }
  }
  return result;
}

/*
 * Get interface index from name using if_nametoindex
 */
int
getIfIndex(const std::string& ifName) {
  unsigned int idx = if_nametoindex(ifName.c_str());
  if (idx == 0) {
    LOG(ERROR) << "Failed to get ifIndex for " << ifName << ": "
               << strerror(errno);
    return -1;
  }
  return static_cast<int>(idx);
}

} // namespace

int
main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  google::SetStderrLogging(google::INFO);

  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  LOG(INFO) << "\n"
            << "╔══════════════════════════════════════════════════════════╗\n"
            << "║          OpenR Scale Test Server                         ║\n"
            << "╚══════════════════════════════════════════════════════════╝";

  LOG(INFO) << fmt::format("[SCALE-TEST] Configuration:");
  LOG(INFO) << fmt::format(
      "  DUT Host:        {}:{}", FLAGS_dut_host, FLAGS_dut_port);
  LOG(INFO) << fmt::format("  Topology:        {}", FLAGS_topology_type);
  LOG(INFO) << fmt::format("  Spines:          {}", FLAGS_num_spines);
  LOG(INFO) << fmt::format("  Leaves:          {}", FLAGS_num_leaves);
  LOG(INFO) << fmt::format("  Super-spines:    {}", FLAGS_num_super_spines);
  LOG(INFO) << fmt::format("  Pods:            {}", FLAGS_num_pods);
  LOG(INFO) << fmt::format(
      "  Interfaces:      {}",
      FLAGS_interfaces.empty() ? "(none)" : FLAGS_interfaces);
  LOG(INFO) << fmt::format(
      "  Inject topology: {}", FLAGS_inject_topology ? "yes" : "no");
  LOG(INFO) << fmt::format(
      "  Simulate nbrs:   {}", FLAGS_simulate_neighbors ? "yes" : "no");
  LOG(INFO) << fmt::format(
      "  Verify routes:   {}", FLAGS_verify_routes ? "yes" : "no");
  LOG(INFO) << fmt::format(
      "  Run duration:    {} sec (0=infinite)", FLAGS_run_duration_sec);
  LOG(INFO) << "";
  LOG(INFO)
      << "TIP: Use -v=1 for detailed state transitions, -v=2 for packet-level debug";
  LOG(INFO) << "";

  /*
   * Parse interfaces
   */
  auto interfaces = parseInterfaces(FLAGS_interfaces);
  if (interfaces.empty() && FLAGS_simulate_neighbors) {
    LOG(ERROR) << "No interfaces specified. Use --interfaces=eth0.1,eth0.2,...";
    return 1;
  }

  LOG(INFO) << fmt::format("Interfaces: {}", FLAGS_interfaces);

  /*
   * Generate topology
   */
  LOG(INFO) << "Generating topology...";
  openr::Topology topology;

  if (FLAGS_topology_type == "bbf-simple") {
    topology = openr::TopologyGenerator::createBbfSimple(
        FLAGS_num_spines,
        FLAGS_num_leaves,
        FLAGS_num_super_spines,
        FLAGS_num_pods);
  } else if (FLAGS_topology_type == "bbf-full") {
    topology = openr::TopologyGenerator::createBbf(
        FLAGS_num_spines,
        FLAGS_num_leaves,
        FLAGS_num_super_spines,
        FLAGS_num_pods);
  } else {
    LOG(ERROR) << "Unknown topology type: " << FLAGS_topology_type;
    return 1;
  }

  LOG(INFO) << fmt::format(
      "Generated topology: {} routers, {} adjacencies, {} prefixes",
      topology.getRouterCount(),
      topology.getTotalAdjacencyCount(),
      topology.getTotalPrefixCount());

  /*
   * Connect to DUT
   */
  openr::KvStoreThriftInjector injector(FLAGS_dut_host, FLAGS_dut_port);

  if (!injector.connect()) {
    LOG(ERROR) << "Failed to connect to DUT";
    return 1;
  }

  auto dutNodeName = injector.getDutNodeName();
  LOG(INFO) << "Connected to DUT: " << dutNodeName;

  /*
   * Setup SparkFaker with real I/O (if simulating neighbors)
   */
  std::shared_ptr<openr::SparkFaker> faker;
  std::shared_ptr<openr::RealSparkIo> realIo;

  if (FLAGS_simulate_neighbors && !interfaces.empty()) {
    LOG(INFO) << "Setting up SparkFaker with real network I/O...";

    realIo = std::make_shared<openr::RealSparkIo>();

    /*
     * Distribute spines across interfaces
     */
    int neighborsPerInterface = FLAGS_num_spines / interfaces.size();
    int neighborIdx = 0;

    faker = std::make_shared<openr::SparkFaker>(realIo);

    for (size_t i = 0; i < interfaces.size(); ++i) {
      const auto& ifName = interfaces[i];
      int ifIndex = getIfIndex(ifName);

      if (ifIndex < 0) {
        LOG(WARNING) << "Skipping interface " << ifName << " (not found)";
        continue;
      }

      /*
       * Add neighbors for this interface
       */
      int neighborsOnThisIf = (i == interfaces.size() - 1)
          ? (FLAGS_num_spines - neighborIdx) /* Last interface gets remainder */
          : neighborsPerInterface;

      for (int j = 0; j < neighborsOnThisIf && neighborIdx < FLAGS_num_spines;
           ++j, ++neighborIdx) {
        std::string spineName = fmt::format("spine-{}", neighborIdx);
        std::string spineIfName = fmt::format("{}-to-dut", spineName);

        /*
         * Generate link-local IPv6 address for the fake neighbor
         */
        std::string v6Addr = fmt::format("fe80::{:x}", 0x1000 + neighborIdx);

        faker->addNeighbor(
            spineName, spineIfName, ifIndex, v6Addr, ifName, ifIndex);

        VLOG(1) << fmt::format(
            "Added fake neighbor {} on {} ({})", spineName, ifName, v6Addr);
      }

      LOG(INFO) << fmt::format(
          "Interface {}: {} neighbors", ifName, neighborsOnThisIf);
    }

    LOG(INFO) << fmt::format(
        "SparkFaker configured with {} neighbors across {} interfaces",
        faker->getNeighborCount(),
        interfaces.size());
  }

  /*
   * Inject topology
   */
  if (FLAGS_inject_topology) {
    LOG(INFO) << "Injecting topology into DUT KvStore...";
    auto startTime = std::chrono::steady_clock::now();

    size_t keysInjected = injector.injectTopology(topology);

    auto endTime = std::chrono::steady_clock::now();
    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          endTime - startTime)
                          .count();

    LOG(INFO) << fmt::format(
        "Injected {} keys in {} ms ({} keys/sec)",
        keysInjected,
        durationMs,
        durationMs > 0 ? (keysInjected * 1000 / durationMs) : 0);
  }

  /*
   * Start SparkFaker (if simulating neighbors)
   */
  if (faker) {
    LOG(INFO) << "Starting SparkFaker...";
    faker->start();
  }

  /*
   * Wait for route computation (if verifying)
   */
  if (FLAGS_verify_routes) {
    LOG(INFO) << "Waiting for route computation...";
    std::this_thread::sleep_for(std::chrono::seconds(5));

    auto routeDb = injector.getRouteDatabase(dutNodeName);
    LOG(INFO) << fmt::format(
        "DUT computed {} unicast routes, {} MPLS routes",
        routeDb.unicastRoutes()->size(),
        routeDb.mplsRoutes()->size());
  }

  /*
   * Main loop
   */
  LOG(INFO) << "=== Scale test running. Press Ctrl+C to stop. ===";

  auto startTime = std::chrono::steady_clock::now();
  int64_t lastStatsTime = 0;

  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - startTime)
                       .count();

    if (FLAGS_run_duration_sec > 0 && elapsed >= FLAGS_run_duration_sec) {
      LOG(INFO) << "[SCALE-TEST] Run duration reached, stopping...";
      break;
    }

    /*
     * Periodic stats every 10 seconds
     */
    if (elapsed - lastStatsTime >= 10) {
      lastStatsTime = elapsed;

      LOG(INFO) << fmt::format(
          "\n========== Status at {} seconds ==========", elapsed);

      if (faker) {
        LOG(INFO) << faker->getStatsReport();
        VLOG(1) << faker->getNeighborStatusReport();
      }
    }
  }

  /*
   * Final stats
   */
  LOG(INFO) << "\n========== Final Statistics ==========";
  if (faker) {
    LOG(INFO) << faker->getStatsReport();
    LOG(INFO) << faker->getNeighborStatusReport();
  }

  /*
   * Cleanup
   */
  LOG(INFO) << "[SCALE-TEST] Shutting down...";

  if (faker) {
    faker->stop();
  }

  injector.disconnect();

  LOG(INFO) << "[SCALE-TEST] Scale test complete.";
  return 0;
}
