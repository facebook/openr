/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/decision/tests/RoutingBenchmarkUtils.h>

namespace openr {
// Get a unique Id for adjacency-label
inline uint32_t
getId(const uint8_t swMarker, const int podId, const int swId) {
  CHECK_GT(1000, podId);
  CHECK_GT(100, swId);
  return swMarker * 100000 + podId * 100 + swId;
}

std::string
getNodeName(const uint8_t swMarker, const int podId, const int swId) {
  return folly::sformat("{}-{}-{}", swMarker, podId, swId);
}

// Accumulate the time extracted from perfevent
void
accumulatePerfTimes(
    const thrift::PerfEvents& perfEvents, std::vector<uint64_t>& processTimes) {
  // The size of perfEvents.events should = processTimes.size() + 1
  CHECK_EQ((*perfEvents.events_ref()).size(), processTimes.size() + 1);

  // Accumulate time into processTimes
  for (size_t index = 1; index < (*perfEvents.events_ref()).size(); index++) {
    processTimes[index - 1] +=
        (*(*perfEvents.events_ref())[index].unixTs_ref() -
         *(*perfEvents.events_ref())[index - 1].unixTs_ref());
  }
}

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
// Send adjacencies update to decision and receive routes
void
sendRecvUpdate(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    thrift::Publication& newPub,
    const std::string& nodeName,
    const std::vector<thrift::Adjacency>& adjs,
    std::vector<uint64_t>& processTimes,
    bool overloadBit) {
  // Add perfevent
  thrift::PerfEvents perfEvents;
  addPerfEvent(perfEvents, nodeName, "DECISION_INIT_UPDATE");

  // Add adjs to publication
  (*newPub.keyVals_ref())[folly::sformat("adj:{}", nodeName)] =
      decisionWrapper->createAdjValue(
          nodeName, 2, adjs, std::move(perfEvents), overloadBit);

  LOG(INFO) << "Advertising adj update";
  decisionWrapper->sendKvPublication(newPub);

  // Receive route update from Decision
  auto routes2 = decisionWrapper->recvMyRouteDb();

  // Extract time from perfevent and accumulate processing time
  if (routes2.perfEvents.has_value()) {
    accumulatePerfTimes(routes2.perfEvents.value(), processTimes);
  }
}
// Add an adjacency to node
inline void
createAdjacencyEntry(
    const uint32_t nodeId,
    const std::string& ifName,
    std::vector<thrift::Adjacency>& adjs,
    const std::string& otherIfName) {
  adjs.emplace_back(createThriftAdjacency(
      folly::sformat("{}", nodeId),
      ifName,
      folly::sformat(
          "fe80:{}::{}", toHex(nodeId >> 16), toHex(nodeId & 0xffff)),
      folly::sformat(
          "10.{}.{}.{}", nodeId >> 16, (nodeId >> 8) & 0xff, nodeId & 0xff),
      1,
      100001 + nodeId /* adjacency-label */,
      false /* overload-bit */,
      100,
      10000 /* timestamp */,
      1 /* weight */,
      otherIfName));
}
// Get ifName
std::string
getFabricIfName(const std::string& id, const std::string& otherId) {
  // Naming convention of ifName: "if_<my-id>_<neighbor-id>"
  return folly::sformat("if_{}_{}", id, otherId);
}

/**
 * Add an adjacency to the node identified by (swMarker, podId, swId)
 */
inline void
createFabricAdjacency(
    const std::string& sourceNodeName,
    const uint8_t swMarker,
    const int podId,
    const int swId,
    std::vector<thrift::Adjacency>& adjs) {
  const auto otherName = getNodeName(swMarker, podId, swId);
  adjs.emplace_back(createThriftAdjacency(
      otherName,
      getFabricIfName(sourceNodeName, otherName),
      folly::sformat(
          "fe80:{}:{}::{}", toHex(swMarker), toHex(podId), toHex(swId)),
      folly::sformat(
          "{}.{}.{}.{}", swMarker, (podId >> 8), (podId & 0xff), swId),
      1,
      getId(swMarker, podId, swId) /* adjacency-label */,
      false /* overload-bit */,
      100,
      10000 /* timestamp */,
      1 /* weight */,
      getFabricIfName(otherName, sourceNodeName)));
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
  createAdjacencyEntry(nodeId, ifName, adjs, otherIfName);
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
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    const int n,
    const int numPrefixes,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm) {
  LOG(INFO) << "grid: " << n << " by " << n;
  LOG(INFO) << " number of prefixes " << numPrefixes;
  thrift::Publication initialPub;

  // Grid topology
  for (int row = 0; row < n; ++row) {
    for (int col = 0; col < n; ++col) {
      auto nodeId = row * n + col;
      auto nodeName = folly::sformat("{}", nodeId);
      // Add adjs
      auto adjs = createGridAdjacencys(row, col, n);
      (*initialPub.keyVals_ref())
          .emplace(
              folly::sformat("adj:{}", nodeName),
              decisionWrapper->createAdjValue(nodeName, 1, adjs, std::nullopt));

      // prefixes
      std::vector<thrift::IpPrefix> prefixes;
      for (int i = 0; i < numPrefixes; i++) {
        prefixes.push_back(toIpPrefix(nodeToPrefixV6(nodeId + i)));
      }

      (*initialPub.keyVals_ref())
          .emplace(
              folly::sformat("prefix:{}", nodeName),
              decisionWrapper->createPrefixValue(
                  nodeName, 1, prefixes, forwardingAlgorithm));
    }
  }
  return initialPub;
}

/**
 * Create Adjacencies for spine switches.
 * Each spine switch has numOfPods connections,
 * it connects to one fsw in every pod.
 */
void
createSswsAdjacencies(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    thrift::Publication& initialPub,
    const uint8_t sswMarker,
    const uint8_t fswMarker,
    const int numOfPods,
    const int numOfPlanes,
    const int numOfSswsPerPlane) {
  for (int planeId = 0; planeId < numOfPlanes; planeId++) {
    for (int sswIdInPlane = 0; sswIdInPlane < numOfSswsPerPlane;
         sswIdInPlane++) {
      auto nodeName = getNodeName(sswMarker, planeId, sswIdInPlane);
      // Add one fsw in each pod to ssw's adjacencies.
      for (int podId = 0; podId < numOfPods; podId++) {
        std::vector<thrift::Adjacency> adjs;
        auto otherName = getNodeName(fswMarker, podId, planeId);
        createFabricAdjacency(nodeName, fswMarker, podId, planeId, adjs);

        // Add to publication
        (*initialPub.keyVals_ref())
            .emplace(
                folly::sformat("adj:{}", nodeName),
                decisionWrapper->createAdjValue(
                    nodeName, 1, adjs, std::nullopt));
      }
    }
  }
}

/**
 * Create Adjacencies for fabric switches.
 * Each fabric switch has (numOfSswsPerPlane + numOfRswsPerPod) connections.
 */
void
createFswsAdjacencies(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    thrift::Publication& initialPub,
    const uint8_t sswMarker,
    const uint8_t fswMarker,
    const uint8_t rswMarker,
    const int numOfPods,
    const int numOfFswsPerPod,
    const int numOfSswsPerPlane,
    const int numOfRswsPerPod) {
  for (int podId = 0; podId < numOfPods; podId++) {
    for (int swIdInPod = 0; swIdInPod < numOfFswsPerPod; swIdInPod++) {
      auto nodeName = getNodeName(fswMarker, podId, swIdInPod);
      std::vector<thrift::Adjacency> adjs;
      // Add ssws within the plane to adjacencies
      auto planeId = swIdInPod;
      for (int otherId = 0; otherId < numOfSswsPerPlane; otherId++) {
        auto otherName = getNodeName(sswMarker, planeId, otherId);
        createFabricAdjacency(nodeName, sswMarker, planeId, otherId, adjs);
      }

      // Add all rsws within the pod to adjacencies.
      for (int otherId = 0; otherId < numOfRswsPerPod; otherId++) {
        auto otherName =
            getNodeName(rswMarker, podId, otherId); // folly::sformat("{}",
                                                    // otherId); //
        createFabricAdjacency(nodeName, rswMarker, podId, otherId, adjs);
      }

      // Add to publication
      (*initialPub.keyVals_ref())
          .emplace(
              folly::sformat("adj:{}", nodeName),
              decisionWrapper->createAdjValue(nodeName, 1, adjs, std::nullopt));
    }
  }
}

/**
 * Create Adjacencies for rack switches.
 * Each rack switch has numOfFswsPerPod connections.
 */
void
createRswsAdjacencies(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    thrift::Publication& initialPub,
    const uint8_t fswMarker,
    const uint8_t rswMarker,
    const int numOfPods,
    const int numOfFswsPerPod,
    const int numOfRswsPerPod) {
  for (int podId = 0; podId < numOfPods; podId++) {
    for (int swIdInPod = 0; swIdInPod < numOfRswsPerPod; swIdInPod++) {
      auto nodeName = getNodeName(rswMarker, podId, swIdInPod);
      // Add all fsws within the pod to adjacencies.
      std::vector<thrift::Adjacency> adjs;
      for (int otherId = 0; otherId < numOfFswsPerPod; otherId++) {
        auto otherName = getNodeName(fswMarker, podId, otherId);
        createFabricAdjacency(nodeName, fswMarker, podId, otherId, adjs);
      }

      // Add to publication
      (*initialPub.keyVals_ref())
          .emplace(
              folly::sformat("adj:{}", nodeName),
              decisionWrapper->createAdjValue(nodeName, 1, adjs, std::nullopt));
    }
  }
}

//
// Create a fabric topology
//
thrift::Publication
createFabric(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    const int numOfPods,
    const int numOfSswsPerPlane,
    const int numOfFswsPerPod,
    const int numOfRswsPerPod) {
  LOG(INFO) << "Pods number: " << numOfPods;
  thrift::Publication initialPub;

  // ssw: each ssw connects to one fsw of each pod
  auto numOfPlanes = numOfFswsPerPod;
  createSswsAdjacencies(
      decisionWrapper,
      initialPub,
      kSswMarker,
      kFswMarker,
      numOfPods,
      numOfPlanes,
      numOfSswsPerPlane);

  // fsw: each fsw connects to all ssws within a plane,
  // each fsw also connects to all rsws within its pod
  createFswsAdjacencies(
      decisionWrapper,
      initialPub,
      kSswMarker,
      kFswMarker,
      kRswMarker,
      numOfPods,
      numOfFswsPerPod,
      numOfSswsPerPlane,
      numOfRswsPerPod);

  // rsw: each rsw connects to all fsws within the pod
  createRswsAdjacencies(
      decisionWrapper,
      initialPub,
      kFswMarker,
      kRswMarker,
      numOfPods,
      numOfFswsPerPod,
      numOfRswsPerPod);

  return initialPub;
}

//
// Randomly choose one rsw from a random pod,
// toggle it's overload bit in AdjacencyDb
//
void
updateRandomFabricAdjs(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    std::optional<std::pair<int, int>>& selectedNode,
    const int numOfPods,
    const int numOfFswsPerPod,
    const int numOfRswsPerPod,
    std::vector<uint64_t>& processTimes) {
  thrift::Publication newPub;

  // Choose a random pod
  auto podId = selectedNode.has_value() ? selectedNode.value().first
                                        : folly::Random::rand32() % numOfPods;

  //
  // If there has been an update, revert the update,
  // otherwise, choose a random rsw for update
  //
  auto rswIdInPod = selectedNode.has_value()
      ? selectedNode.value().second
      : folly::Random::rand32() % numOfRswsPerPod;

  auto rwsNodeName = getNodeName(kRswMarker, podId, rswIdInPod);

  // Add all fsws within the pod to the adjacencies.
  std::vector<thrift::Adjacency> adjsRsw;
  for (int otherId = 0; otherId < numOfFswsPerPod; otherId += 1) {
    auto otherName = getNodeName(kFswMarker, podId, otherId);
    createFabricAdjacency(rwsNodeName, kFswMarker, podId, otherId, adjsRsw);
  }

  auto overloadBit = (selectedNode.has_value()) ? false : true;

  // Record the updated rsw
  selectedNode = (selectedNode.has_value())
      ? std::nullopt
      : std::optional<std::pair<int, int>>(std::make_pair(podId, rswIdInPod));

  // Send the update to decision and receive the routes
  sendRecvUpdate(
      decisionWrapper, newPub, rwsNodeName, adjsRsw, processTimes, overloadBit);
}

//
// Choose a random nodeId for update or revert the last updated nodeId:
// toggle it's overload bit in AdjacencyDb
//
void
updateRandomGridAdjs(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    std::optional<std::pair<int, int>>& selectedNode,
    const int n,
    std::vector<uint64_t>& processTimes) {
  thrift::Publication newPub;

  // If there has been an update, revert the update,
  // otherwise, choose a random nodeId for update
  auto row = selectedNode.has_value() ? selectedNode.value().first
                                      : folly::Random::rand32() % n;
  auto col = selectedNode.has_value() ? selectedNode.value().second
                                      : folly::Random::rand32() % n;

  auto nodeName = folly::sformat("{}", row * n + col);
  auto adjs = createGridAdjacencys(row, col, n);
  auto overloadBit = selectedNode.has_value() ? false : true;
  // Record the updated nodeId
  selectedNode = selectedNode.has_value()
      ? std::nullopt
      : std::optional<std::pair<int, int>>(std::make_pair(row, col));

  // Send the update to decision and receive the routes
  sendRecvUpdate(
      decisionWrapper, newPub, nodeName, adjs, processTimes, overloadBit);
}

//
// Get average processTimes and insert as user counters.
//
void
insertUserCounters(
    folly::UserCounters& counters,
    uint32_t iters,
    std::vector<uint64_t>& processTimes,
    std::optional<thrift::PrefixForwardingAlgorithm> forwardingAlgorithm) {
  // Get average time of each itaration
  for (auto& processTime : processTimes) {
    processTime /= iters == 0 ? 1 : iters;
  }

  // Add customized counters to state.
  counters["adj_receive"] = processTimes[0];

  // Counter for spf is for regular benchmark output
  counters["spf"] = processTimes[2];
  if (forwardingAlgorithm.has_value()) {
    CHECK(
        forwardingAlgorithm.value() == SP_ECMP ||
        forwardingAlgorithm.value() == KSP2_ED_ECMP);

    if (forwardingAlgorithm.value() == SP_ECMP) {
      counters["openr.rib_computation.benchmark_spf.time_ms"] = processTimes[2];
    } else {
      counters["openr.rib_computation.benchmark_kspf.time_ms"] =
          processTimes[2];
    }
  }
}

//
// Benchmark test for grid topology
//
void
BM_DecisionGrid(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfSws,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    uint32_t numberOfPrefixes) {
  auto suspender = folly::BenchmarkSuspender();
  const std::string nodeName{"1"};
  auto decisionWrapper = std::make_shared<DecisionWrapper>(nodeName);
  int n = std::sqrt(numOfSws);
  auto initialPub =
      createGrid(decisionWrapper, n, numberOfPrefixes, forwardingAlgorithm);

  //
  // Publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //
  decisionWrapper->sendKvPublication(initialPub);

  // Receive RouteUpdate from Decision
  decisionWrapper->recvMyRouteDb();

  // Record the updated nodeId
  std::optional<std::pair<int, int>> selectedNode = std::nullopt;
  //
  // Customized time counter
  // processTimes[0] is the time of sending adjDB from Kvstore (simulated) to
  // Decision, processTimes[1] is the time of debounce, and processTimes[2] is
  // the time of spf solver
  //
  std::vector<uint64_t> processTimes{0, 0, 0};
  suspender.dismiss(); // Start measuring benchmark time

  for (uint32_t i = 0; i < iters; i++) {
    // Advertise adj update. This should trigger the SPF run.
    updateRandomGridAdjs(decisionWrapper, selectedNode, n, processTimes);
  }

  suspender.rehire(); // Stop measuring time again
  // Insert processTimes as user counters
  insertUserCounters(counters, iters, processTimes, forwardingAlgorithm);
}

//
// Benchmark test for fabric topology.
//
void
BM_DecisionFabric(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfSws,
    thrift::PrefixForwardingAlgorithm /* TODO use this */) {
  auto suspender = folly::BenchmarkSuspender();
  const std::string nodeName = folly::sformat("{}-{}", kFswMarker, "0-0");
  auto decisionWrapper = std::make_shared<DecisionWrapper>(nodeName);
  const int numOfFswsPerPod = kNumOfFswsPerPod;
  const int numOfRswsPerPod = kNumOfRswsPerPod;
  const int numOfSswsPerPlane = kNumOfSswsPerPlane;
  const int numOfPlanes = numOfFswsPerPod;

  // Check the total number of switches is no smaller than (the number of ssws +
  // the number of switches in one pod)
  CHECK_LE(
      numOfPlanes * numOfSswsPerPlane + numOfFswsPerPod + numOfRswsPerPod,
      numOfSws);

  // #pods = (#total_switches - #ssws) / (sws_per_pod)
  const int numOfPods = (numOfSws - numOfPlanes * numOfSswsPerPlane) /
      (numOfFswsPerPod + numOfRswsPerPod);

  auto initialPub = createFabric(
      decisionWrapper,
      numOfPods,
      numOfSswsPerPlane,
      numOfFswsPerPod,
      numOfRswsPerPod);

  //
  // Publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //
  decisionWrapper->sendKvPublication(initialPub);

  // Receive RouteUpdate from Decision
  decisionWrapper->recvMyRouteDb();

  // Record the updated node
  std::optional<std::pair<int, int>> selectedNode = std::nullopt;

  //
  // Customized time counters
  // processTimes[0] is the time of sending adjDB from Kvstore (simulated) to
  // Decision, processTimes[1] is the time of debounce, and processTimes[2] is
  // the time of spf solver
  //
  std::vector<uint64_t> processTimes{0, 0, 0};
  suspender.dismiss(); // Start measuring benchmark time

  for (uint32_t i = 0; i < iters; i++) {
    // Advertise adj update. This should trigger the SPF run.
    updateRandomFabricAdjs(
        decisionWrapper,
        selectedNode,
        numOfPods,
        numOfFswsPerPod,
        numOfRswsPerPod,
        processTimes);
  }

  suspender.rehire(); // Stop measuring time again
  // Insert processTimes as user counters
  insertUserCounters(counters, iters, processTimes, std::nullopt);
}
} // namespace openr
