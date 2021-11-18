/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/Benchmark.h>
#include <folly/init/Init.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

#include <openr/config/Config.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/fib/Fib.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/SystemMetrics.h>
#include <openr/tests/mocks/MockNetlinkFibHandler.h>
#include <openr/tests/mocks/PrefixGenerator.h>
#include <openr/tests/utils/Utils.h>

/**
 * Defines a benchmark that allows users to record customized counter during
 * benchmarking and passes a parameter to another one. This is common for
 * benchmarks that need a "problem size" in addition to "number of iterations".
 */
#define BENCHMARK_COUNTERS_PARAM(name, counters, existing, update) \
  BENCHMARK_COUNTERS_NAME_PARAM(                                   \
      name,                                                        \
      counters,                                                    \
      FB_CONCATENATE(existing, FB_CONCATENATE(_, update)),         \
      existing,                                                    \
      update)

/*
 * Like BENCHMARK_COUNTERS_PARAM(), but allows a custom name to be specified for
 * each parameter, rather than using the parameter value.
 */
#define BENCHMARK_COUNTERS_NAME_PARAM(name, counters, param_name, ...) \
  BENCHMARK_IMPL_COUNTERS(                                             \
      FB_CONCATENATE(name, FB_CONCATENATE(_, param_name)),             \
      FOLLY_PP_STRINGIZE(name) "(" FOLLY_PP_STRINGIZE(param_name) ")", \
      counters,                                                        \
      iters,                                                           \
      unsigned,                                                        \
      iters) {                                                         \
    name(counters, iters, ##__VA_ARGS__);                              \
  }

namespace {
// Virtual interface
const std::string kVethNameY("vethTestY");
// Prefix length of a subnet
static const long kBitMaskLen = 128;

// Number of nexthops
const uint8_t kNumOfNexthops = 128;

} // anonymous namespace

namespace openr {

using apache::thrift::ThriftServer;
using apache::thrift::util::ScopedServerThread;

class FibWrapper {
 public:
  FibWrapper() {
    // Register Singleton
    folly::SingletonVault::singleton()->registrationComplete();
    // Create MockNetlinkFibHandler
    mockFibHandler = std::make_shared<MockNetlinkFibHandler>();

    // Start ThriftServer
    server = std::make_shared<ThriftServer>();
    server->setNumIOWorkerThreads(1);
    server->setNumAcceptThreads(1);
    server->setPort(0);
    server->setInterface(mockFibHandler);
    fibThriftThread.start(server);

    auto tConfig = getBasicOpenrConfig(
        "node-1",
        "domain",
        {}, /* area config */
        true, /* enableV4 */
        false /*enableSegmentRouting*/,
        false /*orderedFibProgramming*/,
        false /*dryrun*/);
    tConfig.fib_port_ref() = fibThriftThread.getAddress()->getPort();
    config = std::make_shared<Config>(tConfig);

    // Creat Fib module and start fib thread
    fib = std::make_shared<Fib>(
        config,
        routeUpdatesQueue.getReader(),
        staticRouteUpdatesQueue.getReader(),
        fibRouteUpdatesQueue,
        logSampleQueue);

    fibThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Fib thread starting";
      fib->run();
      LOG(INFO) << "Fib thread finishing";
    });
    fib->waitUntilRunning();
  }

  ~FibWrapper() {
    LOG(INFO) << "Closing queues";
    fibRouteUpdatesQueue.close();
    routeUpdatesQueue.close();
    staticRouteUpdatesQueue.close();
    logSampleQueue.close();

    // This will be invoked before Fib's d-tor
    fib->stop();
    fibThread->join();

    // Stop mocked nl platform
    mockFibHandler->stop();
    fibThriftThread.stop();
  }

  std::shared_ptr<ThriftServer> server;
  ScopedServerThread fibThriftThread;

  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
  messaging::ReplicateQueue<LogSample> logSampleQueue;

  std::shared_ptr<Config> config;
  std::shared_ptr<Fib> fib;
  std::unique_ptr<std::thread> fibThread;

  std::shared_ptr<MockNetlinkFibHandler> mockFibHandler{nullptr};
  PrefixGenerator prefixGenerator;
};

/**
 * Benchmark for fib
 * 1. Create a fib
 * 2. Generate random IpV6s and routes
 * 3. Send routes to fib
 * 4. Wait until the completion of routes update
 */
static void
BM_Fib(
    folly::UserCounters& counters,
    uint32_t iters,
    unsigned numOfRoutes,
    unsigned numOfUpdateRoutes) {
  auto suspender = folly::BenchmarkSuspender();
  // Add boolean to control profiling memory for the 1st iteration
  SystemMetrics sysMetrics;
  bool record = true;
  for (uint32_t i = 0; i < iters; i++) {
    // Fib starts with clean route database
    auto fibWrapper = std::make_unique<FibWrapper>();

    // Initial syncFib debounce
    fibWrapper->routeUpdatesQueue.push(DecisionRouteUpdate());
    fibWrapper->mockFibHandler->waitForSyncFib();

    // Generate random `numOfRoutes` prefixes
    auto prefixes = fibWrapper->prefixGenerator.ipv6PrefixGenerator(
        numOfRoutes, kBitMaskLen);
    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory before adding existing routes(MB)"] =
            mem.value() / 1024 / 1024;
      }
    }
    {
      DecisionRouteUpdate routeUpdate;
      for (auto& prefix : prefixes) {
        auto nhs = fibWrapper->prefixGenerator.getRandomNextHopsUnicast(
            kNumOfNexthops, kVethNameY);
        auto nhsSet =
            std::unordered_set<thrift::NextHopThrift>(nhs.begin(), nhs.end());
        routeUpdate.unicastRoutesToUpdate.emplace(
            toIPNetwork(prefix), RibUnicastEntry(toIPNetwork(prefix), nhsSet));
      }
      // Send routeDB to Fib and wait for updating completing
      fibWrapper->routeUpdatesQueue.push(std::move(routeUpdate));
    }
    fibWrapper->mockFibHandler->waitForUpdateUnicastRoutes();
    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory after adding existing routes(MB)"] =
            mem.value() / 1024 / 1024;
      }
      record = false;
    }

    {
      // Update routes by randomly regenerating nextHops for numOfUpdatePrefixes
      // prefixes.
      DecisionRouteUpdate routeUpdate;
      for (uint32_t index = 0; index < numOfUpdateRoutes; index++) {
        auto nhs = fibWrapper->prefixGenerator.getRandomNextHopsUnicast(
            kNumOfNexthops, kVethNameY);
        auto nhsSet =
            std::unordered_set<thrift::NextHopThrift>(nhs.begin(), nhs.end());
        routeUpdate.unicastRoutesToUpdate.emplace(
            toIPNetwork(prefixes[index]),
            RibUnicastEntry(toIPNetwork(prefixes[index]), nhsSet));
      }

      suspender.dismiss(); // Start measuring benchmark time
      // Send routeDB to Fib for updates
      fibWrapper->routeUpdatesQueue.push(std::move(routeUpdate));
      fibWrapper->mockFibHandler->waitForUpdateUnicastRoutes();
      suspender.rehire(); // Stop measuring time again
    }
  }
}

/**
 * Benchmark for fib delete unicast route
 * 1. Create a fib
 * 2. Generate random IpV6s and routes
 * 3. Send routes to fib
 * 4. Wait until the completion of routes update
 * 5. Create a DecisionRouteUpdate for routes-to-delete
 * 6. Wait until the completion of routes update
 */
static void
BM_FibDeleteUnicastRoute(
    folly::UserCounters& counters,
    uint32_t iters,
    unsigned numOfRoutes,
    unsigned numOfDeleteRoutes) {
  auto suspender = folly::BenchmarkSuspender();
  SystemMetrics sysMetrics;
  // Add boolean to control profiling memory for the 1st iteration
  bool record = true;
  for (uint32_t i = 0; i < iters; i++) {
    // Fib starts with clean route database
    auto fibWrapper = std::make_unique<FibWrapper>();

    // Initial syncFib debounce
    fibWrapper->routeUpdatesQueue.push(DecisionRouteUpdate());
    fibWrapper->mockFibHandler->waitForSyncFib();

    // Generate random `numOfRoutes` prefixes
    auto prefixes = fibWrapper->prefixGenerator.ipv6PrefixGenerator(
        numOfRoutes, kBitMaskLen);

    {
      DecisionRouteUpdate routeUpdate;
      for (auto& prefix : prefixes) {
        auto nhs = fibWrapper->prefixGenerator.getRandomNextHopsUnicast(
            kNumOfNexthops, kVethNameY);
        auto nhsSet =
            std::unordered_set<thrift::NextHopThrift>(nhs.begin(), nhs.end());
        routeUpdate.unicastRoutesToUpdate.emplace(
            toIPNetwork(prefix), RibUnicastEntry(toIPNetwork(prefix), nhsSet));
      }
      // Send routeDB to Fib and wait for updating completing
      fibWrapper->routeUpdatesQueue.push(std::move(routeUpdate));
    }
    fibWrapper->mockFibHandler->waitForUpdateUnicastRoutes();

    // Profile memory before the routeUpdate is generated
    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory before deleting routes(MB)"] =
            mem.value() / 1024 / 1024;
      }
    }

    auto prefixToDelete = prefixes;
    prefixToDelete.resize(numOfDeleteRoutes);
    {
      // Delete routes
      DecisionRouteUpdate routeUpdate;
      for (auto& prefix : prefixToDelete) {
        routeUpdate.unicastRoutesToDelete.emplace_back(toIPNetwork(prefix));
      }

      suspender.dismiss(); // Start measuring benchmark time
      // Send routeDB to Fib for updates
      fibWrapper->routeUpdatesQueue.push(std::move(routeUpdate));
      fibWrapper->mockFibHandler->waitForDeleteUnicastRoutes();
      suspender.rehire(); // Stop measuring time again
    }

    // Profile memory after the routeUpdate is handled by Fib
    if (record) {
      auto mem = sysMetrics.getVirtualMemBytes();
      if (mem.has_value()) {
        counters["memory after deleting routes(MB)"] =
            mem.value() / 1024 / 1024;
      }
      record = false;
    }
  }
}

/*
 * @params counters: reserved counter for customized profile
 * @params first integer: num of existing routes
 * @params second integer: num of updating routes
 */
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 100, 100);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 1000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 10000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 100000, 1);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 100000, 10);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 100000, 100);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 100000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 100000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 100000, 100000);

/*
 * @params counters: reserved counter for customized profile
 * @params first integer: num of existing routes
 * @params second integer: num of deleting routes
 */
BENCHMARK_COUNTERS_PARAM(BM_FibDeleteUnicastRoute, counters, 100, 100);
BENCHMARK_COUNTERS_PARAM(BM_FibDeleteUnicastRoute, counters, 1000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_FibDeleteUnicastRoute, counters, 10000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_FibDeleteUnicastRoute, counters, 100000, 1);
BENCHMARK_COUNTERS_PARAM(BM_FibDeleteUnicastRoute, counters, 100000, 10);
BENCHMARK_COUNTERS_PARAM(BM_FibDeleteUnicastRoute, counters, 100000, 100);
BENCHMARK_COUNTERS_PARAM(BM_FibDeleteUnicastRoute, counters, 100000, 1000);
BENCHMARK_COUNTERS_PARAM(BM_FibDeleteUnicastRoute, counters, 100000, 10000);
BENCHMARK_COUNTERS_PARAM(BM_FibDeleteUnicastRoute, counters, 100000, 100000);

} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
