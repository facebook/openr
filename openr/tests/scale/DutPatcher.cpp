/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/DutPatcher.h>

#include <algorithm>

#include <fmt/format.h>
#include <folly/logging/xlog.h>

namespace openr {

namespace {

// Node ID for the DUT entry. Chosen well above any simulated router ID so
// it never collides.
constexpr int kDutNodeId = 99999;

} // namespace

std::vector<std::string>
DutPatcher::buildDutNeighborNames(const thrift::ScaleTestConfig& cfg) {
  const auto& t = *cfg.topology();
  const bool dutIsSpine = (*t.dutRole() == thrift::DutRole::SPINE);
  std::vector<std::string> names;
  if (dutIsSpine) {
    names.reserve(*t.numLeaves() + *t.numSuperSpines());
    for (int i = 0; i < *t.numLeaves(); ++i) {
      names.emplace_back(fmt::format("leaf-{}", i));
    }
    for (int i = 0; i < *t.numSuperSpines(); ++i) {
      names.emplace_back(fmt::format("control-{}", i));
    }
  } else {
    names.reserve(*t.numSpines() + (*t.numSites() > 0 ? 1 : 0));
    for (int i = 0; i < *t.numSpines(); ++i) {
      names.emplace_back(fmt::format("spine-{}", i));
    }
    // DUT plays the role of leaf-0. Connect only to the eb-site that owns
    // leaf-0 (site index 0).
    if (*t.numSites() > 0) {
      names.emplace_back("eb-site-0");
    }
  }
  return names;
}

void
DutPatcher::stripReplacedLeaf(Topology& topo) {
  constexpr const char* kReplacedLeaf = "leaf-0";
  if (topo.routers.erase(kReplacedLeaf) == 0) {
    return;
  }
  topo.routerNames.erase(
      std::remove(
          topo.routerNames.begin(), topo.routerNames.end(), kReplacedLeaf),
      topo.routerNames.end());
  for (auto& [_, router] : topo.routers) {
    router.adjacencies.erase(
        std::remove_if(
            router.adjacencies.begin(),
            router.adjacencies.end(),
            [&](const VirtualAdjacency& a) {
              return a.remoteRouterName == kReplacedLeaf;
            }),
        router.adjacencies.end());
    router.interfaces.erase(
        std::remove_if(
            router.interfaces.begin(),
            router.interfaces.end(),
            [&](const VirtualInterface& i) {
              return i.connectedRouterName == kReplacedLeaf;
            }),
        router.interfaces.end());
  }
}

void
DutPatcher::patchDutIntoTopology(
    Topology& topo,
    const std::string& dutNodeName,
    const std::vector<std::string>& dutNeighborNames,
    const std::vector<std::string>& interfaces) {
  VirtualRouter dutRouter;
  dutRouter.nodeName = dutNodeName;
  dutRouter.nodeId = kDutNodeId;
  dutRouter.nodeLabel = 0;
  topo.routers.emplace(dutNodeName, std::move(dutRouter));
  // Keep routerNames in sync with routers (see Topology struct contract;
  // TopologyGenerator and stripReplacedLeaf maintain the same invariant).
  topo.routerNames.push_back(dutNodeName);

  if (interfaces.empty() || dutNeighborNames.empty()) {
    return;
  }
  const int numDutNeighbors = static_cast<int>(dutNeighborNames.size());
  const int neighborsPerInterface = numDutNeighbors / interfaces.size();
  int neighborIdx = 0;
  for (size_t i = 0; i < interfaces.size(); ++i) {
    const auto& ifName = interfaces[i];
    const int neighborsOnThisIf = (i == interfaces.size() - 1)
        ? (numDutNeighbors - neighborIdx)
        : neighborsPerInterface;
    for (int j = 0; j < neighborsOnThisIf && neighborIdx < numDutNeighbors;
         ++j, ++neighborIdx) {
      const auto& neighborName = dutNeighborNames[neighborIdx];
      auto it = topo.routers.find(neighborName);
      if (it == topo.routers.end()) {
        XLOGF(
            WARN,
            "[DutPatcher] dutNeighbor {} not present in topology — "
            "skipping (topology shape mismatch or typo'd config)",
            neighborName);
        continue;
      }
      VirtualAdjacency dutAdj;
      dutAdj.localIfName = fmt::format("{}-to-dut", neighborName);
      dutAdj.remoteRouterName = dutNodeName;
      dutAdj.remoteIfName = ifName;
      dutAdj.metric = 1;
      dutAdj.latencyMs = 1;
      it->second.adjacencies.push_back(std::move(dutAdj));
    }
  }
}

} // namespace openr
