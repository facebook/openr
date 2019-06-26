/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <benchmark/benchmark.h>
#include <fbzmq/async/StopEventLoopSignalHandler.h>
#include <openr/fib/Fib.h>
#include <openr/fib/tests/MockNetlinkFibHandler.h>
#include <openr/fib/tests/PrefixGenerator.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>
#include <thread>

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

using apache::thrift::CompactSerializer;
using apache::thrift::FRAGILE;
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

    // Create sockets
    decisionPub.bind(fbzmq::SocketUrl{"inproc://decision-pub"});
    decisionRep.bind(fbzmq::SocketUrl{"inproc://decision-cmd"}).value();
    lmPub.bind(fbzmq::SocketUrl{"inproc://lm-pub"}).value();
    fibReq.connect(fbzmq::SocketUrl{"inproc://fib-cmd"}).value();

    // Creat Fib module and start fib thread
    port = fibThriftThread.getAddress()->getPort();
    fib = std::make_shared<Fib>(
        "node-1",
        port, // thrift port
        false, // dryrun
        true, // periodic syncFib
        false, // segment route
        false, // orderedFib
        std::chrono::seconds(2),
        false, // waitOnDecision
        DecisionPubUrl{"inproc://decision-pub"},
        std::string{"inproc://fib-cmd"},
        LinkMonitorGlobalPubUrl{"inproc://lm-pub"},
        MonitorSubmitUrl{"inproc://monitor-sub"},
        KvStoreLocalCmdUrl{"inproc://kvstore-cmd"},
        KvStoreLocalPubUrl{"inproc://kvstore-sub"},
        context);

    fibThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Fib thread starting";
      fib->run();
      LOG(INFO) << "Fib thread finishing";
    });
    fib->waitUntilRunning();
  }

  ~FibWrapper() {
    // This will be invoked before Fib's d-tor
    fib->stop();
    fibThread->join();

    // Close socket
    decisionPub.close();
    decisionRep.close();
    lmPub.close();

    // Stop mocked nl platform
    mockFibHandler->stop();
    fibThriftThread.stop();
  }

  thrift::PerfDatabase
  getPerfDb() {
    // Send a fib request to get perfDB
    fibReq.sendThriftObj(
        thrift::FibRequest(FRAGILE, thrift::FibCommand::PERF_DB_GET),
        serializer);

    // Receive reply
    auto maybeReply = fibReq.recvThriftObj<thrift::PerfDatabase>(serializer);
    if (maybeReply.hasError()) {
      LOG(ERROR) << "Error::maybeReply.hasError()";
    }
    const auto& perfDb = maybeReply.value();
    return perfDb;
  }

  void
  accumulatePerfTimes(std::vector<uint64_t>& processTimes) {
    // Get perfDB
    auto perfDB = getPerfDb();
    // If get empty perfDB, just log it
    if (perfDB.eventInfo.size() == 0 or
        perfDB.eventInfo[0].events.size() == 0) {
      LOG(INFO) << "perfDB is emtpy.";
    } else {
      // Accumulate time into processTimes
      // Each time get the latest perf event.
      auto perfDBInfoSize = perfDB.eventInfo.size();
      auto eventInfo = perfDB.eventInfo[perfDBInfoSize - 1];
      for (auto index = 1; index < eventInfo.events.size(); index++) {
        processTimes[index - 1] +=
            (eventInfo.events[index].unixTs -
             eventInfo.events[index - 1].unixTs);
      }
    }
  }

  int port{0};
  std::shared_ptr<ThriftServer> server;
  ScopedServerThread fibThriftThread;

  fbzmq::Context context{};
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> decisionPub{context};
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> decisionRep{context};
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> lmPub{context};
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> fibReq{context};

  // Create the serializer for write/read
  apache::thrift::CompactSerializer serializer;

  std::shared_ptr<Fib> fib;
  std::unique_ptr<std::thread> fibThread;

  std::shared_ptr<MockNetlinkFibHandler> mockFibHandler;
  PrefixGenerator prefixGenerator;
};

void
BM_Fib(benchmark::State& state) {
  // Fib starts with clean route database
  auto fibWrapper = std::make_unique<FibWrapper>();

  // Initial syncFib debounce
  fibWrapper->mockFibHandler->waitForSyncFib();

  // Mimic decision pub sock publishing RouteDatabase
  const uint32_t numOfPrefixes = state.range(0);
  // Generate random prefixes
  auto prefixes = fibWrapper->prefixGenerator.ipv6PrefixGenerator(
      numOfPrefixes, kBitMaskLen);
  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = "node-1";
  for (auto& prefix : prefixes) {
    routeDb.unicastRoutes.emplace_back(createUnicastRoute(
        prefix,
        fibWrapper->prefixGenerator.getRandomNextHopsUnicast(
            kNumOfNexthops, kVethNameY)));
  }
  // Send routeDB to Fib and wait for updating completing
  fibWrapper->decisionPub.sendThriftObj(routeDb, fibWrapper->serializer);
  fibWrapper->mockFibHandler->waitForUpdateUnicastRoutes();

  // Customized time counter
  // processTimes[0] is the time of sending routDB from decision to Fib
  // processTimes[1] is the time of processing DB within Fib
  // processTimes[2] is the time of programming routs with Fib agent server
  std::vector<uint64_t> processTimes{0, 0, 0};
  // Maek sure deltaSize <= numOfPrefixes
  auto deltaSize = kDeltaSize <= numOfPrefixes ? kDeltaSize : numOfPrefixes;

  for (auto _ : state) {
    // Update routes by randomly regenerating nextHops for deltaSize prefixes.
    for (auto index = 0; index < deltaSize; index++) {
      routeDb.unicastRoutes.emplace_back(createUnicastRoute(
          prefixes[index],
          fibWrapper->prefixGenerator.getRandomNextHopsUnicast(
              kNumOfNexthops, kVethNameY)));
    }
    // Add perfevents
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, routeDb.thisNodeName, "FIB_INIT_UPDATE");
    routeDb.perfEvents = perfEvents;

    // Send routeDB to Fib for updates
    fibWrapper->decisionPub.sendThriftObj(routeDb, fibWrapper->serializer);
    fibWrapper->mockFibHandler->waitForUpdateUnicastRoutes();

    // Get time information from perf event
    fibWrapper->accumulatePerfTimes(processTimes);
  }

  // Get average time for each itaration
  // To avoid 'division by 0', add 1 to state.iterations()
  for (auto& processTime : processTimes) {
    processTime /= (state.iterations() + 1);
  }

  // Add customized counters to state.
  state.counters.insert({{"DB_Receive", processTimes[0]},
                         {"Fib_Debounce", processTimes[1]},
                         {"DB_Program", processTimes[2]}});
}

} // namespace openr
