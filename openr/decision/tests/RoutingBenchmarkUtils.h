/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Benchmark.h>
#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/Random.h>
#include <folly/futures/Promise.h>
#include <folly/init/Init.h>

#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/decision/Decision.h>
#include <openr/tests/utils/Utils.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

/**
 * Defines a benchmark that allows users to record customized counter during
 * benchmarking and passes a parameter to another one. This is common for
 * benchmarks that need a "problem size" in addition to "number of iterations".
 */
#define BENCHMARK_COUNTERS_PARAM(name, counters, size, forwarding, prefixNum) \
  BENCHMARK_COUNTERS_NAME_PARAM(                                              \
      name,                                                                   \
      counters,                                                               \
      FB_CONCATENATE(FB_CONCATENATE(size, forwarding), prefixNum),            \
      size,                                                                   \
      forwarding,                                                             \
      prefixNum)

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
// We have 24 SSWs per plane as of now and moving towards 36 per plane.
const int kNumOfSswsPerPlane = 36;
const int kNumOfFswsPerPod = 8;
const int kNumOfRswsPerPod = 48;
const uint8_t kSswMarker = 1;
const uint8_t kFswMarker = 2;
const uint8_t kRswMarker = 3;

} // namespace

namespace openr {

using apache::thrift::CompactSerializer;

//
// Start the decision thread and simulate KvStore communications
// Expect proper RouteDatabase publications to appear
//
class DecisionWrapper {
 public:
  explicit DecisionWrapper(const std::string& nodeName) {
    auto tConfig = getBasicOpenrConfig(nodeName);
    // decision config
    tConfig.decision_config_ref()->debounce_min_ms_ref() = 10;
    tConfig.decision_config_ref()->debounce_max_ms_ref() = 500;
    tConfig.decision_config_ref()->enable_bgp_route_programming_ref() = true;
    config = std::make_shared<Config>(tConfig);

    decision = std::make_shared<Decision>(
        config,
        peerUpdatesQueue.getReader(),
        kvStoreUpdatesQueue.getReader(),
        staticRouteUpdatesQueue.getReader(),
        routeUpdatesQueue);

    decisionThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Decision thread starting";
      decision->run();
      LOG(INFO) << "Decision thread finishing";
    });
    decision->waitUntilRunning();
  }

  ~DecisionWrapper() {
    peerUpdatesQueue.close();
    kvStoreUpdatesQueue.close();
    staticRouteUpdatesQueue.close();
    LOG(INFO) << "Stopping the decision thread";
    decision->stop();
    decisionThread->join();
    LOG(INFO) << "Decision thread got stopped";
  }

  //
  // member methods
  //

  DecisionRouteUpdate
  recvMyRouteDb() {
    auto maybeRouteDb = routeUpdatesQueueReader.get();
    auto routeDb = maybeRouteDb.value();
    return routeDb;
  }

  // helper function
  thrift::Value
  createAdjValue(
      const std::string& nodeId,
      int64_t version,
      const std::vector<thrift::Adjacency>& adjs,
      const std::optional<thrift::PerfEvents>& perfEvents,
      bool overloadBit = false) {
    auto adjDb = createAdjDb(nodeId, adjs, 0, overloadBit);
    if (perfEvents.has_value()) {
      adjDb.perfEvents_ref().from_optional(perfEvents);
    }
    return createThriftValue(
        version,
        "originator-1",
        writeThriftObjStr(adjDb, serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  // publish routeDb
  void
  sendKvPublication(const thrift::Publication& publication) {
    kvStoreUpdatesQueue.push(publication);
  }

 private:
  //
  // private member methods
  //

  std::unordered_map<std::string, thrift::RouteDatabase>
  dumpRouteDb(const std::vector<std::string>& allNodes) {
    std::unordered_map<std::string, thrift::RouteDatabase> routeMap;

    for (std::string const& node : allNodes) {
      auto resp = decision->getDecisionRouteDb(node).get();
      routeMap[node] = std::move(*resp);
    }

    return routeMap;
  }

  //
  // private member variables
  //

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  CompactSerializer serializer{};

  std::shared_ptr<Config> config;
  messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue;
  messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  messaging::RQueue<DecisionRouteUpdate> routeUpdatesQueueReader{
      routeUpdatesQueue.getReader()};

  // KvStore owned by this wrapper.
  std::shared_ptr<Decision> decision{nullptr};

  // Thread in which KvStore will be running.
  std::unique_ptr<std::thread> decisionThread{nullptr};
};

// Convert an integer to hex
inline std::string toHex(const int num);

// Convert an integer to prefix IPv6
inline std::string nodeToPrefixV6(const uint32_t nodeId);

// Get a unique Id for adjacency-label
inline uint32_t getId(const uint8_t swMarker, const int podId, const int swId);

// Get a unique node name
std::string getNodeName(
    const uint8_t swMarker, const int podId, const int swId);

// Accumulate the time extracted from perfevent
void accumulatePerfTimes(
    const thrift::PerfEvents& perfEvents, std::vector<uint64_t>& processTimes);

// Send kvstore update to decision and receive routes
void sendRecvUpdate(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    std::vector<uint64_t>& processTimes,
    thrift::Publication& newPub);

void sendRecvInitialUpdate(
    std::shared_ptr<DecisionWrapper> const& decisionWrapper,
    std::vector<uint64_t>& processTimes,
    const std::string& nodeName,
    std::unordered_map<std::string, thrift::AdjacencyDatabase>&& adjs,
    std::unordered_map<std::string, thrift::PrefixDatabase>&& prefixes);

// Send kvstore update for a given node's adjacency DB
void sendRecvAdjUpdate(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    std::vector<uint64_t>& processTimes,
    const std::string& nodeName,
    const std::vector<thrift::Adjacency>& adjs,
    bool overloadBit);

// Send kvstore update for a given node's prefox advertisement
void sendRecvPrefixUpdate(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    std::vector<uint64_t>& processTimes,
    const std::string& nodeName,
    std::pair<PrefixKey, thrift::PrefixDatabase>&& keyDbPair);

// Add an adjacency to node
inline void createAdjacencyEntry(
    const uint32_t nodeId,
    const std::string& ifName,
    std::vector<thrift::Adjacency>& adjs,
    const std::string& otherIfName);

// Get ifName
std::string getFabricIfName(const std::string& id, const std::string& otherId);

/**
 * Add an adjacency to the node identified by (swMarker, podId, swId)
 */
inline void createFabricAdjacency(
    const std::string& sourceNodeName,
    const uint8_t swMarker,
    const int podId,
    const int swId,
    std::vector<thrift::Adjacency>& adjs);

// Add one adjacency to node at grid(row, col)
inline void createGridAdjacency(
    const int row,
    const int col,
    const std::string& ifName,
    std::vector<thrift::Adjacency>& adjs,
    const int n,
    const std::string& otherIfName);

// Get ifName
std::string getIfName(const uint32_t id, const uint32_t otherId);

// Add all adjacencies to node at (row, col)
inline std::vector<thrift::Adjacency> createGridAdjacencys(
    const int row, const int col, const uint32_t n);

// Create a grid topology
std::pair<
    std::unordered_map<std::string, thrift::AdjacencyDatabase>,
    std::unordered_map<std::string, thrift::PrefixDatabase>>
createGrid(
    const int n,
    const int numPrefixes,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm);

/**
 * Create Adjacencies for spine switches.
 * Each spine switch has numOfPods connections,
 * it connects to one fsw in every pod.
 */
void createSswsAdjacencies(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    thrift::Publication& initialPub,
    const uint8_t sswMarker,
    const uint8_t fswMarker,
    const int numOfPods,
    const int numOfPlanes,
    const int numOfSswsPerPlane);

/**
 * Create Adjacencies for fabric switches.
 * Each fabric switch has (numOfSswsPerPlane + numOfRswsPerPod) connections.
 */
void createFswsAdjacencies(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    thrift::Publication& initialPub,
    const uint8_t sswMarker,
    const uint8_t fswMarker,
    const uint8_t rswMarker,
    const int numOfPods,
    const int numOfFswsPerPod,
    const int numOfSswsPerPlane,
    const int numOfRswsPerPod);

/**
 * Create Adjacencies for rack switches.
 * Each rack switch has numOfFswsPerPod connections.
 */
void createRswsAdjacencies(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    thrift::Publication& initialPub,
    const uint8_t fswMarker,
    const uint8_t rswMarker,
    const int numOfPods,
    const int numOfFswsPerPod,
    const int numOfRswsPerPod);

//
// Create a fabric topology
//
thrift::Publication createFabric(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    const int numOfPods,
    const int numOfSswsPerPlane,
    const int numOfFswsPerPod,
    const int numOfRswsPerPod);

//
// Randomly choose one rsw from a random pod,
// toggle it's overload bit in AdjacencyDb
//
void updateRandomFabricAdjs(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    std::optional<std::pair<int, int>>& selectedNode,
    const int numOfPods,
    const int numOfFswsPerPod,
    const int numOfRswsPerPod,
    std::vector<uint64_t>& processTimes);

//
// Choose a random nodeId for update or revert the last updated nodeId:
// toggle it's advertisement of default route
//
void updateRandomGridPrefixes(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    std::optional<std::pair<int, int>>& selectedNode,
    const int n,
    std::vector<uint64_t>& processTimes);

//
// Choose a random nodeId for update or revert the last updated nodeId:
// toggle it's overload bit in AdjacencyDb
//
void updateRandomGridAdjs(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    std::optional<std::pair<int, int>>& selectedNode,
    const int n,
    std::vector<uint64_t>& processTimes);

//
// Get average processTimes and insert as user counters.
//
void insertUserCounters(
    folly::UserCounters& counters,
    uint32_t iters,
    std::vector<uint64_t>& processTimes,
    std::optional<thrift::PrefixForwardingAlgorithm> forwardingAlgorithm);

//
// Benchmark tests for grid topology
//

void BM_DecisionGridInitialUpdate(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfSws,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    uint32_t numberOfPrefixes);

void BM_DecisionGridPrefixUpdates(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfSws,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    uint32_t numberOfPrefixes);

void BM_DecisionGridAdjUpdates(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfSws,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    uint32_t numberOfPrefixes);

//
// Benchmark test for fabric topology.
//
void BM_DecisionFabric(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfSws,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    uint32_t numberOfPrefixes = 1);

const auto SP_ECMP = thrift::PrefixForwardingAlgorithm::SP_ECMP;
const auto KSP2_ED_ECMP = thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;
} // namespace openr
