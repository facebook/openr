/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/tests/scale/TopologyGenerator.h>
#include <openr/tests/scale/VirtualRouter.h>

namespace openr {

TEST(MultiAreaTopologyTest, ReplicatesStructurePerArea) {
  auto base = TopologyGenerator::createRing(4, 2);
  auto multi = TopologyGenerator::replicateAcrossAreas(base, {"pod", "plane"});

  EXPECT_EQ(multi.getRouterCount(), base.getRouterCount() * 2);
  EXPECT_EQ(multi.routerNames.size(), base.routerNames.size() * 2);

  size_t pod = 0;
  size_t plane = 0;
  std::set<int> ids;
  for (const auto& [name, router] : multi.routers) {
    EXPECT_TRUE(router.area == "pod" || router.area == "plane");
    /* Node name is namespaced as "<area>-<original>". */
    EXPECT_EQ(name.rfind(router.area + "-", 0), 0u);
    ids.insert(router.nodeId);
    if (router.area == "pod") {
      ++pod;
    } else {
      ++plane;
    }
  }
  EXPECT_EQ(pod, base.getRouterCount());
  EXPECT_EQ(plane, base.getRouterCount());
  /* Node ids are unique across the merged topology. */
  EXPECT_EQ(ids.size(), multi.getRouterCount());
}

TEST(MultiAreaTopologyTest, NoCrossAreaAdjacencies) {
  auto base = TopologyGenerator::createGrid(3, 1);
  auto multi = TopologyGenerator::replicateAcrossAreas(base, {"a", "b", "c"});

  EXPECT_EQ(multi.getRouterCount(), base.getRouterCount() * 3);

  for (const auto& [name, router] : multi.routers) {
    for (const auto& adj : router.adjacencies) {
      auto it = multi.routers.find(adj.remoteRouterName);
      ASSERT_NE(it, multi.routers.end())
          << name << " has adjacency to missing node " << adj.remoteRouterName;
      EXPECT_EQ(it->second.area, router.area)
          << name << " adjacency crosses area boundary";
    }
    for (const auto& intf : router.interfaces) {
      if (intf.connectedRouterName.empty()) {
        continue;
      }
      auto it = multi.routers.find(intf.connectedRouterName);
      ASSERT_NE(it, multi.routers.end());
      EXPECT_EQ(it->second.area, router.area);
    }
  }
}

TEST(MultiAreaTopologyTest, ReplicatesAdjacencyAndPrefixCounts) {
  auto base = TopologyGenerator::createRing(5, 3);
  auto multi = TopologyGenerator::replicateAcrossAreas(base, {"x", "y"});

  EXPECT_EQ(multi.getTotalAdjacencyCount(), base.getTotalAdjacencyCount() * 2);
  EXPECT_EQ(multi.getTotalPrefixCount(), base.getTotalPrefixCount() * 2);
}

TEST(MultiAreaTopologyTest, SingleAreaNamespacesAndTags) {
  auto base = TopologyGenerator::createRing(3, 1);
  auto multi = TopologyGenerator::replicateAcrossAreas(base, {"solo"});

  EXPECT_EQ(multi.getRouterCount(), base.getRouterCount());
  for (const auto& [name, router] : multi.routers) {
    EXPECT_EQ(router.area, "solo");
    EXPECT_EQ(name.rfind("solo-", 0), 0u);
  }
}

TEST(MultiAreaTopologyTest, NodeLabelsUniqueAcrossAreas) {
  /*
   * Base whose label gap (2) exceeds its id gap (1): with a single id-derived
   * stride, area "b" would offset every label by 2 and land router "hi"'s base
   * label onto "lo"'s offset label. Node labels must therefore use their own
   * stride to stay unique across the merged topology.
   */
  Topology base;
  VirtualRouter lo;
  lo.nodeName = "lo";
  lo.nodeId = 0;
  lo.nodeLabel = 0;
  VirtualRouter hi;
  hi.nodeName = "hi";
  hi.nodeId = 1;
  hi.nodeLabel = 2;
  base.routers.emplace("lo", lo);
  base.routers.emplace("hi", hi);
  base.routerNames = {"lo", "hi"};

  auto multi = TopologyGenerator::replicateAcrossAreas(base, {"a", "b"});

  std::set<int64_t> labels;
  for (const auto& [name, router] : multi.routers) {
    labels.insert(router.nodeLabel);
  }
  EXPECT_EQ(labels.size(), multi.getRouterCount());
}

TEST(MultiAreaTopologyTest, InterfaceV6AddrsRederivedFromOffsetNodeId) {
  /*
   * After per-area nodeId offsetting, each interface link-local must be
   * re-derived from the offset nodeId (fe80::<nodeId>:<ifIndex>); otherwise
   * area copies keep the base nodeId's address and collide across areas.
   */
  auto base = TopologyGenerator::createRing(4, 1);
  auto multi = TopologyGenerator::replicateAcrossAreas(base, {"pod", "plane"});

  std::set<std::string> v6Addrs;
  for (const auto& [name, router] : multi.routers) {
    for (const auto& intf : router.interfaces) {
      EXPECT_EQ(
          folly::IPAddressV6(
              fmt::format("fe80::{:x}:{:x}", router.nodeId, intf.ifIndex))
              .str(),
          intf.v6Addr.first.str())
          << name << " interface " << intf.ifName << " v6Addr is stale";
      v6Addrs.insert(intf.v6Addr.first.str());
    }
  }
  /* Every interface link-local is unique across the merged topology. */
  EXPECT_EQ(multi.getTotalAdjacencyCount(), v6Addrs.size());
}

TEST(MultiAreaTopologyTest, RejectsEmptyAreaList) {
  auto base = TopologyGenerator::createRing(3, 1);
  EXPECT_THROW(
      TopologyGenerator::replicateAcrossAreas(base, {}), std::invalid_argument);
}

TEST(MultiAreaTopologyTest, RejectsDuplicateAreaNames) {
  auto base = TopologyGenerator::createRing(3, 1);
  EXPECT_THROW(
      TopologyGenerator::replicateAcrossAreas(base, {"pod", "pod"}),
      std::invalid_argument);
}

TEST(MultiAreaTopologyTest, CustomInterfaceV6AddrPreservedAcrossAreas) {
  /*
   * An interface whose v6Addr was not produced by addBidirectionalAdjacency
   * (does not match the fe80::<nodeId>:<ifIndex> template) must survive
   * replication unchanged -- the per-area re-derivation only rewrites
   * auto-generated link-locals.
   */
  Topology base;
  VirtualRouter r;
  r.nodeName = "r";
  r.nodeId = 7;
  VirtualInterface intf;
  intf.ifName = "lo0";
  intf.ifIndex = 1;
  intf.v6Addr = folly::IPAddress::createNetwork("2401:db00:dead:beef::1/128");
  r.interfaces.push_back(intf);
  base.routers.emplace("r", r);
  base.routerNames = {"r"};

  auto multi = TopologyGenerator::replicateAcrossAreas(base, {"pod", "plane"});

  ASSERT_EQ(2u, multi.getRouterCount());
  for (const auto& [name, router] : multi.routers) {
    ASSERT_EQ(1u, router.interfaces.size());
    EXPECT_EQ(
        folly::IPAddressV6("2401:db00:dead:beef::1").str(),
        router.interfaces.front().v6Addr.first.str())
        << name << " custom v6Addr was overwritten";
  }
}

TEST(MultiAreaTopologyTest, RejectsNodeIdOverflow) {
  /*
   * A base id space large enough that offsetting it by one area copy would
   * exceed the 32-bit nodeId range must be rejected, not silently truncated
   * into colliding ids.
   */
  Topology base;
  VirtualRouter big;
  big.nodeName = "big";
  big.nodeId = 1'500'000'000;
  base.routers.emplace("big", big);
  base.routerNames = {"big"};
  EXPECT_THROW(
      TopologyGenerator::replicateAcrossAreas(base, {"a", "b"}),
      std::invalid_argument);
}

} // namespace openr
