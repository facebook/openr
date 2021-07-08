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
#include <openr/config/tests/Utils.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/fib/Fib.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/tests/mocks/MockNetlinkFibHandler.h>
#include <openr/tests/mocks/PrefixGenerator.h>

/**
 * Defines a benchmark that allows users to record customized counter during
 * benchmarking and passes a parameter to another one. This is common for
 * benchmarks that need a "problem size" in addition to "number of iterations".
 */
#define BENCHMARK_COUNTERS_PARAM(name, counters, param) \
  BENCHMARK_COUNTERS_NAME_PARAM(name, counters, param, param)

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
// Updating kDeltaSize routing entries
static const uint32_t kDeltaSize = 10;
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

    // instantiate openrCtrlHandler to invoke fib API
    handler = std::make_shared<OpenrCtrlHandler>(
        "node-1",
        std::unordered_set<std::string>{} /* acceptable peers */,
        &evb,
        nullptr /* decision */,
        fib.get() /* fib */,
        nullptr /* kvStore */,
        nullptr /* linkMonitor */,
        nullptr /* monitor */,
        nullptr /* configStore */,
        nullptr /* prefixManager */,
        nullptr /* spark */,
        config /* config */);

    evbThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Starting ctrlEvb";
      evb.run();
      LOG(INFO) << "ctrlEvb finished";
    });
    evb.waitUntilRunning();
  }

  ~FibWrapper() {
    LOG(INFO) << "Closing queues";
    fibRouteUpdatesQueue.close();
    routeUpdatesQueue.close();
    staticRouteUpdatesQueue.close();
    logSampleQueue.close();

    LOG(INFO) << "Stopping openr-ctrl handler";
    handler.reset();
    evb.stop();
    evb.waitUntilStopped();
    evbThread->join();

    // This will be invoked before Fib's d-tor
    fib->stop();
    fibThread->join();

    // Stop mocked nl platform
    mockFibHandler->stop();
    fibThriftThread.stop();
  }

  thrift::PerfDatabase
  getPerfDb() {
    thrift::PerfDatabase perfDb;
    auto resp = handler->semifuture_getPerfDb().get();
    EXPECT_TRUE(resp);

    perfDb = *resp;
    return perfDb;
  }

  void
  accumulatePerfTimes(std::vector<uint64_t>& processTimes) {
    // Get perfDB
    auto perfDB = getPerfDb();
    // If get empty perfDB, just log it
    if (perfDB.eventInfo_ref()->size() == 0 or
        perfDB.eventInfo_ref()[0].events_ref()->size() == 0) {
      LOG(INFO) << "perfDB is emtpy.";
    } else {
      // Accumulate time into processTimes
      // Each time get the latest perf event.
      auto perfDBInfoSize = perfDB.eventInfo_ref()->size();
      auto eventInfo = perfDB.eventInfo_ref()[perfDBInfoSize - 1];
      for (size_t index = 1; index < eventInfo.events_ref()->size(); index++) {
        processTimes[index - 1] +=
            (*eventInfo.events_ref()[index].unixTs_ref() -
             *eventInfo.events_ref()[index - 1].unixTs_ref());
      }
    }
  }

  std::shared_ptr<ThriftServer> server;
  ScopedServerThread fibThriftThread;

  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue;
  messaging::ReplicateQueue<LogSample> logSampleQueue;

  // ctrlEvb for openrCtrlHandler instantiation
  OpenrEventBase evb;
  std::unique_ptr<std::thread> evbThread;

  std::shared_ptr<Config> config;
  std::shared_ptr<Fib> fib;
  std::unique_ptr<std::thread> fibThread;

  std::shared_ptr<MockNetlinkFibHandler> mockFibHandler{nullptr};
  std::shared_ptr<OpenrCtrlHandler> handler{nullptr};
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
BM_Fib(folly::UserCounters& counters, uint32_t iters, unsigned numOfPrefixes) {
  auto suspender = folly::BenchmarkSuspender();
  // Fib starts with clean route database
  auto fibWrapper = std::make_unique<FibWrapper>();

  // Initial syncFib debounce
  fibWrapper->mockFibHandler->waitForSyncFib();

  // Generate random prefixes
  auto prefixes = fibWrapper->prefixGenerator.ipv6PrefixGenerator(
      numOfPrefixes, kBitMaskLen);
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

  // Customized time counter
  // processTimes[0] is the time of sending routDB from decision to Fib
  // processTimes[1] is the time of processing DB within Fib
  // processTimes[2] is the time of programming routs with Fib agent server
  std::vector<uint64_t> processTimes{0, 0, 0};
  // Maek sure deltaSize <= numOfPrefixes
  auto deltaSize = kDeltaSize <= numOfPrefixes ? kDeltaSize : numOfPrefixes;
  suspender.dismiss(); // Start measuring benchmark time

  for (uint32_t i = 0; i < iters; i++) {
    // Update routes by randomly regenerating nextHops for deltaSize prefixes.
    DecisionRouteUpdate routeUpdate;
    for (uint32_t index = 0; index < deltaSize; index++) {
      auto nhs = fibWrapper->prefixGenerator.getRandomNextHopsUnicast(
          kNumOfNexthops, kVethNameY);
      auto nhsSet =
          std::unordered_set<thrift::NextHopThrift>(nhs.begin(), nhs.end());
      routeUpdate.unicastRoutesToUpdate.emplace(
          toIPNetwork(prefixes[index]),
          RibUnicastEntry(toIPNetwork(prefixes[index]), nhsSet));
    }
    // Add perfevents
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, "node-1", "FIB_INIT_UPDATE");
    routeUpdate.perfEvents = perfEvents;

    // Send routeDB to Fib for updates
    fibWrapper->routeUpdatesQueue.push(std::move(routeUpdate));
    fibWrapper->mockFibHandler->waitForUpdateUnicastRoutes();

    // Get time information from perf event
    fibWrapper->accumulatePerfTimes(processTimes);
  }

  suspender.rehire(); // Stop measuring time again
  // Get average time for each itaration
  for (auto& processTime : processTimes) {
    processTime /= iters == 0 ? 1 : iters;
  }

  // Add customized counters to state.
  counters["route_receive"] = processTimes[0];
  counters["route_install"] = processTimes[2];
}

// The parameter is the number of prefixes sent to fib
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 10);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 100);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 1000);
BENCHMARK_COUNTERS_PARAM(BM_Fib, counters, 9000);

} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
