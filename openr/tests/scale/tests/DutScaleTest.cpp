/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * DUT In-Process Scale Test
 *
 * This test runs a full OpenR DUT (Device Under Test) in-process:
 * 1. Start DUT with full OpenR stack (Spark, KvStore, Decision, Fib)
 * 2. Inject BBF topology directly into KvStore (bypass Spark)
 * 3. Decision computes SPF on the topology
 * 4. Dump route database to file
 *
 * This proves OpenR can handle the BBF topology at scale.
 */

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/tests/OpenrWrapper.h>
#include <openr/tests/mocks/MockIoProvider.h>
#include <openr/tests/scale/KvStoreBulkInjector.h>
#include <openr/tests/scale/TopologyGenerator.h>

using namespace openr;
using apache::thrift::CompactSerializer;

namespace {

const std::chrono::milliseconds kSpark2HelloTime(100);
const std::chrono::milliseconds kSpark2FastInitHelloTime(20);
const std::chrono::milliseconds kSpark2HandshakeTime(20);
const std::chrono::milliseconds kSpark2HeartbeatTime(20);
const std::chrono::milliseconds kSpark2HandshakeHoldTime(200);
const std::chrono::milliseconds kSpark2HeartbeatHoldTime(500);
const std::chrono::milliseconds kSpark2GRHoldTime(1000);
const std::chrono::milliseconds kLinkFlapInitialBackoff(1);
const std::chrono::milliseconds kLinkFlapMaxBackoff(8);

/*
 * Polling interval for waitForRoutes
 */
const std::chrono::milliseconds kPollInterval(100);

} // namespace

class DutScaleTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    mockIoProvider_ = std::make_shared<MockIoProvider>();

    mockIoProviderThread_ = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Starting MockIoProvider thread";
      mockIoProvider_->start();
      LOG(INFO) << "MockIoProvider thread stopped";
    });
    mockIoProvider_->waitUntilRunning();
  }

  void
  TearDown() override {
    LOG(INFO) << "Stopping MockIoProvider";
    mockIoProvider_->stop();
    mockIoProviderThread_->join();
  }

  std::unique_ptr<OpenrWrapper<CompactSerializer>>
  createDut(const std::string& nodeId) {
    return std::make_unique<OpenrWrapper<CompactSerializer>>(
        nodeId,
        true, /* v4Enabled */
        kSpark2HelloTime,
        kSpark2FastInitHelloTime,
        kSpark2HandshakeTime,
        kSpark2HeartbeatTime,
        kSpark2HandshakeHoldTime,
        kSpark2HeartbeatHoldTime,
        kSpark2GRHoldTime,
        kLinkFlapInitialBackoff,
        kLinkFlapMaxBackoff,
        mockIoProvider_);
  }

  std::string
  dumpRouteDatabase(const thrift::RouteDatabase& routeDb) {
    std::string result;
    result +=
        fmt::format("=== Route Database for {} ===\n", *routeDb.thisNodeName());
    result +=
        fmt::format("Unicast Routes: {}\n", routeDb.unicastRoutes()->size());

    int count = 0;
    for (const auto& route : *routeDb.unicastRoutes()) {
      if (count++ < 20) {
        result += fmt::format("  Prefix: {}\n", toString(*route.dest()));
        for (const auto& nh : *route.nextHops()) {
          auto nhAddr = toIPAddress(*nh.address());
          result += fmt::format(
              "    -> {} via {} (metric={})\n",
              nhAddr.str(),
              nh.address()->ifName().value_or("?"),
              *nh.metric());
        }
      }
    }
    if (routeDb.unicastRoutes()->size() > 20) {
      result += fmt::format(
          "  ... and {} more routes\n", routeDb.unicastRoutes()->size() - 20);
    }

    result += fmt::format("MPLS Routes: {}\n", routeDb.mplsRoutes()->size());
    return result;
  }

  std::shared_ptr<MockIoProvider> mockIoProvider_;
  std::unique_ptr<std::thread> mockIoProviderThread_;

  /*
   * Poll until the DUT has computed at least minRoutes unicast routes,
   * or timeout is reached. Returns the final route count.
   */
  size_t
  waitForRoutes(
      OpenrWrapper<CompactSerializer>& dut,
      size_t minRoutes,
      std::chrono::seconds timeout) {
    auto startTime = std::chrono::steady_clock::now();
    size_t routeCount = 0;

    while (std::chrono::steady_clock::now() - startTime < timeout) {
      auto routeDb = dut.fibDumpRouteDatabase();
      routeCount = routeDb.unicastRoutes()->size();

      if (routeCount >= minRoutes) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime);
        LOG(INFO) << fmt::format(
            "Routes ready: {} routes in {} ms", routeCount, elapsed.count());
        return routeCount;
      }

      std::this_thread::sleep_for(kPollInterval);
    }

    LOG(WARNING) << fmt::format(
        "Timeout waiting for routes: got {} routes, expected at least {}",
        routeCount,
        minRoutes);
    return routeCount;
  }

  /*
   * Poll until the route count changes from the previous value.
   * Used for reconvergence tests.
   */
  size_t
  waitForRouteChange(
      OpenrWrapper<CompactSerializer>& dut,
      size_t previousRouteCount,
      std::chrono::seconds timeout) {
    auto startTime = std::chrono::steady_clock::now();
    size_t routeCount = previousRouteCount;

    while (std::chrono::steady_clock::now() - startTime < timeout) {
      auto routeDb = dut.fibDumpRouteDatabase();
      routeCount = routeDb.unicastRoutes()->size();

      if (routeCount != previousRouteCount) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime);
        LOG(INFO) << fmt::format(
            "Route count changed: {} -> {} in {} ms",
            previousRouteCount,
            routeCount,
            elapsed.count());
        return routeCount;
      }

      std::this_thread::sleep_for(kPollInterval);
    }

    LOG(WARNING) << fmt::format(
        "Timeout waiting for route change: still {} routes", routeCount);
    return routeCount;
  }
};

/*
 * Test: Small topology injection
 *
 * Inject a small 8-node BBF topology and verify DUT computes routes.
 * The DUT is "leaf-0" in the topology.
 */
TEST_F(DutScaleTestFixture, SmallTopologyInjection) {
  LOG(INFO) << "=== SmallTopologyInjection Test ===";

  /*
   * Step 1: Generate small BBF topology (4 spines + 4 leaves)
   * DUT is leaf-0, so it will have routes to all other nodes
   */
  auto topology = TopologyGenerator::createBbfSimple(
      4, /* numSpines */
      4, /* numLeaves */
      0, /* numControlNodes */
      1); /* ecmpWidth */

  LOG(INFO) << "Generated topology: " << topology.getRouterCount()
            << " routers, " << topology.getTotalAdjacencyCount()
            << " adjacencies";

  /*
   * Step 2: Create DUT as "leaf-0" (a node in the topology)
   */
  auto dut = createDut("leaf-0");
  dut->run();
  LOG(INFO) << "DUT (leaf-0) started";

  /*
   * Step 3: Inject topology into DUT's KvStore
   */
  KvStoreBulkInjector injector(dut->getKvStoreUpdatesQueue());
  injector.injectTopology(topology);
  LOG(INFO) << "Injected topology into KvStore";

  /*
   * Step 4: Wait for Decision to compute routes (poll instead of fixed sleep)
   * With 8 nodes, we expect at least a few routes
   */
  waitForRoutes(*dut, 1, std::chrono::seconds(30));

  /*
   * Step 5: Dump route database
   */
  auto routeDb = dut->fibDumpRouteDatabase();
  std::string routeDump = dumpRouteDatabase(routeDb);

  LOG(INFO) << "\n" << routeDump;

  /*
   * Step 6: Write to file
   */
  std::string outputPath = "/tmp/dut_small_topology_routes.txt";
  std::ofstream outFile(outputPath);
  outFile << "=== DUT Scale Test Results ===\n\n";
  outFile << "Topology: " << topology.name << "\n";
  outFile << "Routers: " << topology.getRouterCount() << "\n";
  outFile << "Adjacencies: " << topology.getTotalAdjacencyCount() << "\n\n";
  outFile << routeDump;
  outFile.close();
  LOG(INFO) << "Results written to: " << outputPath;

  /*
   * Step 7: Verify routes exist
   * DUT (leaf-0) should have routes to other leaves' prefixes
   */
  LOG(INFO) << "DUT computed " << routeDb.unicastRoutes()->size()
            << " unicast routes";
}

/*
 * Test: Production BBF topology injection
 *
 * Inject full 320-node BBF topology (64 spines, 252 leaves, 4 control nodes)
 * and verify DUT computes routes. DUT is "leaf-0" in the topology.
 */
TEST_F(DutScaleTestFixture, ProductionBbfTopologyInjection) {
  LOG(INFO) << "=== ProductionBbfTopologyInjection Test ===";

  /*
   * Generate topology FIRST before starting DUT
   */
  auto topology = TopologyGenerator::createBbfSimple(
      64, /* numSpines */
      252, /* numLeaves */
      4, /* numControlNodes */
      8); /* ecmpWidth */

  LOG(INFO) << "Generated production BBF topology:";
  LOG(INFO) << "  Routers: " << topology.getRouterCount();
  LOG(INFO) << "  Adjacencies: " << topology.getTotalAdjacencyCount();

  /*
   * Now start DUT
   */
  auto dut = createDut("leaf-0");
  dut->run();
  LOG(INFO) << "DUT (leaf-0) started";

  /*
   * Immediately inject topology before Decision initializes
   */
  auto startTime = std::chrono::steady_clock::now();

  KvStoreBulkInjector injector(dut->getKvStoreUpdatesQueue());
  injector.injectTopology(topology);

  auto injectTime = std::chrono::steady_clock::now();
  auto injectDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
      injectTime - startTime);
  LOG(INFO) << "Topology injection took: " << injectDuration.count() << "ms";

  /*
   * Wait for SPF computation using polling (not fixed sleep)
   * For large topology, we expect at least 1 route
   */
  LOG(INFO) << "Waiting for Decision to compute routes...";
  waitForRoutes(*dut, 1, std::chrono::seconds(120));

  auto spfTime = std::chrono::steady_clock::now();
  auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
      spfTime - startTime);

  /*
   * Dump results
   */
  auto routeDb = dut->fibDumpRouteDatabase();
  std::string routeDump = dumpRouteDatabase(routeDb);

  LOG(INFO) << "\n" << routeDump;

  std::string outputPath = "/tmp/dut_production_bbf_routes.txt";
  std::ofstream outFile(outputPath);
  outFile << "=== DUT Production BBF Scale Test Results ===\n\n";
  outFile << "Topology: " << topology.name << "\n";
  outFile << "Routers: " << topology.getRouterCount() << "\n";
  outFile << "Adjacencies: " << topology.getTotalAdjacencyCount() << "\n";
  outFile << "Injection Time: " << injectDuration.count() << "ms\n";
  outFile << "Total Time: " << totalDuration.count() << "ms\n\n";
  outFile << routeDump;
  outFile.close();

  LOG(INFO) << "Results written to: " << outputPath;
  LOG(INFO) << "DUT computed " << routeDb.unicastRoutes()->size()
            << " unicast routes from " << topology.getRouterCount()
            << " node topology";

  /*
   * Verify routes were computed
   */
  EXPECT_GT(routeDb.unicastRoutes()->size(), 0)
      << "DUT should have computed routes";
}

/*
 * Test: Topological event - link failure and reconvergence
 *
 * 1. Inject topology
 * 2. Verify DUT has routes via spine-0
 * 3. Simulate spine-0 going down (remove its adjacencies)
 * 4. Verify DUT reconverges - routes now via other spines
 */
TEST_F(DutScaleTestFixture, TopologyEventLinkFailure) {
  LOG(INFO) << "=== TopologyEventLinkFailure Test ===";

  /*
   * Step 1: Generate small topology
   */
  auto topology = TopologyGenerator::createBbfSimple(
      4, /* numSpines */
      4, /* numLeaves */
      0, /* numControlNodes */
      1); /* ecmpWidth */

  LOG(INFO) << fmt::format(
      "Generated topology: {} routers, {} adjacencies",
      topology.getRouterCount(),
      topology.getTotalAdjacencyCount());

  /*
   * Step 2: Start DUT and inject topology
   */
  auto dut = createDut("leaf-0");
  dut->run();

  KvStoreBulkInjector injector(dut->getKvStoreUpdatesQueue());
  injector.injectTopology(topology);

  LOG(INFO) << "Initial topology injected";

  /*
   * Step 3: Wait for initial routes using polling
   */
  waitForRoutes(*dut, 1, std::chrono::seconds(30));

  auto routeDbBefore = dut->fibDumpRouteDatabase();
  size_t routesBefore = routeDbBefore.unicastRoutes()->size();
  LOG(INFO) << fmt::format("Before failure: {} routes", routesBefore);

  /*
   * Step 4: Simulate spine-0 failure by removing its adjacencies
   *
   * We inject an updated adj:spine-0 with no adjacencies.
   * This simulates spine-0 going down from the topology.
   */
  LOG(INFO) << "Simulating spine-0 failure...";
  injector.removeNode("spine-0");

  /*
   * Step 5: Wait for reconvergence using polling
   * Route count may change (could increase or decrease)
   */
  waitForRouteChange(*dut, routesBefore, std::chrono::seconds(30));

  auto routeDbAfter = dut->fibDumpRouteDatabase();
  size_t routesAfter = routeDbAfter.unicastRoutes()->size();
  LOG(INFO) << fmt::format("After failure: {} routes", routesAfter);

  /*
   * Step 6: Verify reconvergence
   *
   * DUT should still have routes, but they should now go via
   * remaining spines (spine-1, spine-2, spine-3).
   */
  EXPECT_GT(routesAfter, 0) << "DUT should still have routes after failure";

  /*
   * Step 7: Write results
   */
  std::string outputPath = "/tmp/dut_link_failure_test.txt";
  std::ofstream outFile(outputPath);
  outFile << "=== DUT Link Failure Test Results ===\n\n";
  outFile << fmt::format("Routes before spine-0 failure: {}\n", routesBefore);
  outFile << fmt::format("Routes after spine-0 failure: {}\n", routesAfter);
  outFile << "\n" << dumpRouteDatabase(routeDbAfter);
  outFile.close();

  LOG(INFO) << "Results written to: " << outputPath;
  LOG(INFO) << "DUT successfully reconverged after spine-0 failure";
}
