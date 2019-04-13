/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>

#include <fbzmq/service/monitor/ZmqMonitor.h>
#include <fbzmq/zmq/Context.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace openr;

class OpenrCtrlFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    const std::unordered_set<std::string> acceptablePeerNames;

    // Create zmq-monitor
    zmqMonitor = std::make_unique<fbzmq::ZmqMonitor>(
        monitorSubmitUrl_, "inproc://monitor_pub_url", context_);
    zmqMonitorThread_ = std::thread([&]() { zmqMonitor->run(); });

    // Create modules
    std::unordered_map<thrift::OpenrModuleType, std::shared_ptr<OpenrEventLoop>>
        moduleTypeToEvl;

    // Create KvStore module
    kvStoreWrapper = std::make_unique<KvStoreWrapper>(
        context_,
        nodeName,
        std::chrono::seconds(1),
        std::chrono::seconds(1),
        std::unordered_map<std::string, thrift::PeerSpec>(),
        folly::none,
        folly::none,
        std::chrono::milliseconds(1),
        true /* enableFloodOptimization */,
        true /* isFloodRoot */);
    kvStoreWrapper->run();
    moduleTypeToEvl[thrift::OpenrModuleType::KVSTORE] =
        kvStoreWrapper->getKvStore();

    // Create Decision module
    decision = std::make_shared<Decision>(
        nodeName, /* node name */
        true, /* enable v4 */
        true, /* computeLfaPaths */
        AdjacencyDbMarker{"adj:"},
        PrefixDbMarker{"prefix:"},
        std::chrono::milliseconds(10),
        std::chrono::milliseconds(500),
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        std::string{"inproc://decision-rep"},
        DecisionPubUrl{decisionPubUrl_},
        monitorSubmitUrl_,
        context_);
    decisionThread_ = std::thread([&]() { decision->run(); });
    moduleTypeToEvl[thrift::OpenrModuleType::DECISION] = decision;

    // Create Fib module
    fib = std::make_shared<Fib>(
        nodeName,
        -1, /* thrift port */
        true, /* dryrun */
        false, /* periodic syncFib */
        true, /* enableSegmentRouting */
        std::chrono::seconds(2),
        DecisionPubUrl{"inproc://decision-pub"},
        std::string{"inproc://fib-cmd"},
        LinkMonitorGlobalPubUrl{"inproc://lm-pub"},
        MonitorSubmitUrl{"inproc://monitor-sub"},
        context_);
    fibThread_ = std::thread([&]() { fib->run(); });
    moduleTypeToEvl[thrift::OpenrModuleType::FIB] = fib;

    // Create open/r handler
    handler = std::make_unique<OpenrCtrlHandler>(
        nodeName,
        acceptablePeerNames,
        moduleTypeToEvl,
        monitorSubmitUrl_,
        context_);
  }

  void
  TearDown() override {
    handler.reset();

    fib->stop();
    fibThread_.join();

    decision->stop();
    decisionThread_.join();

    kvStoreWrapper->stop();

    zmqMonitor->stop();
    zmqMonitorThread_.join();
  }

 private:
  const MonitorSubmitUrl monitorSubmitUrl_{"inproc://monitor-submit-url"};
  const DecisionPubUrl decisionPubUrl_{"inproc://decision-pub"};
  fbzmq::Context context_;
  std::thread zmqMonitorThread_;
  std::thread decisionThread_;
  std::thread fibThread_;

 public:
  const std::string nodeName{"thanos@universe"};
  std::unique_ptr<fbzmq::ZmqMonitor> zmqMonitor;
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper;
  std::shared_ptr<Decision> decision;
  std::shared_ptr<Fib> fib;
  std::unique_ptr<OpenrCtrlHandler> handler;
};

TEST_F(OpenrCtrlFixture, RouteApis) {
  {
    auto ret = handler->semifuture_getRouteDb().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nodeName, ret->thisNodeName);
    EXPECT_EQ(0, ret->unicastRoutes.size());
    EXPECT_EQ(0, ret->mplsRoutes.size());
  }

  {
    auto ret = handler
                   ->semifuture_getRouteDbComputed(
                       std::make_unique<std::string>(nodeName))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nodeName, ret->thisNodeName);
    EXPECT_EQ(0, ret->unicastRoutes.size());
    EXPECT_EQ(0, ret->mplsRoutes.size());
  }

  {
    const std::string testNode("avengers@universe");
    auto ret = handler
                   ->semifuture_getRouteDbComputed(
                       std::make_unique<std::string>(testNode))
                   .get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(testNode, ret->thisNodeName);
    EXPECT_EQ(0, ret->unicastRoutes.size());
    EXPECT_EQ(0, ret->mplsRoutes.size());
  }
}

TEST_F(OpenrCtrlFixture, PerfApis) {
  {
    auto ret = handler->semifuture_getPerfDb().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(nodeName, ret->thisNodeName);
  }
}

TEST_F(OpenrCtrlFixture, DecisionApis) {
  {
    auto ret = handler->semifuture_getDecisionAdjacencyDbs().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->size());
  }

  {
    auto ret = handler->semifuture_getDecisionPrefixDbs().get();
    ASSERT_NE(nullptr, ret);
    EXPECT_EQ(0, ret->size());
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
