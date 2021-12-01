/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/decision/tests/RoutingBenchmarkUtils.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/tests/mocks/PrefixGenerator.h>

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
  return fmt::format("{}-{}-{}", swMarker, podId, swId);
}

// Convert an integer to hex
inline std::string
toHex(const int num) {
  return fmt::format("{:02x}", num);
}

// Convert an integer to prefix IPv6
inline std::string
nodeToPrefixV6(const uint32_t nodeId) {
  return fmt::format(
      "fc00:{}::{}/128", toHex(nodeId >> 16), toHex(nodeId & 0xffff));
}

// Send adjacencies update to decision and receive routes
void
sendRecvUpdate(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    thrift::Publication& pub) {
  decisionWrapper->sendKvPublication(pub);

  // Receive route update from Decision
  auto routes = decisionWrapper->recvMyRouteDb();
}

void
sendRecvInitialUpdate(
    std::shared_ptr<DecisionWrapper> const& decisionWrapper,
    const std::string& nodeName,
    std::unordered_map<std::string, thrift::AdjacencyDatabase>&& adjDbs,
    std::unordered_map<std::string, thrift::PrefixDatabase>&& prefixDbs) {
  thrift::PerfEvents perfEvents;
  addPerfEvent(perfEvents, nodeName, "DECISION_INIT_UPDATE");

  std::unordered_map<std::string, thrift::Value> keyVals;
  apache::thrift::CompactSerializer serializer;
  for (auto& [key, adjDb] : adjDbs) {
    adjDb.perfEvents_ref() = perfEvents;
    keyVals.emplace(
        key,
        createThriftValue(
            1,
            adjDb.get_thisNodeName(),
            writeThriftObjStr(std::move(adjDb), serializer)));
  }
  for (auto& [key, prefixDb] : prefixDbs) {
    prefixDb.perfEvents_ref() = perfEvents;
    keyVals.emplace(
        key,
        createThriftValue(
            1,
            prefixDb.get_thisNodeName(),
            writeThriftObjStr(std::move(prefixDb), serializer)));
  }

  thrift::Publication pub;
  pub.area_ref() = kTestingAreaName;
  pub.keyVals_ref() = std::move(keyVals);
  decisionWrapper->sendKvPublication(pub);
  decisionWrapper->sendKvStoreSyncedEvent();

  // Receive route update from Decision
  auto routes = decisionWrapper->recvMyRouteDb();
}

// Send adjacencies update to decision and receive routes
void
sendRecvAdjUpdate(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    const std::string& nodeName,
    const std::vector<thrift::Adjacency>& adjs,
    bool overloadBit) {
  LOG(INFO) << "Advertising adj update";
  thrift::Publication pub;
  pub.area_ref() = kTestingAreaName;
  thrift::PerfEvents perfEvents;
  addPerfEvent(perfEvents, nodeName, "DECISION_ADJ_UPDATE");

  pub.keyVals_ref() = {
      {fmt::format("adj:{}", nodeName),
       decisionWrapper->createAdjValue(
           nodeName, 2, adjs, std::move(perfEvents), overloadBit)}};
  sendRecvUpdate(decisionWrapper, pub);
}

void
sendRecvPrefixUpdate(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    const std::string& nodeName,
    std::pair<PrefixKey, thrift::PrefixDatabase>&& keyDbPair,
    folly::BenchmarkSuspender& suspender) {
  thrift::PerfEvents perfEvents;
  addPerfEvent(perfEvents, nodeName, "DECISION_INIT_UPDATE");
  keyDbPair.second.perfEvents_ref() = std::move(perfEvents);
  apache::thrift::CompactSerializer serializer;
  thrift::Publication pub;
  pub.area_ref() = kTestingAreaName;
  pub.keyVals_ref() = {
      {keyDbPair.first.getPrefixKeyV2(),
       createThriftValue(
           1,
           nodeName,
           writeThriftObjStr(std::move(keyDbPair.second), serializer))}};
  suspender.dismiss();
  sendRecvUpdate(decisionWrapper, pub);
  suspender.rehire();
}

// Add an adjacency to node
inline void
createAdjacencyEntry(
    const uint32_t nodeId,
    const std::string& ifName,
    std::vector<thrift::Adjacency>& adjs,
    const std::string& otherIfName) {
  adjs.emplace_back(createThriftAdjacency(
      fmt::format("{}", nodeId),
      ifName,
      fmt::format("fe80:{}::{}", toHex(nodeId >> 16), toHex(nodeId & 0xffff)),
      fmt::format(
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
  return fmt::format("if_{}_{}", id, otherId);
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
      fmt::format("fe80:{}:{}::{}", toHex(swMarker), toHex(podId), toHex(swId)),
      fmt::format("{}.{}.{}.{}", swMarker, (podId >> 8), (podId & 0xff), swId),
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
  return fmt::format("if_{}_{}", id, otherId);
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
std::pair<
    std::unordered_map<std::string, thrift::AdjacencyDatabase>,
    std::unordered_map<std::string, thrift::PrefixDatabase>>
createGrid(
    const int n,
    const int numPrefixes,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm) {
  LOG(INFO) << "grid: " << n << " by " << n;
  LOG(INFO) << " number of prefixes " << numPrefixes;
  std::unordered_map<std::string, thrift::AdjacencyDatabase> adjDbs;
  std::unordered_map<std::string, thrift::PrefixDatabase> prefixDbs;
  PrefixGenerator prefixGenerator;

  // Grid topology
  for (int row = 0; row < n; ++row) {
    for (int col = 0; col < n; ++col) {
      auto nodeId = row * n + col;
      auto nodeName = fmt::format("{}", nodeId);
      // Add adjs
      auto adjs = createGridAdjacencys(row, col, n);
      adjDbs.emplace(
          fmt::format("adj:{}", nodeName),
          createAdjDb(nodeName, adjs, nodeId + 1));

      std::vector<thrift::IpPrefix> prefixes =
          prefixGenerator.ipv6PrefixGenerator(numPrefixes, kBitMaskLen);

      // prefixes
      for (auto prefix : prefixes) {
        auto [key, db] = createPrefixKeyAndDb(
            nodeName,
            createPrefixEntry(
                prefix,
                thrift::PrefixType::LOOPBACK,
                "",
                thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP ==
                        forwardingAlgorithm
                    ? thrift::PrefixForwardingType::SR_MPLS
                    : thrift::PrefixForwardingType::IP,
                forwardingAlgorithm));
        prefixDbs.emplace(key.getPrefixKeyV2(), std::move(db));
      }
    }
  }
  return std::make_pair(std::move(adjDbs), std::move(prefixDbs));
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
    const int numOfSswsPerPlane,
    std::unordered_map<std::string, std::vector<std::string>>&
        listOfNodenames) {
  std::vector<std::string> nodeNames;
  for (int planeId = 0; planeId < numOfPlanes; planeId++) {
    for (int sswIdInPlane = 0; sswIdInPlane < numOfSswsPerPlane;
         sswIdInPlane++) {
      auto nodeName = getNodeName(sswMarker, planeId, sswIdInPlane);
      nodeNames.push_back(nodeName);
      // Add one fsw in each pod to ssw's adjacencies.
      for (int podId = 0; podId < numOfPods; podId++) {
        std::vector<thrift::Adjacency> adjs;
        auto otherName = getNodeName(fswMarker, podId, planeId);
        createFabricAdjacency(nodeName, fswMarker, podId, planeId, adjs);

        // Add to publication
        (*initialPub.keyVals_ref())
            .emplace(
                fmt::format("adj:{}", nodeName),
                decisionWrapper->createAdjValue(
                    nodeName, 1, adjs, std::nullopt));
      }
    }
  }
  listOfNodenames.emplace("ssw", std::move(nodeNames));
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
    const int numOfRswsPerPod,
    std::unordered_map<std::string, std::vector<std::string>>&
        listOfNodenames) {
  std::vector<std::string> nodeNames;
  for (int podId = 0; podId < numOfPods; podId++) {
    for (int swIdInPod = 0; swIdInPod < numOfFswsPerPod; swIdInPod++) {
      auto nodeName = getNodeName(fswMarker, podId, swIdInPod);
      nodeNames.push_back(nodeName);
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
            getNodeName(rswMarker, podId, otherId); // fmt::format("{}",
                                                    // otherId); //
        createFabricAdjacency(nodeName, rswMarker, podId, otherId, adjs);
      }

      // Add to publication
      (*initialPub.keyVals_ref())
          .emplace(
              fmt::format("adj:{}", nodeName),
              decisionWrapper->createAdjValue(nodeName, 1, adjs, std::nullopt));
    }
  }
  listOfNodenames.emplace("fsw", std::move(nodeNames));
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
    const int numOfRswsPerPod,
    std::unordered_map<std::string, std::vector<std::string>>&
        listOfNodenames) {
  std::vector<std::string> nodeNames;
  for (int podId = 0; podId < numOfPods; podId++) {
    for (int swIdInPod = 0; swIdInPod < numOfRswsPerPod; swIdInPod++) {
      auto nodeName = getNodeName(rswMarker, podId, swIdInPod);
      nodeNames.push_back(nodeName);
      // Add all fsws within the pod to adjacencies.
      std::vector<thrift::Adjacency> adjs;
      for (int otherId = 0; otherId < numOfFswsPerPod; otherId++) {
        auto otherName = getNodeName(fswMarker, podId, otherId);
        createFabricAdjacency(nodeName, fswMarker, podId, otherId, adjs);
      }

      // Add to publication
      (*initialPub.keyVals_ref())
          .emplace(
              fmt::format("adj:{}", nodeName),
              decisionWrapper->createAdjValue(nodeName, 1, adjs, std::nullopt));
    }
  }
  listOfNodenames.emplace("rsw", std::move(nodeNames));
}

//
// Create a fabric topology
//
thrift::Publication
createFabric(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    const int numOfPods,
    const int numOfPlanes,
    const int numOfSswsPerPlane,
    const int numOfFswsPerPod,
    const int numOfRswsPerPod,
    std::unordered_map<std::string, std::vector<std::string>>&
        listOfNodenames) {
  LOG(INFO) << "Pods number: " << numOfPods;
  thrift::Publication initialPub;
  initialPub.area_ref() = kTestingAreaName;

  // ssw: each ssw connects to one fsw of each pod
  // auto numOfPlanes = numOfFswsPerPod;
  createSswsAdjacencies(
      decisionWrapper,
      initialPub,
      kSswMarker,
      kFswMarker,
      numOfPods,
      numOfPlanes,
      numOfSswsPerPlane,
      listOfNodenames);

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
      numOfRswsPerPod,
      listOfNodenames);

  // rsw: each rsw connects to all fsws within the pod
  createRswsAdjacencies(
      decisionWrapper,
      initialPub,
      kFswMarker,
      kRswMarker,
      numOfPods,
      numOfFswsPerPod,
      numOfRswsPerPod,
      listOfNodenames);

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
    const int numOfRswsPerPod) {
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
  sendRecvAdjUpdate(decisionWrapper, rwsNodeName, adjsRsw, overloadBit);
}

//
// Choose a random nodeId for update or revert the last updated nodeId:
// toggle it's overload bit in AdjacencyDb
//
void
updateRandomGridAdjs(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    std::optional<std::pair<int, int>>& selectedNode,
    const int n) {
  // If there has been an update, revert the update,
  // otherwise, choose a random nodeId for update
  auto row = selectedNode.has_value() ? selectedNode.value().first
                                      : folly::Random::rand32() % n;
  auto col = selectedNode.has_value() ? selectedNode.value().second
                                      : folly::Random::rand32() % n;

  auto nodeName = fmt::format("{}", row * n + col);
  auto adjs = createGridAdjacencys(row, col, n);
  auto overloadBit = selectedNode.has_value() ? false : true;
  // Record the updated nodeId
  selectedNode = selectedNode.has_value()
      ? std::nullopt
      : std::optional<std::pair<int, int>>(std::make_pair(row, col));

  // Send the update to decision and receive the routes
  sendRecvAdjUpdate(decisionWrapper, nodeName, adjs, overloadBit);
}

//
// Choose a random nodeId for update or revert the last updated nodeId:
// toggle it's advertisement of default route
//
void
updateRandomGridPrefixes(
    const std::shared_ptr<DecisionWrapper>& decisionWrapper,
    const int n,
    const int numOfUpdatePrefixes,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    folly::BenchmarkSuspender& suspender) {
  PrefixGenerator prefixGenerator;
  apache::thrift::CompactSerializer serializer;

  // Generate one pub for all update prefixes
  // For each node, generate `numOfUpdatePrefixes` keyVals
  std::unordered_map<std::string, thrift::Value> keyVals;
  for (int row = 0; row < n; ++row) {
    for (int col = 0; col < n; ++col) {
      auto nodeName = fmt::format("{}", row * n + col);
      auto prefixEntries =
          generatePrefixEntries(prefixGenerator, numOfUpdatePrefixes);
      for (auto& prefixEntry : prefixEntries) {
        prefixEntry.forwardingType_ref() =
            (thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP ==
                     forwardingAlgorithm
                 ? thrift::PrefixForwardingType::SR_MPLS
                 : thrift::PrefixForwardingType::IP);
        prefixEntry.forwardingAlgorithm_ref() = forwardingAlgorithm;
        auto keyDbPair = createPrefixKeyAndDb(nodeName, prefixEntry);
        keyVals.emplace(
            keyDbPair.first.getPrefixKeyV2(),
            createThriftValue(
                1,
                nodeName,
                writeThriftObjStr(std::move(keyDbPair.second), serializer)));
      }
    }
  }
  thrift::Publication pub;
  pub.area_ref() = kTestingAreaName;
  pub.keyVals_ref() = std::move(keyVals);
  suspender.dismiss();
  sendRecvUpdate(decisionWrapper, pub);
  suspender.rehire();
}

void
generatePrefixUpdatePublication(
    const uint32_t& numOfPrefixes,
    const std::unordered_map<std::string, std::vector<std::string>>&
        listOfNodenames,
    const thrift::PrefixForwardingAlgorithm& forwardingAlgorithm,
    thrift::Publication& initialPub) {
  PrefixGenerator prefixGenerator;
  apache::thrift::CompactSerializer serializer;
  std::unordered_map<std::string, thrift::Value> keyVals;
  for (auto& [_, names] : listOfNodenames) {
    for (auto& nodeName : names) {
      auto prefixEntries =
          generatePrefixEntries(prefixGenerator, numOfPrefixes);
      for (auto& prefixEntry : prefixEntries) {
        prefixEntry.forwardingType_ref() =
            (thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP ==
                     forwardingAlgorithm
                 ? thrift::PrefixForwardingType::SR_MPLS
                 : thrift::PrefixForwardingType::IP);
        prefixEntry.forwardingAlgorithm_ref() = forwardingAlgorithm;
        auto keyDbPair = createPrefixKeyAndDb(nodeName, prefixEntry);
        keyVals.emplace(
            keyDbPair.first.getPrefixKeyV2(),
            createThriftValue(
                1,
                nodeName,
                writeThriftObjStr(std::move(keyDbPair.second), serializer)));
      }
    }
  }
  initialPub.keyVals_ref()->merge(std::move(keyVals));
}

void
BM_DecisionGridInitialUpdate(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfSws,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    uint32_t numberOfPrefixes) {
  auto suspender = folly::BenchmarkSuspender();
  const std::string nodeName{"1"};
  int n = std::sqrt(numOfSws);
  auto [adjs, prefixes] = createGrid(n, numberOfPrefixes, forwardingAlgorithm);

  suspender.dismiss(); // Start measuring benchmark time
  for (uint32_t i = 0; i < iters; i++) {
    auto decisionWrapper = std::make_shared<DecisionWrapper>(nodeName);
    sendRecvInitialUpdate(
        decisionWrapper, nodeName, std::move(adjs), std::move(prefixes));
  }
  suspender.rehire(); // Stop measuring time again
}

void
BM_DecisionGridAdjUpdates(
    folly::UserCounters& counters,
    uint32_t iters,
    uint32_t numOfSws,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    uint32_t numberOfPrefixes) {
  auto suspender = folly::BenchmarkSuspender();
  const std::string nodeName{"1"};
  auto decisionWrapper = std::make_shared<DecisionWrapper>(nodeName);
  int n = std::sqrt(numOfSws);
  auto [adjs, prefixes] = createGrid(n, numberOfPrefixes, forwardingAlgorithm);

  sendRecvInitialUpdate(
      decisionWrapper, nodeName, std::move(adjs), std::move(prefixes));

  // Record the updated nodeId
  std::optional<std::pair<int, int>> selectedNode = std::nullopt;

  suspender.dismiss(); // Start measuring benchmark time

  for (uint32_t i = 0; i < iters; i++) {
    // Advertise adj update. This should trigger the SPF run.
    updateRandomGridAdjs(decisionWrapper, selectedNode, n);
  }

  suspender.rehire(); // Stop measuring time again
}

void
BM_DecisionGridPrefixUpdates(
    uint32_t iters,
    uint32_t numOfNodes,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm,
    uint32_t numOfPrefixes,
    uint32_t numOfUpdatePrefixes) {
  auto suspender = folly::BenchmarkSuspender();
  for (uint32_t i = 0; i < iters; i++) {
    const std::string nodeName{"1"};
    auto decisionWrapper = std::make_shared<DecisionWrapper>(nodeName);
    int n = std::sqrt(numOfNodes);
    auto [adjs, prefixes] = createGrid(n, numOfPrefixes, forwardingAlgorithm);

    sendRecvInitialUpdate(
        decisionWrapper, nodeName, std::move(adjs), std::move(prefixes));

    // Advertise new random prefix from random node to build route
    updateRandomGridPrefixes(
        decisionWrapper,
        n,
        numOfUpdatePrefixes,
        forwardingAlgorithm,
        suspender);
  }
}

//
// Benchmark test for fabric topology.
//
void
BM_DecisionFabricInitialUpdate(
    uint32_t iters,
    uint32_t numOfPods,
    uint32_t numOfPlanes,
    uint32_t numberOfPrefixes,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm) {
  auto suspender = folly::BenchmarkSuspender();
  const std::string nodeName = getNodeName(kFswMarker, 0, 0);
  for (uint32_t i = 0; i < iters; i++) {
    auto decisionWrapper = std::make_shared<DecisionWrapper>(nodeName);
    std::unordered_map<std::string, std::vector<std::string>> listOfNodenames;

    auto initialPub = createFabric(
        decisionWrapper,
        numOfPods,
        numOfPlanes,
        kNumOfSswsPerPlane,
        numOfPlanes,
        kNumOfRswsPerPod,
        listOfNodenames);

    generatePrefixUpdatePublication(
        numberOfPrefixes, listOfNodenames, forwardingAlgorithm, initialPub);

    suspender.dismiss(); // Start measuring benchmark time
    //
    // Publish initial link state info to KvStore, This should trigger the
    // SPF run.
    //
    decisionWrapper->sendKvPublication(initialPub);
    // Trigger initial route build by pushing initialization event
    // to kvStoreUpdatesQueue
    decisionWrapper->sendKvStoreSyncedEvent();

    // Receive RouteUpdate from Decision
    decisionWrapper->recvMyRouteDb();
    suspender.rehire(); // Stop measuring time again
  }
}

void
BM_DecisionFabricPrefixUpdates(
    uint32_t iters,
    uint32_t numOfPods,
    uint32_t numOfPlanes,
    uint32_t numberOfPrefixes,
    uint32_t numOfUpdatePrefixes,
    thrift::PrefixForwardingAlgorithm forwardingAlgorithm) {
  auto suspender = folly::BenchmarkSuspender();
  const std::string nodeName = getNodeName(kFswMarker, 0, 0);
  for (uint32_t i = 0; i < iters; i++) {
    auto decisionWrapper = std::make_shared<DecisionWrapper>(nodeName);
    std::unordered_map<std::string, std::vector<std::string>> listOfNodenames;

    auto initialPub = createFabric(
        decisionWrapper,
        numOfPods,
        numOfPlanes,
        kNumOfSswsPerPlane,
        numOfPlanes, // numOfFswsPerPod == numOfPlanes
        kNumOfRswsPerPod,
        listOfNodenames);

    generatePrefixUpdatePublication(
        numberOfPrefixes, listOfNodenames, forwardingAlgorithm, initialPub);

    //
    // Publish initial link state info to KvStore, This should trigger the
    // SPF run.
    //
    decisionWrapper->sendKvPublication(initialPub);
    // Trigger initial route build by pushing initialization event
    // to kvStoreUpdatesQueue
    decisionWrapper->sendKvStoreSyncedEvent();

    // Receive RouteUpdate from Decision
    decisionWrapper->recvMyRouteDb();

    thrift::Publication pub;
    pub.area_ref() = kTestingAreaName;
    generatePrefixUpdatePublication(
        numOfUpdatePrefixes, listOfNodenames, forwardingAlgorithm, pub);

    suspender.dismiss(); // Start measuring benchmark time

    decisionWrapper->sendKvPublication(pub);
    decisionWrapper->recvMyRouteDb();

    suspender.rehire(); // Stop measuring time again
  }
}
} // namespace openr
