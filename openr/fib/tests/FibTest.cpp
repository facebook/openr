/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MockNetlinkFibHandler.h"

#include <chrono>
#include <thread>

#include <fbzmq/async/StopEventLoopSignalHandler.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/Memory.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <folly/gen/Base.h>
#include <folly/init/Init.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp/util/ScopedServerThread.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/BinaryProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/AddressUtil.h>
#include <openr/fib/Fib.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/IpPrefix_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>

using namespace std;
using namespace openr;

using ::testing::InSequence;
using ::testing::InvokeWithoutArgs;
using ::testing::_;
using apache::thrift::CompactSerializer;
using apache::thrift::FRAGILE;
using apache::thrift::ThriftServer;
using apache::thrift::util::ScopedServerThread;

const int16_t kFibId{static_cast<int16_t>(thrift::FibClient::OPENR)};

const auto prefix1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto prefix2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto prefix3 = toIpPrefix("::ffff:10.3.3.3/128");
const auto prefix4 = toIpPrefix("::ffff:10.4.4.4/128");

const auto path1_2_1 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::2")), "iface_1_2_1", 1);
const auto path1_2_2 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::2")), "iface_1_2_2", 2);
const auto path1_3_1 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::3")), "iface_1_3_1", 2);
const auto path1_3_2 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::3")), "iface_1_3_2", 2);
const auto path3_2_1 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::2")), "iface_3_2_1", 1);
const auto path3_2_2 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::2")), "iface_3_2_2", 2);
const auto path3_4_1 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::4")), "iface_3_4_1", 2);
const auto path3_4_2 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::4")), "iface_3_4_2", 2);

class FibTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    mockFibHandler = std::make_shared<MockNetlinkFibHandler>();

    server = make_shared<ThriftServer>();
    server->setPort(0);
    server->setInterface(mockFibHandler);

    fibThriftThread.start(server);
    port = fibThriftThread.getAddress()->getPort();

    EXPECT_NO_THROW(
        decisionPub.bind(fbzmq::SocketUrl{"inproc://decision-pub"}).value());
    EXPECT_NO_THROW(
        decisionRep.bind(fbzmq::SocketUrl{"inproc://decision-cmd"}).value());
    EXPECT_NO_THROW(lmPub.bind(fbzmq::SocketUrl{"inproc://lm-pub"}).value());

    fib = std::make_shared<Fib>(
        "node-1",
        port, /* thrift port */
        false, /* dryrun */
        std::chrono::seconds(2),
        DecisionPubUrl{"inproc://decision-pub"},
        FibCmdUrl{"inproc://fib-cmd"},
        LinkMonitorGlobalPubUrl{"inproc://lm-pub"},
        MonitorSubmitUrl{"inproc://monitor-sub"},
        context);

    fibThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Fib thread starting";
      fib->run();
      LOG(INFO) << "Fib thread finishing";
    });
    fib->waitUntilRunning();
  }

  void
  TearDown() override {
    // this will be invoked before Fib's d-tor
    LOG(INFO) << "Stopping the Fib thread";
    fib->stop();
    fibThread->join();

    decisionPub.close();
    decisionRep.close();
    lmPub.close();

    // stop mocked nl platform
    mockFibHandler->stop();
    fibThriftThread.stop();
    LOG(INFO) << "Mock fib platform is stopped";
  }

  int port{0};
  std::shared_ptr<ThriftServer> server;
  ScopedServerThread fibThriftThread;

  fbzmq::Context context{};
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> decisionPub{context};
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> decisionRep{context};
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> lmPub{context};

  // Create the serializer for write/read
  apache::thrift::CompactSerializer serializer;

  std::shared_ptr<Fib> fib;
  std::unique_ptr<std::thread> fibThread;

  std::shared_ptr<MockNetlinkFibHandler> mockFibHandler;
};

TEST_F(FibTestFixture, processRouteDb) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);

  int64_t countSync = mockFibHandler->getFibSyncCount();

  // Mimic decision pub sock publishing RouteDatabase
  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = "node-1";
  routeDb.routes.emplace_back(
      thrift::Route(FRAGILE, prefix2, {path1_2_1, path1_2_2}));
  decisionPub.sendThriftObj(routeDb, serializer).value();

  // syncFib debounce
  while (mockFibHandler->getFibSyncCount() <= countSync) {
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);

  // Update routes
  countSync = mockFibHandler->getFibSyncCount();
  routeDb.routes.emplace_back(
      thrift::Route(FRAGILE, prefix3, {path1_3_1, path1_3_2}));
  decisionPub.sendThriftObj(routeDb, serializer).value();

  // syncFib debounce
  while (mockFibHandler->getFibSyncCount() <= countSync) {
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
}

TEST_F(FibTestFixture, failedRouteAdd) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);

  int64_t countSync = mockFibHandler->getFibSyncCount();

  // Mimic decision pub sock publishing RouteDatabase
  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = "node-1";
  routeDb.routes.emplace_back(
      thrift::Route(FRAGILE, prefix1, {path1_2_1, path1_2_2}));
  routeDb.routes.emplace_back(
      thrift::Route(FRAGILE, prefix3, {path1_3_1, path1_3_2}));
  decisionPub.sendThriftObj(routeDb, serializer).value();

  // Force to remove route from fib agent to mimic route programming failure
  mockFibHandler->deleteUnicastRoute(
      kFibId, std::make_unique<thrift::IpPrefix>(prefix3));

  // syncFib debounce
  while (mockFibHandler->getFibSyncCount() <= countSync) {
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 2);
}

TEST_F(FibTestFixture, fibRestart) {
  // Make sure fib starts with clean route database
  std::vector<thrift::UnicastRoute> routes;
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);

  int64_t countSync = mockFibHandler->getFibSyncCount();

  // Mimic decision pub sock publishing RouteDatabase
  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = "node-1";
  routeDb.routes.emplace_back(
      thrift::Route(FRAGILE, prefix1, {path1_2_1, path1_2_2}));

  decisionPub.sendThriftObj(routeDb, serializer).value();

  // syncFib debounce
  while (mockFibHandler->getFibSyncCount() <= countSync) {
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);

  mockFibHandler->restart();
  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 0);

  // syncFib debounce
  while (mockFibHandler->getFibSyncCount() <= 0) {
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  mockFibHandler->getRouteTableByClient(routes, kFibId);
  EXPECT_EQ(routes.size(), 1);
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  testing::InitGoogleMock(&argc, argv);
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();

  auto rc = RUN_ALL_TESTS();

  // Run the tests
  return rc;
}
