/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/TopologyGenerator.h>

#include <fmt/format.h>
#include <folly/FileUtil.h>
#include <folly/json/dynamic.h>
#include <folly/json/json.h>
#include <glog/logging.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>

namespace openr {

std::string
TopologyGenerator::getGridNodeName(int row, int col, int n) {
  return fmt::format("{}", row * n + col);
}

std::string
TopologyGenerator::getFabricNodeName(uint8_t swMarker, int podId, int swId) {
  return fmt::format("{}-{}-{}", swMarker, podId, swId);
}

std::string
TopologyGenerator::getIfName(
    const std::string& localNode, const std::string& remoteNode) {
  return fmt::format("if_{}_{}", localNode, remoteNode);
}

void
TopologyGenerator::addBidirectionalAdjacency(
    VirtualRouter& router1,
    VirtualRouter& router2,
    const std::string& if1Name,
    const std::string& if2Name,
    int32_t metric,
    int32_t latencyMs) {
  /*
   * Add interface to router1
   */
  VirtualInterface if1;
  if1.ifName = if1Name;
  if1.ifIndex = static_cast<int>(router1.interfaces.size()) + 1;
  if1.v6Addr = folly::IPAddress::createNetwork(
      fmt::format("fe80::{:x}:{:x}/128", router1.nodeId, if1.ifIndex));
  if1.connectedRouterName = router2.nodeName;
  if1.connectedIfName = if2Name;
  if1.latencyMs = latencyMs;
  if1.metric = metric;
  router1.interfaces.push_back(std::move(if1));

  /*
   * Add adjacency to router1
   */
  VirtualAdjacency adj1;
  adj1.localIfName = if1Name;
  adj1.remoteRouterName = router2.nodeName;
  adj1.remoteIfName = if2Name;
  adj1.metric = metric;
  adj1.latencyMs = latencyMs;
  router1.adjacencies.push_back(std::move(adj1));

  /*
   * Add interface to router2
   */
  VirtualInterface if2;
  if2.ifName = if2Name;
  if2.ifIndex = static_cast<int>(router2.interfaces.size()) + 1;
  if2.v6Addr = folly::IPAddress::createNetwork(
      fmt::format("fe80::{:x}:{:x}/128", router2.nodeId, if2.ifIndex));
  if2.connectedRouterName = router1.nodeName;
  if2.connectedIfName = if1Name;
  if2.latencyMs = latencyMs;
  if2.metric = metric;
  router2.interfaces.push_back(std::move(if2));

  /*
   * Add adjacency to router2
   */
  VirtualAdjacency adj2;
  adj2.localIfName = if2Name;
  adj2.remoteRouterName = router1.nodeName;
  adj2.remoteIfName = if1Name;
  adj2.metric = metric;
  adj2.latencyMs = latencyMs;
  router2.adjacencies.push_back(std::move(adj2));
}

std::vector<thrift::PrefixEntry>
TopologyGenerator::generatePrefixes(
    const std::string& /* nodeName */,
    int numPrefixes,
    PrefixGenerator& prefixGen) {
  std::vector<thrift::PrefixEntry> prefixes;
  prefixes.reserve(numPrefixes);
  auto ipPrefixes = prefixGen.ipv6PrefixGenerator(numPrefixes, 128);

  for (const auto& ipPrefix : ipPrefixes) {
    prefixes.push_back(createPrefixEntry(
        ipPrefix,
        thrift::PrefixType::LOOPBACK,
        "" /* data */,
        thrift::PrefixForwardingType::IP,
        thrift::PrefixForwardingAlgorithm::SP_ECMP));
  }

  return prefixes;
}

Topology
TopologyGenerator::createGrid(int n, int numPrefixesPerNode) {
  LOG(INFO) << fmt::format(
      "Creating {}x{} grid topology ({} routers, {} prefixes per router)",
      n,
      n,
      n * n,
      numPrefixesPerNode);

  Topology topo;
  topo.name = fmt::format("grid_{}x{}", n, n);
  topo.description = fmt::format(
      "{}x{} grid topology with {} prefixes per router",
      n,
      n,
      numPrefixesPerNode);

  PrefixGenerator prefixGen;

  /*
   * Create all routers first
   */
  for (int row = 0; row < n; ++row) {
    for (int col = 0; col < n; ++col) {
      int nodeId = row * n + col;
      std::string nodeName = getGridNodeName(row, col, n);

      VirtualRouter router;
      router.nodeName = nodeName;
      router.nodeId = nodeId;
      router.nodeLabel = 100001 + nodeId;
      router.advertisedPrefixes =
          generatePrefixes(nodeName, numPrefixesPerNode, prefixGen);

      topo.routers.emplace(nodeName, std::move(router));
      topo.routerNames.push_back(nodeName);
    }
  }

  /*
   * Create adjacencies between neighbors.
   * Each router connects to right and down neighbors to avoid duplicates.
   */
  for (int row = 0; row < n; ++row) {
    for (int col = 0; col < n; ++col) {
      std::string nodeName = getGridNodeName(row, col, n);
      auto& router = topo.routers.at(nodeName);

      /*
       * Connect to right neighbor
       */
      if (col + 1 < n) {
        std::string rightName = getGridNodeName(row, col + 1, n);
        auto& rightRouter = topo.routers.at(rightName);
        addBidirectionalAdjacency(
            router,
            rightRouter,
            getIfName(nodeName, rightName),
            getIfName(rightName, nodeName));
      }

      /*
       * Connect to down neighbor
       */
      if (row + 1 < n) {
        std::string downName = getGridNodeName(row + 1, col, n);
        auto& downRouter = topo.routers.at(downName);
        addBidirectionalAdjacency(
            router,
            downRouter,
            getIfName(nodeName, downName),
            getIfName(downName, nodeName));
      }
    }
  }

  LOG(INFO) << fmt::format(
      "Created grid topology: {} routers, {} adjacencies, {} prefixes",
      topo.getRouterCount(),
      topo.getTotalAdjacencyCount(),
      topo.getTotalPrefixCount());

  return topo;
}

size_t
TopologyGenerator::calculateFabricRouterCount(
    int numPods, int numPlanes, int numSswsPerPlane, int numRswsPerPod) {
  size_t sswCount = numPlanes * numSswsPerPlane;
  size_t fswCount = numPods * numPlanes; // numPlanes FSWs per pod
  size_t rswCount = numPods * numRswsPerPod;
  return sswCount + fswCount + rswCount;
}

void
TopologyGenerator::createFabricSsws(
    Topology& topo,
    int /* numPods */,
    int numPlanes,
    int numSswsPerPlane,
    int numPrefixesPerNode,
    PrefixGenerator& prefixGen) {
  int nodeIdCounter = 0;

  for (int planeId = 0; planeId < numPlanes; ++planeId) {
    for (int sswId = 0; sswId < numSswsPerPlane; ++sswId) {
      std::string nodeName = getFabricNodeName(kScaleSswMarker, planeId, sswId);

      VirtualRouter router;
      router.nodeName = nodeName;
      router.nodeId = nodeIdCounter++;
      router.nodeLabel = kScaleSswMarker * 100000 + planeId * 100 + sswId;
      router.advertisedPrefixes =
          generatePrefixes(nodeName, numPrefixesPerNode, prefixGen);

      topo.routers.emplace(nodeName, std::move(router));
      topo.routerNames.push_back(nodeName);
    }
  }
}

void
TopologyGenerator::createFabricFsws(
    Topology& topo,
    int numPods,
    int numPlanes,
    int numSswsPerPlane,
    int /* numRswsPerPod */,
    int numPrefixesPerNode,
    PrefixGenerator& prefixGen) {
  int nodeIdCounter = numPlanes * numSswsPerPlane;

  for (int podId = 0; podId < numPods; ++podId) {
    for (int fswId = 0; fswId < numPlanes; ++fswId) {
      std::string nodeName = getFabricNodeName(kScaleFswMarker, podId, fswId);

      VirtualRouter router;
      router.nodeName = nodeName;
      router.nodeId = nodeIdCounter++;
      router.nodeLabel = kScaleFswMarker * 100000 + podId * 100 + fswId;
      router.advertisedPrefixes =
          generatePrefixes(nodeName, numPrefixesPerNode, prefixGen);

      topo.routers.emplace(nodeName, std::move(router));
      topo.routerNames.push_back(nodeName);

      /*
       * Connect FSW to SSWs in the same plane
       */
      int planeId = fswId;
      for (int sswId = 0; sswId < numSswsPerPlane; ++sswId) {
        std::string sswName =
            getFabricNodeName(kScaleSswMarker, planeId, sswId);
        auto& sswRouter = topo.routers.at(sswName);
        auto& fswRouter = topo.routers.at(nodeName);

        addBidirectionalAdjacency(
            fswRouter,
            sswRouter,
            getIfName(nodeName, sswName),
            getIfName(sswName, nodeName));
      }
    }
  }
}

void
TopologyGenerator::createFabricRsws(
    Topology& topo,
    int numPods,
    int numPlanes,
    int numSswsPerPlane,
    int numRswsPerPod,
    int numPrefixesPerNode,
    PrefixGenerator& prefixGen) {
  int nodeIdCounter = numPlanes * numSswsPerPlane + numPods * numPlanes;

  for (int podId = 0; podId < numPods; ++podId) {
    for (int rswId = 0; rswId < numRswsPerPod; ++rswId) {
      std::string nodeName = getFabricNodeName(kScaleRswMarker, podId, rswId);

      VirtualRouter router;
      router.nodeName = nodeName;
      router.nodeId = nodeIdCounter++;
      router.nodeLabel = kScaleRswMarker * 100000 + podId * 100 + rswId;
      router.advertisedPrefixes =
          generatePrefixes(nodeName, numPrefixesPerNode, prefixGen);

      topo.routers.emplace(nodeName, std::move(router));
      topo.routerNames.push_back(nodeName);

      /*
       * Connect RSW to all FSWs in the same pod
       */
      for (int fswId = 0; fswId < numPlanes; ++fswId) {
        std::string fswName = getFabricNodeName(kScaleFswMarker, podId, fswId);
        auto& fswRouter = topo.routers.at(fswName);
        auto& rswRouter = topo.routers.at(nodeName);

        addBidirectionalAdjacency(
            rswRouter,
            fswRouter,
            getIfName(nodeName, fswName),
            getIfName(fswName, nodeName));
      }
    }
  }
}

Topology
TopologyGenerator::createFabric(
    int numPods,
    int numPlanes,
    int numSswsPerPlane,
    int numRswsPerPod,
    int numPrefixesPerNode) {
  size_t totalRouters = calculateFabricRouterCount(
      numPods, numPlanes, numSswsPerPlane, numRswsPerPod);

  LOG(INFO) << fmt::format(
      "Creating fabric topology: {} pods, {} planes, {} SSWs/plane, {} RSWs/pod ({} total routers)",
      numPods,
      numPlanes,
      numSswsPerPlane,
      numRswsPerPod,
      totalRouters);

  Topology topo;
  topo.name = fmt::format("fabric_{}pods_{}planes", numPods, numPlanes);
  topo.description = fmt::format(
      "Fabric topology with {} pods, {} planes, {} total routers",
      numPods,
      numPlanes,
      totalRouters);

  PrefixGenerator prefixGen;

  /*
   * Create routers in order: SSWs, FSWs, RSWs
   */
  createFabricSsws(
      topo, numPods, numPlanes, numSswsPerPlane, numPrefixesPerNode, prefixGen);
  createFabricFsws(
      topo,
      numPods,
      numPlanes,
      numSswsPerPlane,
      numRswsPerPod,
      numPrefixesPerNode,
      prefixGen);
  createFabricRsws(
      topo,
      numPods,
      numPlanes,
      numSswsPerPlane,
      numRswsPerPod,
      numPrefixesPerNode,
      prefixGen);

  LOG(INFO) << fmt::format(
      "Created fabric topology: {} routers, {} adjacencies, {} prefixes",
      topo.getRouterCount(),
      topo.getTotalAdjacencyCount(),
      topo.getTotalPrefixCount());

  return topo;
}

Topology
TopologyGenerator::createRing(int numRouters, int numPrefixesPerNode) {
  LOG(INFO)
      << fmt::format("Creating ring topology with {} routers", numRouters);

  Topology topo;
  topo.name = fmt::format("ring_{}", numRouters);
  topo.description = fmt::format("Ring topology with {} routers", numRouters);

  PrefixGenerator prefixGen;

  /*
   * Create all routers
   */
  for (int i = 0; i < numRouters; ++i) {
    std::string nodeName = fmt::format("ring-{}", i);

    VirtualRouter router;
    router.nodeName = nodeName;
    router.nodeId = i;
    router.nodeLabel = 100001 + i;
    router.advertisedPrefixes =
        generatePrefixes(nodeName, numPrefixesPerNode, prefixGen);

    topo.routers.emplace(nodeName, std::move(router));
    topo.routerNames.push_back(nodeName);
  }

  /*
   * Connect each router to next in ring
   */
  for (int i = 0; i < numRouters; ++i) {
    int nextIdx = (i + 1) % numRouters;
    std::string nodeName = fmt::format("ring-{}", i);
    std::string nextName = fmt::format("ring-{}", nextIdx);

    auto& router = topo.routers.at(nodeName);
    auto& nextRouter = topo.routers.at(nextName);

    addBidirectionalAdjacency(
        router,
        nextRouter,
        getIfName(nodeName, nextName),
        getIfName(nextName, nodeName));
  }

  LOG(INFO) << fmt::format(
      "Created ring topology: {} routers, {} adjacencies",
      topo.getRouterCount(),
      topo.getTotalAdjacencyCount());

  return topo;
}

Topology
TopologyGenerator::loadFromFile(const std::string& filePath) {
  LOG(INFO) << "Loading topology from file: " << filePath;

  std::string jsonContent;
  if (!folly::readFile(filePath.c_str(), jsonContent)) {
    throw std::runtime_error(
        fmt::format("Failed to read topology file: {}", filePath));
  }

  auto jsonObj = folly::parseJson(jsonContent);
  Topology topo;

  topo.name = jsonObj.getDefault("name", "custom").asString();
  topo.description = jsonObj.getDefault("description", "").asString();

  int nodeIdCounter = 0;

  /*
   * Parse routers
   */
  for (const auto& routerJson : jsonObj["routers"]) {
    VirtualRouter router;
    router.nodeName = routerJson["name"].asString();
    router.nodeId = nodeIdCounter++;
    router.nodeLabel = 100001 + router.nodeId;

    /*
     * Parse prefixes if provided
     */
    if (routerJson.count("prefixes")) {
      for (const auto& prefixStr : routerJson["prefixes"]) {
        auto network = folly::IPAddress::createNetwork(prefixStr.asString());
        thrift::IpPrefix ipPrefix;
        ipPrefix.prefixAddress() = toBinaryAddress(network.first);
        ipPrefix.prefixLength() = network.second;

        router.advertisedPrefixes.push_back(createPrefixEntry(
            ipPrefix,
            thrift::PrefixType::LOOPBACK,
            "" /* data */,
            thrift::PrefixForwardingType::IP,
            thrift::PrefixForwardingAlgorithm::SP_ECMP));
      }
    }

    std::string nodeName = router.nodeName;
    topo.routers.emplace(nodeName, std::move(router));
    topo.routerNames.push_back(nodeName);
  }

  /*
   * Parse adjacencies and create connections.
   * Adjacencies are defined per-router in the JSON.
   */
  for (const auto& routerJson : jsonObj["routers"]) {
    std::string nodeName = routerJson["name"].asString();
    auto& router = topo.routers.at(nodeName);

    if (routerJson.count("adjacencies")) {
      for (const auto& adjJson : routerJson["adjacencies"]) {
        std::string neighborName = adjJson["neighbor"].asString();
        std::string ifName =
            adjJson.getDefault("interface", getIfName(nodeName, neighborName))
                .asString();
        int32_t metric =
            static_cast<int32_t>(adjJson.getDefault("metric", 1).asInt());

        /*
         * Only add if neighbor exists and adjacency not already added
         */
        if (topo.routers.count(neighborName)) {
          bool alreadyExists = false;
          for (const auto& adj : router.adjacencies) {
            if (adj.remoteRouterName == neighborName) {
              alreadyExists = true;
              break;
            }
          }

          if (!alreadyExists) {
            auto& neighborRouter = topo.routers.at(neighborName);
            addBidirectionalAdjacency(
                router,
                neighborRouter,
                ifName,
                getIfName(neighborName, nodeName),
                metric);
          }
        }
      }
    }
  }

  LOG(INFO) << fmt::format(
      "Loaded topology from file: {} routers, {} adjacencies",
      topo.getRouterCount(),
      topo.getTotalAdjacencyCount());

  return topo;
}

//
// BBF (Backbone Fabric) Topology Generation
//

std::string
TopologyGenerator::getBbfSpineName(int planeId, int spineId) {
  return fmt::format("spine-{}-{}", planeId, spineId);
}

std::string
TopologyGenerator::getBbfLeafName(int podId, int leafId) {
  return fmt::format("leaf-{}-{}", podId, leafId);
}

size_t
TopologyGenerator::calculateBbfRouterCount(
    int numPods, int numPlanes, int spinesPerPlane, int leavesPerPod) {
  size_t spines = numPlanes * spinesPerPlane;
  size_t leaves = numPods * leavesPerPod;
  return spines + leaves;
}

size_t
TopologyGenerator::calculateBbfAdjacencyCount(
    int numPods,
    int numPlanes,
    int spinesPerPlane,
    int leavesPerPod,
    int ecmpWidth) {
  /*
   * In a 2-layer Clos, each leaf connects to all spines.
   * Each spine-leaf pair has ecmpWidth parallel links.
   * Total links = (numSpines * numLeaves * ecmpWidth)
   * Each link creates 2 adjacencies (bidirectional).
   */
  size_t numSpines = numPlanes * spinesPerPlane;
  size_t numLeaves = numPods * leavesPerPod;
  return numSpines * numLeaves * ecmpWidth * 2;
}

Topology
TopologyGenerator::createBbf(
    int numPods,
    int numPlanes,
    int spinesPerPlane,
    int leavesPerPod,
    int ecmpWidth,
    int numPrefixesPerNode) {
  size_t totalRouters =
      calculateBbfRouterCount(numPods, numPlanes, spinesPerPlane, leavesPerPod);

  LOG(INFO) << fmt::format(
      "Creating BBF topology: {} pods, {} planes, {} spines/plane, {} leaves/pod, ECMP width={}, total={} routers",
      numPods,
      numPlanes,
      spinesPerPlane,
      leavesPerPod,
      ecmpWidth,
      totalRouters);

  Topology topo;
  topo.name = fmt::format("bbf_p{}_l{}_ecmp{}", numPods, numPlanes, ecmpWidth);
  topo.description = fmt::format(
      "BBF {} pods x {} planes with {} ECMP links ({} total routers)",
      numPods,
      numPlanes,
      ecmpWidth,
      totalRouters);

  PrefixGenerator prefixGen;

  /*
   * Create spine and leaf routers
   */
  createBbfSpines(
      topo, numPlanes, spinesPerPlane, numPrefixesPerNode, prefixGen);
  createBbfLeaves(topo, numPods, leavesPerPod, numPrefixesPerNode, prefixGen);

  /*
   * Create ECMP links between spines and leaves
   */
  createBbfEcmpLinks(
      topo, numPods, numPlanes, spinesPerPlane, leavesPerPod, ecmpWidth);

  LOG(INFO) << fmt::format(
      "Created BBF topology: {} routers, {} adjacencies (ECMP width={})",
      topo.getRouterCount(),
      topo.getTotalAdjacencyCount(),
      ecmpWidth);

  return topo;
}

Topology
TopologyGenerator::createBbfSimple(
    int numSpines,
    int numLeaves,
    int numControlNodes,
    int ecmpWidth,
    int numPrefixesPerNode) {
  LOG(INFO) << fmt::format(
      "Creating BBF topology: {} spines, {} leaves, {} control nodes, ECMP width={}",
      numSpines,
      numLeaves,
      numControlNodes,
      ecmpWidth);

  Topology topo;
  topo.name =
      fmt::format("bbf_s{}_l{}_c{}", numSpines, numLeaves, numControlNodes);
  topo.description = fmt::format(
      "BBF with {} spines, {} leaves, {} control nodes ({} total)",
      numSpines,
      numLeaves,
      numControlNodes,
      numSpines + numLeaves + numControlNodes);

  PrefixGenerator prefixGen;

  /*
   * Create spine nodes
   */
  for (int i = 0; i < numSpines; ++i) {
    std::string nodeName = fmt::format("spine-{}", i);

    VirtualRouter router;
    router.nodeName = nodeName;
    router.nodeId = i;
    router.nodeLabel = 100001 + i;
    router.advertisedPrefixes =
        generatePrefixes(nodeName, numPrefixesPerNode, prefixGen);

    topo.routers.emplace(nodeName, std::move(router));
    topo.routerNames.push_back(nodeName);
  }

  /*
   * Create leaf nodes
   */
  for (int i = 0; i < numLeaves; ++i) {
    std::string nodeName = fmt::format("leaf-{}", i);

    VirtualRouter router;
    router.nodeName = nodeName;
    router.nodeId = numSpines + i;
    router.nodeLabel = 200001 + i;
    router.advertisedPrefixes =
        generatePrefixes(nodeName, numPrefixesPerNode, prefixGen);

    topo.routers.emplace(nodeName, std::move(router));
    topo.routerNames.push_back(nodeName);
  }

  /*
   * Create control nodes
   */
  for (int i = 0; i < numControlNodes; ++i) {
    std::string nodeName = fmt::format("control-{}", i);

    VirtualRouter router;
    router.nodeName = nodeName;
    router.nodeId = numSpines + numLeaves + i;
    router.nodeLabel = 300001 + i;
    router.advertisedPrefixes =
        generatePrefixes(nodeName, numPrefixesPerNode, prefixGen);

    topo.routers.emplace(nodeName, std::move(router));
    topo.routerNames.push_back(nodeName);
  }

  /*
   * Create ECMP links: every leaf connects to every spine
   */
  for (int leaf = 0; leaf < numLeaves; ++leaf) {
    std::string leafName = fmt::format("leaf-{}", leaf);
    auto& leafRouter = topo.routers.at(leafName);

    for (int spine = 0; spine < numSpines; ++spine) {
      std::string spineName = fmt::format("spine-{}", spine);
      auto& spineRouter = topo.routers.at(spineName);

      addEcmpAdjacencies(leafRouter, spineRouter, ecmpWidth);
    }
  }

  /*
   * Control nodes connect to all spines (single link, not ECMP)
   */
  for (int ctrl = 0; ctrl < numControlNodes; ++ctrl) {
    std::string ctrlName = fmt::format("control-{}", ctrl);
    auto& ctrlRouter = topo.routers.at(ctrlName);

    for (int spine = 0; spine < numSpines; ++spine) {
      std::string spineName = fmt::format("spine-{}", spine);
      auto& spineRouter = topo.routers.at(spineName);

      addBidirectionalAdjacency(
          ctrlRouter,
          spineRouter,
          getIfName(ctrlName, spineName),
          getIfName(spineName, ctrlName));
    }
  }

  LOG(INFO) << fmt::format(
      "Created BBF topology: {} routers, {} adjacencies",
      topo.getRouterCount(),
      topo.getTotalAdjacencyCount());

  return topo;
}

void
TopologyGenerator::createBbfSpines(
    Topology& topo,
    int numPlanes,
    int spinesPerPlane,
    int numPrefixesPerNode,
    PrefixGenerator& prefixGen) {
  for (int plane = 0; plane < numPlanes; ++plane) {
    for (int spine = 0; spine < spinesPerPlane; ++spine) {
      std::string nodeName = getBbfSpineName(plane, spine);

      VirtualRouter router;
      router.nodeName = nodeName;
      router.nodeId = plane * spinesPerPlane + spine;
      router.nodeLabel = 100001 + router.nodeId;
      router.advertisedPrefixes =
          generatePrefixes(nodeName, numPrefixesPerNode, prefixGen);

      topo.routers.emplace(nodeName, std::move(router));
      topo.routerNames.push_back(nodeName);
    }
  }
}

void
TopologyGenerator::createBbfLeaves(
    Topology& topo,
    int numPods,
    int leavesPerPod,
    int numPrefixesPerNode,
    PrefixGenerator& prefixGen) {
  for (int pod = 0; pod < numPods; ++pod) {
    for (int leaf = 0; leaf < leavesPerPod; ++leaf) {
      std::string nodeName = getBbfLeafName(pod, leaf);

      VirtualRouter router;
      router.nodeName = nodeName;
      /*
       * Give leaves unique IDs starting after spines
       */
      router.nodeId = 10000 + pod * leavesPerPod + leaf;
      router.nodeLabel = 200001 + router.nodeId;
      router.advertisedPrefixes =
          generatePrefixes(nodeName, numPrefixesPerNode, prefixGen);

      topo.routers.emplace(nodeName, std::move(router));
      topo.routerNames.push_back(nodeName);
    }
  }
}

void
TopologyGenerator::createBbfEcmpLinks(
    Topology& topo,
    int numPods,
    int numPlanes,
    int spinesPerPlane,
    int leavesPerPod,
    int ecmpWidth) {
  /*
   * In a 2-layer Clos, every leaf connects to every spine.
   * With ECMP, each leaf-spine pair has ecmpWidth parallel links.
   */
  for (int pod = 0; pod < numPods; ++pod) {
    for (int leaf = 0; leaf < leavesPerPod; ++leaf) {
      std::string leafName = getBbfLeafName(pod, leaf);
      auto& leafRouter = topo.routers.at(leafName);

      for (int plane = 0; plane < numPlanes; ++plane) {
        for (int spine = 0; spine < spinesPerPlane; ++spine) {
          std::string spineName = getBbfSpineName(plane, spine);
          auto& spineRouter = topo.routers.at(spineName);

          /*
           * Add ecmpWidth parallel links between this leaf-spine pair
           */
          addEcmpAdjacencies(leafRouter, spineRouter, ecmpWidth);
        }
      }
    }
  }
}

void
TopologyGenerator::addEcmpAdjacencies(
    VirtualRouter& router1,
    VirtualRouter& router2,
    int ecmpWidth,
    int32_t metric,
    int32_t latencyMs) {
  /*
   * Create ecmpWidth parallel links between router1 and router2.
   * Each link gets a unique interface name.
   */
  for (int linkId = 0; linkId < ecmpWidth; ++linkId) {
    std::string if1Name =
        fmt::format("{}-{}-{}", router1.nodeName, router2.nodeName, linkId);
    std::string if2Name =
        fmt::format("{}-{}-{}", router2.nodeName, router1.nodeName, linkId);

    addBidirectionalAdjacency(
        router1, router2, if1Name, if2Name, metric, latencyMs);
  }
}

} // namespace openr
