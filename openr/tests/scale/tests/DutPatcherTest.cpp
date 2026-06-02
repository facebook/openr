/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <openr/tests/scale/DutPatcher.h>
#include <openr/tests/scale/TopologyGenerator.h>

namespace openr {
namespace {

constexpr int kNumSpines = 4;
constexpr int kNumLeaves = 8;
constexpr int kNumSuperSpines = 2;
constexpr int kNumPods = 2;
constexpr int kNumSites = 0;
constexpr int kNumPrefixesPerNode = 1;
constexpr int kEcmpWidth = 2;

thrift::ScaleTestConfig
makeConfig(thrift::DutRole role) {
  thrift::ScaleTestConfig cfg;
  cfg.topology()->dutRole() = role;
  cfg.topology()->numSpines() = kNumSpines;
  cfg.topology()->numLeaves() = kNumLeaves;
  cfg.topology()->numSuperSpines() = kNumSuperSpines;
  cfg.topology()->numPods() = kNumPods;
  cfg.topology()->numSites() = kNumSites;
  cfg.topology()->numPrefixesPerNode() = kNumPrefixesPerNode;
  cfg.topology()->ecmpWidth() = kEcmpWidth;
  return cfg;
}

/*
 * Builds a minimal spine/leaf fixture inline from public primitives (no
 * dependency on any specific topology generator) with the node names DutPatcher
 * operates on: a single spine-0 connected to leaf-0, leaf-1, and leaf-2.
 */
Topology
makeTopology() {
  Topology topo;
  auto addRouter = [&](const std::string& name, int id) {
    VirtualRouter router;
    router.nodeName = name;
    router.nodeId = id;
    topo.routerNames.push_back(name);
    topo.routers.emplace(name, std::move(router));
  };

  addRouter("spine-0", 0);
  for (int i = 0; i < 3; ++i) {
    const auto leaf = fmt::format("leaf-{}", i);
    addRouter(leaf, i + 1);
    TopologyGenerator::addBidirectionalAdjacency(
        topo.routers.at(leaf),
        topo.routers.at("spine-0"),
        fmt::format("{}-to-spine-0", leaf),
        fmt::format("spine-0-to-{}", leaf));
  }
  return topo;
}

} // namespace

TEST(DutPatcherTest, BuildDutNeighborNamesForSpineLists_LeavesAndControl) {
  auto cfg = makeConfig(thrift::DutRole::SPINE);
  const std::vector<std::string> expected{
      "leaf-0",
      "leaf-1",
      "leaf-2",
      "leaf-3",
      "leaf-4",
      "leaf-5",
      "leaf-6",
      "leaf-7",
      "control-0",
      "control-1",
  };
  EXPECT_EQ(DutPatcher::buildDutNeighborNames(cfg), expected);
}

TEST(DutPatcherTest, BuildDutNeighborNamesForLeafLists_Spines) {
  auto cfg = makeConfig(thrift::DutRole::LEAF);
  cfg.topology()->numSites() = 0;
  const std::vector<std::string> expected{
      "spine-0",
      "spine-1",
      "spine-2",
      "spine-3",
  };
  EXPECT_EQ(DutPatcher::buildDutNeighborNames(cfg), expected);
}

TEST(DutPatcherTest, BuildDutNeighborNamesForLeafIncludesSiteWhenConfigured) {
  auto cfg = makeConfig(thrift::DutRole::LEAF);
  cfg.topology()->numSites() = 4;
  const std::vector<std::string> expected{
      "spine-0",
      "spine-1",
      "spine-2",
      "spine-3",
      "eb-site-0",
  };
  EXPECT_EQ(DutPatcher::buildDutNeighborNames(cfg), expected);
}

TEST(DutPatcherTest, StripReplacedLeafRemovesLeafZeroAndItsReferences) {
  auto topo = makeTopology();
  ASSERT_GT(topo.routers.count("leaf-0"), 0u);
  // Pick any spine that should be peered with leaf-0 in BBF topology.
  const auto& someSpine = topo.routers.at("spine-0");
  const auto preAdjCount = someSpine.adjacencies.size();

  DutPatcher::stripReplacedLeaf(topo);

  EXPECT_EQ(topo.routers.count("leaf-0"), 0u)
      << "leaf-0 should be removed from routers";
  for (const auto& [name, router] : topo.routers) {
    for (const auto& adj : router.adjacencies) {
      EXPECT_NE(adj.remoteRouterName, "leaf-0")
          << "router " << name << " still references leaf-0";
    }
  }
  EXPECT_LT(topo.routers.at("spine-0").adjacencies.size(), preAdjCount)
      << "spine-0's adj count should have dropped after leaf-0 removal";
}

TEST(DutPatcherTest, StripReplacedLeafIsNoOpWhenLeafZeroAbsent) {
  Topology topo;
  // No leaf-0 present; should not throw or modify anything.
  EXPECT_NO_THROW(DutPatcher::stripReplacedLeaf(topo));
  EXPECT_TRUE(topo.routers.empty());
}

TEST(DutPatcherTest, PatchDutAddsDutRouterAndAdjacenciesPerInterface) {
  auto topo = makeTopology();
  const auto preCount = topo.routers.size();
  const std::vector<std::string> neighbors{"leaf-0", "leaf-1", "leaf-2"};
  const std::vector<std::string> ifaces{"eth0.1", "eth0.2"};

  DutPatcher::patchDutIntoTopology(topo, "rsw1.test", neighbors, ifaces);

  EXPECT_EQ(topo.routers.size(), preCount + 1) << "DUT not added to routers";
  ASSERT_GT(topo.routers.count("rsw1.test"), 0u);

  // Each named neighbor should now carry an adjacency pointing at the DUT.
  for (const auto& name : neighbors) {
    bool found = false;
    for (const auto& adj : topo.routers.at(name).adjacencies) {
      if (adj.remoteRouterName == "rsw1.test") {
        found = true;
        EXPECT_EQ(adj.localIfName, fmt::format("{}-to-dut", name));
        break;
      }
    }
    EXPECT_TRUE(found) << name << " has no adjacency to DUT";
  }
}

TEST(DutPatcherTest, PatchDutNoOpAdjacenciesWhenInterfacesEmpty) {
  auto topo = makeTopology();
  DutPatcher::patchDutIntoTopology(
      topo, "rsw1.test", {"leaf-0"}, /*interfaces=*/{});
  // DUT router still inserted, but no neighbor adjacency was added.
  EXPECT_GT(topo.routers.count("rsw1.test"), 0u);
  for (const auto& adj : topo.routers.at("leaf-0").adjacencies) {
    EXPECT_NE(adj.remoteRouterName, "rsw1.test");
  }
}

TEST(DutPatcherTest, MissingNeighborsReportsNamesAbsentFromTopology) {
  // A neighbor-name scheme that does not match the topology (e.g. BBF leaf-N
  // names against a topology that lacks them) must be reported, preserving
  // input order, so the caller can fail loudly instead of operating on a
  // neighbor set inconsistent with the topology.
  auto topo = makeTopology(); // spine-0, leaf-0, leaf-1, leaf-2
  const std::vector<std::string> neighbors{
      "leaf-0", "spine-9", "leaf-2", "leaf-99"};
  const std::vector<std::string> expectedMissing{"spine-9", "leaf-99"};
  EXPECT_EQ(DutPatcher::missingNeighbors(topo, neighbors), expectedMissing);
}

TEST(DutPatcherTest, MissingNeighborsEmptyWhenAllPresent) {
  auto topo = makeTopology();
  const std::vector<std::string> neighbors{"spine-0", "leaf-0", "leaf-1"};
  EXPECT_TRUE(DutPatcher::missingNeighbors(topo, neighbors).empty());
}

} // namespace openr
