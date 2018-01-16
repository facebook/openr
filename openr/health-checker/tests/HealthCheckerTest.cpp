/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>

#include <fbzmq/zmq/Zmq.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/common/AddressUtil.h>
#include <openr/health-checker/HealthChecker.h>

using namespace openr;

namespace {
const auto adj12 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 0);
const auto adj13 =
    createAdjacency("3", "1/3", "3/1", "fe80::3", "192.168.0.3", 10, 0);

const auto adj21 =
    createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 0);
const auto adj23 =
    createAdjacency("3", "2/3", "3/2", "fe80::3", "192.168.0.3", 10, 0);
const auto adj24 =
    createAdjacency("4", "2/4", "4/2", "fe80::4", "192.168.0.4", 10, 0);

const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto addr3 = toIpPrefix("::ffff:10.3.3.3/128");
const auto addr4 = toIpPrefix("::ffff:10.4.4.4/128");

} // namespace
namespace openr {

class HealthCheckerTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    kvStorePub.bind(fbzmq::SocketUrl{"inproc://kvStore-pub"});
    kvStoreRep.bind(fbzmq::SocketUrl{"inproc://kvStore-rep"});
    monitorRep.bind(fbzmq::SocketUrl{"inproc://monitor-rep"});

    healthChecker = std::make_unique<HealthChecker>(
        std::string{"1"}, /* node name */
        thrift::HealthCheckOption::PingNeighborOfNeighbor,
        uint32_t{50}, /* health check pct */
        uint16_t{0}, // make sure it binds to some open port
        std::chrono::seconds{2},
        folly::none, /* maybeIpTos */
        AdjacencyDbMarker{"adj:"},
        PrefixDbMarker{"prefix:"},
        KvStoreLocalCmdUrl{"inproc://kvStore-rep"},
        KvStoreLocalPubUrl{"inproc://kvStore-pub"},
        HealthCheckerCmdUrl{"inproc://healthchecker-rep"},
        MonitorSubmitUrl{"inproc://monitor-rep"},
        zmqContext);

    healthCheckerThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "HealthChecker thread starting";
      healthChecker->run();
      LOG(INFO) << "HealthChecker thread finishing";
    });

    healthChecker->waitUntilRunning();
    LOG(INFO) << "HealthChecker Running";
  }

  void
  TearDown() override {
    LOG(INFO) << "Stopping the healthChecker thread";
    healthChecker->stop();
    healthCheckerThread->join();
  }

  void
  replyInitialSyncReq(const thrift::Publication& publication) {
    // receive the request for initial routeDb sync
    auto maybeDumpReq = kvStoreRep.recvThriftObj<thrift::Request>(serializer);
    EXPECT_FALSE(maybeDumpReq.hasError());
    auto dumpReq = maybeDumpReq.value();
    EXPECT_EQ(thrift::Command::KEY_DUMP, dumpReq.cmd);

    kvStoreRep.sendThriftObj(publication, serializer);
  }

  // this blocks unitl counters are submitted
  fbzmq::CounterMap
  getCounters() {
    VLOG(4) << "Getting Counters";
    fbzmq::Message requestIdMsg, thriftReqMsg;
    const auto ret = monitorRep.recvMultiple(requestIdMsg, thriftReqMsg);
    EXPECT_FALSE(ret.hasError());
    VLOG(4) << "Got Counters";
    const auto maybeReq =
        thriftReqMsg.readThriftObj<fbzmq::thrift::MonitorRequest>(serializer);
    EXPECT_FALSE(maybeReq.hasError());
    auto req = maybeReq.value();
    EXPECT_EQ(req.cmd, fbzmq::thrift::MonitorCommand::SET_COUNTER_VALUES);
    return req.counterSetParams.counters;
  }

  // helper function
  thrift::Value
  createAdjValue(
      const std::string& node,
      int64_t version,
      const std::vector<thrift::Adjacency>& adjs) {
    return thrift::Value(
        apache::thrift::FRAGILE,
        version,
        "originator-1",
        fbzmq::util::writeThriftObjStr(createAdjDb(node, adjs, 0), serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  thrift::Value
  createPrefixValue(
      const std::string& node,
      int64_t version,
      const std::vector<thrift::IpPrefix>& prefixes) {
    std::vector<thrift::PrefixEntry> prefixEntries;
    for (const auto& prefix : prefixes) {
      prefixEntries.emplace_back(
          apache::thrift::FRAGILE, prefix, thrift::PrefixType::LOOPBACK, "");
    }

    thrift::PrefixDatabase prefixDb;
    prefixDb.thisNodeName = node;
    prefixDb.prefixEntries = std::move(prefixEntries);

    return thrift::Value(
        apache::thrift::FRAGILE,
        version,
        "originator-1",
        fbzmq::util::writeThriftObjStr(prefixDb, serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  //
  // member variables
  //

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  apache::thrift::CompactSerializer serializer{};

  // ZMQ context for IO processing
  fbzmq::Context zmqContext{};

  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> kvStorePub{zmqContext};
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> kvStoreRep{zmqContext};
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> monitorRep{zmqContext};

  std::unique_ptr<HealthChecker> healthChecker;

  // Thread in which HealthChecker will be running.
  std::unique_ptr<std::thread> healthCheckerThread{nullptr};
};

TEST_F(HealthCheckerTestFixture, BasicOperation) {
  auto adjPublication = thrift::Publication(
      apache::thrift::FRAGILE,
      {{"adj:1", createAdjValue("1", 1, {adj12})},
       {"adj:2", createAdjValue("2", 1, {adj21})}},
      {});
  auto prefixPublication = thrift::Publication(
      apache::thrift::FRAGILE,
      {{"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {});
  replyInitialSyncReq(adjPublication);
  replyInitialSyncReq(prefixPublication);
  LOG(INFO) << "Sent initial sync";

  const auto counters1 = getCounters();
  EXPECT_EQ(counters1.at("health_checker.nodes_to_ping_size").value, 0);
  EXPECT_EQ(counters1.at("health_checker.nodes_info_size").value, 2);

  auto nextPub = thrift::Publication(
      apache::thrift::FRAGILE,
      {{"adj:1", createAdjValue("1", 2, {adj12, adj13})},
       {"adj:2", createAdjValue("2", 2, {adj21, adj23, adj24})},
       {"prefix:3", createPrefixValue("3", 1, {addr3})},
       {"prefix:4", createPrefixValue("4", 1, {addr4})}},
      {});

  kvStorePub.sendThriftObj(nextPub, serializer);

  const auto counters2 = getCounters();
  EXPECT_EQ(counters2.at("health_checker.nodes_to_ping_size").value, 1);
  EXPECT_EQ(counters2.at("health_checker.nodes_info_size").value, 4);
}

} // namespace openr

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
