/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <benchmark/benchmark.h>
#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/Random.h>
#include <folly/futures/Promise.h>
#include <memory>

#include <openr/common/Constants.h>
#include <openr/decision/Decision.h>
#include <openr/decision/tests/DecisionBenchmark.h>
#include <openr/tests/OpenrModuleTestBase.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace openr {

using apache::thrift::CompactSerializer;
using apache::thrift::FRAGILE;

//
// createAdjDb(...) defined in Util.h uses a default overloadBit = false
//
inline thrift::AdjacencyDatabase
createAdjDb(
    const std::string& nodeName,
    const std::vector<thrift::Adjacency>& adjs,
    int32_t nodeLabel,
    bool overloadBit = false) {
  auto adjDb = thrift::AdjacencyDatabase(
      apache::thrift::FRAGILE,
      nodeName,
      overloadBit,
      adjs,
      nodeLabel,
      thrift::PerfEvents());
  adjDb.perfEvents = folly::none;
  return adjDb;
}

//
// Start the decision thread and simulate KvStore communications
// Expect proper RouteDatabase publications to appear
//
class DecisionWrapper : public OpenrModuleTestBase {
 public:
  DecisionWrapper() {
    kvStorePub.bind(fbzmq::SocketUrl{"inproc://kvStore-pub"});
    kvStoreRep.bind(fbzmq::SocketUrl{"inproc://kvStore-rep"});

    decision = std::make_shared<Decision>(
        "1", /* node name */
        true, /* enable v4 */
        true, /* computeLfaPaths */
        false, /* enableOrderedFib */
        false, /* bgpDryRun */
        AdjacencyDbMarker{"adj:"},
        PrefixDbMarker{"prefix:"},
        std::chrono::milliseconds(10),
        std::chrono::milliseconds(500),
        folly::none,
        KvStoreLocalCmdUrl{"inproc://kvStore-rep"},
        KvStoreLocalPubUrl{"inproc://kvStore-pub"},
        DecisionPubUrl{"inproc://decision-pub"},
        MonitorSubmitUrl{"inproc://monitor-rep"},
        zeromqContext);

    decisionThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Decision thread starting";
      decision->run();
      LOG(INFO) << "Decision thread finishing";
    });
    decision->waitUntilRunning();

    // put handler into moduleToEvl to make sure openr-ctrl thrift handler
    // can access Decision module.
    moduleTypeToEvl_[thrift::OpenrModuleType::DECISION] = decision;

    // variables used to create Open/R ctrl thrift handler
    std::unordered_set<std::string> acceptablePeerNames;
    startOpenrCtrlHandler(
        "node-1",
        acceptablePeerNames,
        MonitorSubmitUrl{"inproc://monitor-rep"},
        KvStoreLocalPubUrl{"inproc://kvStore-pub"},
        zeromqContext);

    const int hwm = 1000;
    decisionPub.setSockOpt(ZMQ_RCVHWM, &hwm, sizeof(hwm)).value();
    decisionPub.setSockOpt(ZMQ_SUBSCRIBE, "", 0).value();
    decisionPub.connect(fbzmq::SocketUrl{"inproc://decision-pub"});

    // Make initial sync request with empty route-db
    replyInitialSyncReq(thrift::Publication());
    // Make request from decision to ensure that sockets are ready for use!
    dumpRouteDb({"random-node"});
  }

  ~DecisionWrapper() {
    LOG(INFO) << "Stopping openr-ctrl thrift server";
    stopOpenrCtrlHandler();
    LOG(INFO) << "Openr-ctrl thrift server got stopped";

    LOG(INFO) << "Stopping the decision thread";
    decision->stop();
    decisionThread->join();
    LOG(INFO) << "Decision thread got stopped";
  }

  //
  // member methods
  //

  thrift::RouteDatabase
  recvMyRouteDb() {
    auto maybeRouteDb =
        decisionPub.recvThriftObj<thrift::RouteDatabase>(serializer);
    auto routeDb = maybeRouteDb.value();
    return routeDb;
  }

  // helper function
  thrift::Value
  createAdjValue(
      const std::string& nodeId,
      int64_t version,
      const std::vector<thrift::Adjacency>& adjs,
      const folly::Optional<thrift::PerfEvents>& perfEvents,
      bool overloadBit = false) {
    auto adjDb = createAdjDb(nodeId, adjs, 0, overloadBit);
    if (perfEvents.hasValue()) {
      adjDb.perfEvents = perfEvents;
    }
    return thrift::Value(
        FRAGILE,
        version,
        "originator-1",
        fbzmq::util::writeThriftObjStr(adjDb, serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  thrift::Value
  createPrefixValue(
      const std::string& nodeId,
      int64_t version,
      const std::vector<thrift::IpPrefix>& prefixes) {
    std::vector<thrift::PrefixEntry> prefixEntries;
    for (const auto& prefix : prefixes) {
      prefixEntries.emplace_back(createPrefixEntry(prefix));
    }
    return thrift::Value(
        FRAGILE,
        version,
        "originator-1",
        fbzmq::util::writeThriftObjStr(
            createPrefixDb(nodeId, prefixEntries), serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  // publish routeDb
  void
  sendKvPublication(const thrift::Publication& publication) {
    kvStorePub.sendThriftObj(publication, serializer);
  }

 private:
  //
  // private member methods
  //

  std::unordered_map<std::string, thrift::RouteDatabase>
  dumpRouteDb(const std::vector<std::string>& allNodes) {
    std::unordered_map<std::string, thrift::RouteDatabase> routeMap;

    for (std::string const& node : allNodes) {
      auto resp = openrCtrlHandler_
                      ->semifuture_getRouteDbComputed(
                          std::make_unique<std::string>(node))
                      .get();
      routeMap[node] = *resp;
      VLOG(4) << "---";
    }

    return routeMap;
  }

  void
  replyInitialSyncReq(const thrift::Publication& publication) {
    // receive the request for initial routeDb sync
    auto maybeDumpReq =
        kvStoreRep.recvThriftObj<thrift::KvStoreRequest>(serializer);
    auto dumpReq = maybeDumpReq.value();

    // send back routeDb reply
    kvStoreRep.sendThriftObj(publication, serializer);
  }

  //
  // private member variables
  //

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  CompactSerializer serializer{};

  // ZMQ context for IO processing
  fbzmq::Context zeromqContext{};

  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> kvStorePub{zeromqContext};
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> kvStoreRep{zeromqContext};
  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> decisionPub{zeromqContext};
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> decisionReq{zeromqContext};

  // KvStore owned by this wrapper.
  std::shared_ptr<Decision> decision{nullptr};

  // Thread in which KvStore will be running.
  std::unique_ptr<std::thread> decisionThread{nullptr};
};

// Convert an integer to hex
inline std::string
toHex(const int num) {
  return folly::sformat("{:02x}", num);
}

// Convert an integer to prefix IPv6
inline std::string
nodeToPrefixV6(const uint32_t nodeId) {
  return folly::sformat(
      "fc00:{}::{}/128", toHex(nodeId >> 16), toHex(nodeId & 0xffff));
}

// Accumulate the time extracted from perfevent
void
accumulatePerfTimes(
    const thrift::PerfEvents& perfEvents, std::vector<uint64_t>& processTimes) {
  // The size of perfEvents.events should = processTimes.size() + 1
  CHECK_EQ(perfEvents.events.size(), processTimes.size() + 1);

  // Accumulate time into processTimes
  for (auto index = 1; index < perfEvents.events.size(); index++) {
    processTimes[index - 1] +=
        (perfEvents.events[index].unixTs - perfEvents.events[index - 1].unixTs);
  }
}

// Send adjacencies update to decision and receive routes
void
sendRecvUpdate(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    thrift::Publication& newPub,
    const std::string& nodeName,
    const std::vector<thrift::Adjacency>& adjs,
    std::vector<uint64_t>& processTimes,
    bool overloadBit = false) {
  // Add perfevent
  thrift::PerfEvents perfEvents;
  addPerfEvent(perfEvents, nodeName, "DECISION_INIT_UPDATE");

  // Add adjs to publication
  newPub.keyVals[folly::sformat("adj:{}", nodeName)] =
      decisionWrapper->createAdjValue(
          nodeName, 2, adjs, std::move(perfEvents), overloadBit);

  LOG(INFO) << "Advertising adj update";
  decisionWrapper->sendKvPublication(newPub);

  // Receive route update from Decision
  auto routes2 = decisionWrapper->recvMyRouteDb();

  // Extract time from perfevent and accumulate processing time
  if (routes2.perfEvents.hasValue()) {
    accumulatePerfTimes(routes2.perfEvents.value(), processTimes);
  }
}

// Add an adjacency to node
inline void
createAdjacency(
    const uint32_t nodeId,
    const std::string& ifName,
    std::vector<thrift::Adjacency>& adjs,
    const std::string& otherIfName) {
  adjs.emplace_back(thrift::Adjacency(
      FRAGILE,
      folly::sformat("{}", nodeId),
      ifName,
      toBinaryAddress(folly::IPAddress(folly::sformat(
          "fe80:{}::{}", toHex(nodeId >> 16), toHex(nodeId & 0xffff)))),
      toBinaryAddress(folly::IPAddress(folly::sformat(
          "10.{}.{}.{}", nodeId >> 16, (nodeId >> 8) & 0xff, nodeId & 0xff))),
      1,
      100001 + nodeId /* adjacency-label */,
      false /* overload-bit */,
      100,
      10000 /* timestamp */,
      1 /* weight */,
      otherIfName));
}

// Add one adjacency to node at grid(row, col)
inline void
createGridAdjacency(
    const int row,
    const int col,
    const std::string& ifName,
    std::vector<thrift::Adjacency>& adjs,
    const int n,
    const std::string& otherIfName) {
  if (row < 0 || row >= n || col < 0 || col >= n) {
    return;
  }

  auto nodeId = row * n + col;
  createAdjacency(nodeId, ifName, adjs, otherIfName);
}

// Get ifName
std::string
getIfName(const uint32_t id, const uint32_t otherId) {
  // Naming convention of ifName: "if_<my-id>_<neighbor-id>"
  return folly::sformat("if_{}_{}", id, otherId);
}

// Add all adjacencies to node at (row, col)
inline std::vector<thrift::Adjacency>
createGridAdjacencys(const int row, const int col, const uint32_t n) {
  std::vector<thrift::Adjacency> adjs;
  auto nodeId = row * n + col;
  auto otherId = row * n + col + 1;
  createGridAdjacency(
      row,
      col + 1,
      getIfName(nodeId, otherId),
      adjs,
      n,
      getIfName(otherId, nodeId));

  otherId = row * n + col - 1;
  createGridAdjacency(
      row,
      col - 1,
      getIfName(nodeId, otherId),
      adjs,
      n,
      getIfName(otherId, nodeId));

  otherId = (row - 1) * n + col;
  createGridAdjacency(
      row - 1,
      col,
      getIfName(nodeId, otherId),
      adjs,
      n,
      getIfName(otherId, nodeId));

  otherId = (row + 1) * n + col;
  createGridAdjacency(
      row + 1,
      col,
      getIfName(nodeId, otherId),
      adjs,
      n,
      getIfName(otherId, nodeId));
  return adjs;
}

// Create a grid topology
thrift::Publication
createGrid(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper, const int n) {
  LOG(INFO) << "grid: " << n << " by " << n;
  thrift::Publication initialPub;

  // Grid topology
  for (auto row = 0; row < n; ++row) {
    for (auto col = 0; col < n; ++col) {
      auto nodeId = row * n + col;
      auto nodeName = folly::sformat("{}", nodeId);
      // Add adjs
      auto adjs = createGridAdjacencys(row, col, n);
      initialPub.keyVals.emplace(
          folly::sformat("adj:{}", nodeName),
          decisionWrapper->createAdjValue(nodeName, 1, adjs, folly::none));

      // prefix
      auto addrV6 = toIpPrefix(nodeToPrefixV6(nodeId));
      initialPub.keyVals.emplace(
          folly::sformat("prefix:{}", nodeName),
          decisionWrapper->createPrefixValue(nodeName, 1, {addrV6}));
    }
  }
  return initialPub;
}

//
// Choose a random nodeId for update or revert the last updated nodeId:
// toggle it's overload bit in AdjacencyDb
//
void
updateRandomGridAdjs(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    folly::Optional<std::pair<int, int>>& selectedNode,
    const int n,
    std::vector<uint64_t>& processTimes) {
  thrift::Publication newPub;

  // If there has been an update, revert the update,
  // otherwise, choose a random nodeId for update
  auto row = selectedNode.hasValue() ? selectedNode.value().first
                                     : folly::Random::rand32() % n;
  auto col = selectedNode.hasValue() ? selectedNode.value().second
                                     : folly::Random::rand32() % n;

  auto nodeName = folly::sformat("{}", row * n + col);
  auto adjs = createGridAdjacencys(row, col, n);
  auto overloadBit = selectedNode.hasValue() ? false : true;
  // Record the updated nodeId
  selectedNode = selectedNode.hasValue()
      ? folly::none
      : folly::Optional<std::pair<int, int>>(std::make_pair(row, col));

  // Send the update to decision and receive the routes
  sendRecvUpdate(
      decisionWrapper, newPub, nodeName, adjs, processTimes, overloadBit);
}

//
// Get average processTimes and insert as user counters.
//
void
insertUserCounters(
    benchmark::State& state, std::vector<uint64_t>& processTimes) {
  // Get average time of each itaration
  for (auto& processTime : processTimes) {
    processTime /= state.iterations() == 0 ? 1 : state.iterations();
  }

  // Add customized counters to state.
  state.counters.insert(
      {{"adj_receive", processTimes[0]}, {"spf", processTimes[2]}});
}

//
// Benchmark test for grid topology
//
void
BM_DecisionGrid(benchmark::State& state) {
  auto decisionWrapper = std::make_shared<DecisionWrapper>();
  int n = std::sqrt(state.range(0));
  auto initialPub = createGrid(decisionWrapper, n);

  //
  // Publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //
  decisionWrapper->sendKvPublication(initialPub);

  // Receive RouteUpdate from Decision
  decisionWrapper->recvMyRouteDb();

  // Record the updated nodeId
  folly::Optional<std::pair<int, int>> selectedNode = folly::none;
  //
  // Customized time counter
  // processTimes[0] is the time of sending adjDB from Kvstore (simulated) to
  // Decision, processTimes[1] is the time of debounce, and processTimes[2] is
  // the time of spf solver
  //
  std::vector<uint64_t> processTimes{0, 0, 0};

  for (auto _ : state) {
    // Advertise adj update. This should trigger the SPF run.
    updateRandomGridAdjs(decisionWrapper, selectedNode, n, processTimes);
  }

  // Insert processTimes as user counters
  insertUserCounters(state, processTimes);
}

} // namespace openr
