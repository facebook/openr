/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/TopologyGenerator.h>

#include <limits>
#include <stdexcept>

#include <fmt/format.h>
#include <folly/FileUtil.h>
#include <folly/container/F14Set.h>
#include <folly/json/dynamic.h>
#include <folly/json/json.h>
#include <folly/logging/xlog.h>
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
TopologyGenerator::replicateAcrossAreas(
    const Topology& base, const std::vector<std::string>& areaNames) {
  if (areaNames.empty()) {
    throw std::invalid_argument(
        "replicateAcrossAreas requires at least one area name");
  }
  const folly::F14FastSet<std::string> uniqueAreas(
      areaNames.begin(), areaNames.end());
  if (uniqueAreas.size() != areaNames.size()) {
    throw std::invalid_argument(
        "replicateAcrossAreas requires distinct area names; duplicates would "
        "collide namespaced node names");
  }

  Topology out;
  out.name = base.name.empty() ? "multiarea" : base.name + "-multiarea";
  out.description =
      fmt::format("{} area(s) replicated from base topology", areaNames.size());

  /*
   * Offset node ids and labels per area copy so they stay unique across the
   * merged topology. Ids and labels use independent strides: a base's nodeLabel
   * range is unrelated to (and often far larger than) its nodeId range -- e.g.
   * createGrid labels as 100001+id and createFabric as marker*100000+... -- so
   * a single id-derived stride would let labels from different areas collide.
   *
   * Every topology builder assigns non-negative ids and labels, so the maxima
   * start at 0; an empty base just yields an empty merged topology below.
   */
  int maxBaseId = 0;
  int64_t maxBaseLabel = 0;
  for (const auto& [name, router] : base.routers) {
    maxBaseId = std::max(maxBaseId, router.nodeId);
    maxBaseLabel = std::max(maxBaseLabel, router.nodeLabel);
  }
  /* Strides are 64-bit so maxBaseId == INT_MAX cannot overflow the +1 here. */
  const int64_t idStride = static_cast<int64_t>(maxBaseId) + 1;
  const int64_t labelStride = maxBaseLabel + 1;

  /*
   * nodeId is 32-bit, so the largest merged id -- (numAreas - 1) * idStride +
   * maxBaseId -- must fit in int; otherwise the static_cast below would
   * silently truncate and emit colliding node ids. Fail loudly instead. The
   * merged nodeLabel is 64-bit and built from the same small, non-negative base
   * range, so it cannot overflow int64_t for any realistic area count.
   */
  const int64_t maxMergedId =
      static_cast<int64_t>(areaNames.size() - 1) * idStride + maxBaseId;
  if (maxMergedId > std::numeric_limits<int>::max()) {
    throw std::invalid_argument(
        fmt::format(
            "replicateAcrossAreas: merged node id {} exceeds 32-bit range; too many "
            "areas or too large a base topology",
            maxMergedId));
  }

  for (size_t a = 0; a < areaNames.size(); ++a) {
    const auto& area = areaNames[a];
    const int64_t idOffset = static_cast<int64_t>(a) * idStride;
    const int64_t labelOffset = static_cast<int64_t>(a) * labelStride;

    /* Follow base.routerNames for deterministic ordering. */
    for (const auto& oldName : base.routerNames) {
      VirtualRouter router = base.routers.at(oldName);
      const auto newName = fmt::format("{}-{}", area, oldName);
      const int baseNodeId = router.nodeId;

      router.nodeName = newName;
      router.area = area;
      router.nodeId += static_cast<int>(idOffset);
      router.nodeLabel += labelOffset;

      for (auto& adj : router.adjacencies) {
        adj.remoteRouterName = fmt::format("{}-{}", area, adj.remoteRouterName);
      }
      for (auto& intf : router.interfaces) {
        if (!intf.connectedRouterName.empty()) {
          intf.connectedRouterName =
              fmt::format("{}-{}", area, intf.connectedRouterName);
        }
        /*
         * Re-derive only auto-generated link-locals. addBidirectionalAdjacency
         * builds v6Addr as fe80::<nodeId>:<ifIndex>; an interface still
         * carrying that pre-offset address is regenerated from the offset
         * nodeId so replicated copies stay unique across areas. An address set
         * some other way (e.g. loaded from JSON) won't match the template and
         * is preserved.
         */
        const auto autoGenerated = folly::IPAddress::createNetwork(
            fmt::format("fe80::{:x}:{:x}/128", baseNodeId, intf.ifIndex));
        if (intf.v6Addr == autoGenerated) {
          intf.v6Addr = folly::IPAddress::createNetwork(
              fmt::format("fe80::{:x}:{:x}/128", router.nodeId, intf.ifIndex));
        }
      }

      out.routers.emplace(newName, std::move(router));
      out.routerNames.push_back(newName);
    }
  }

  XLOGF(
      INFO,
      "Replicated base topology ({} routers) across {} areas -> {} routers",
      base.getRouterCount(),
      areaNames.size(),
      out.getRouterCount());

  return out;
}

Topology
TopologyGenerator::createGrid(int n, int numPrefixesPerNode) {
  XLOGF(
      INFO,
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

  XLOGF(
      INFO,
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

  XLOGF(
      INFO,
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

  XLOGF(
      INFO,
      "Created fabric topology: {} routers, {} adjacencies, {} prefixes",
      topo.getRouterCount(),
      topo.getTotalAdjacencyCount(),
      topo.getTotalPrefixCount());

  return topo;
}

Topology
TopologyGenerator::createRing(int numRouters, int numPrefixesPerNode) {
  XLOGF(INFO, "Creating ring topology with {} routers", numRouters);

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

  XLOGF(
      INFO,
      "Created ring topology: {} routers, {} adjacencies",
      topo.getRouterCount(),
      topo.getTotalAdjacencyCount());

  return topo;
}

Topology
TopologyGenerator::loadFromFile(const std::string& filePath) {
  XLOGF(INFO, "Loading topology from file: {}", filePath);

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

  XLOGF(
      INFO,
      "Loaded topology from file: {} routers, {} adjacencies",
      topo.getRouterCount(),
      topo.getTotalAdjacencyCount());

  return topo;
}

} // namespace openr
