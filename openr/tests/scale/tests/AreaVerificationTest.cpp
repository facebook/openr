/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/tests/scale/AreaVerification.h>
#include <openr/tests/scale/KvStoreThriftInjector.h>
#include <openr/tests/scale/VirtualRouter.h>

namespace openr {

namespace {

VirtualRouter
makeRouter(const std::string& name, int id, const std::string& area) {
  VirtualRouter r;
  r.nodeName = name;
  r.nodeId = id;
  r.nodeLabel = id;
  r.area = area;
  return r;
}

// Two-area topology: a,b in "pod"; c,d in "plane".
Topology
makeTwoAreaTopology() {
  Topology topo;
  const std::vector<std::tuple<std::string, int, std::string>> nodes{
      {"a", 1, "pod"}, {"b", 2, "pod"}, {"c", 3, "plane"}, {"d", 4, "plane"}};
  for (const auto& [name, id, area] : nodes) {
    topo.routers.emplace(name, makeRouter(name, id, area));
    topo.routerNames.push_back(name);
  }
  return topo;
}

} // namespace

TEST(AreaVerificationTest, AreasInTopology) {
  EXPECT_THAT(
      areasInTopology(makeTwoAreaTopology()),
      ::testing::UnorderedElementsAre("pod", "plane"));
}

TEST(AreaVerificationTest, ConsistentWhenObservedMatchesExpected) {
  auto expected =
      KvStoreThriftInjector::buildKeyValsByArea(makeTwoAreaTopology());
  auto diffs = diffKeysByArea(expected, expected);
  EXPECT_TRUE(allAreasConsistent(diffs));
}

TEST(AreaVerificationTest, ReportsMissingAndExtraPerArea) {
  auto expected =
      KvStoreThriftInjector::buildKeyValsByArea(makeTwoAreaTopology());
  auto observed = expected;

  /* Drop a pod key from the DUT's view -> reported as missing in pod. */
  observed.at("pod").erase("adj:a");

  /* Add a stray plane key the DUT should not have -> reported as extra. */
  thrift::Value stray;
  stray.version() = 1;
  stray.originatorId() = "x";
  observed.at("plane").emplace("adj:stray", std::move(stray));

  auto diffs = diffKeysByArea(expected, observed);
  EXPECT_FALSE(allAreasConsistent(diffs));
  EXPECT_THAT(diffs.at("pod").missing, ::testing::Contains("adj:a"));
  EXPECT_TRUE(diffs.at("pod").extra.empty());
  EXPECT_THAT(diffs.at("plane").extra, ::testing::Contains("adj:stray"));
  EXPECT_TRUE(diffs.at("plane").missing.empty());
}

TEST(AreaVerificationTest, AreaMissingFromObservedReportsAllMissing) {
  auto expected =
      KvStoreThriftInjector::buildKeyValsByArea(makeTwoAreaTopology());

  /* DUT only returned the pod area; plane never synced. */
  std::map<std::string, thrift::KeyVals> observed;
  observed["pod"] = expected.at("pod");

  auto diffs = diffKeysByArea(expected, observed);
  EXPECT_TRUE(diffs.at("pod").consistent());
  EXPECT_EQ(expected.at("plane").size(), diffs.at("plane").missing.size());
  EXPECT_TRUE(diffs.at("plane").extra.empty());
}

namespace {

/*
 * Adds a neighbor->DUT adjacency on `nbr`, exactly as
 * DutPatcher::patchDutIntoTopology produces for a border node.
 */
void
peerWithDut(Topology& topo, const std::string& nbr, const std::string& dut) {
  VirtualAdjacency adj;
  adj.remoteRouterName = dut;
  adj.localIfName = nbr + "-to-dut";
  topo.routers.at(nbr).adjacencies.push_back(std::move(adj));
}

} // namespace

TEST(AreaVerificationTest, ProveAbrBridgesEachAreaWhereDutPeers) {
  // DUT patched in (area "0", like DutPatcher) with one border peer per area.
  auto topo = makeTwoAreaTopology();
  topo.routers.emplace("dut.test", makeRouter("dut.test", 99999, "0"));
  topo.routerNames.emplace_back("dut.test");
  peerWithDut(topo, "a", "dut.test"); // pod
  peerWithDut(topo, "c", "dut.test"); // plane

  auto proof = proveAbr(topo, "dut.test");
  EXPECT_TRUE(proof.isAbr());
  EXPECT_THAT(proof.areas, ::testing::UnorderedElementsAre("pod", "plane"));
  EXPECT_EQ(proof.neighborsPerArea.at("pod"), 1);
  EXPECT_EQ(proof.neighborsPerArea.at("plane"), 1);
}

TEST(AreaVerificationTest, ProveAbrCountsBorderNeighborsPerArea) {
  auto topo = makeTwoAreaTopology();
  topo.routers.emplace("dut.test", makeRouter("dut.test", 99999, "0"));
  topo.routerNames.emplace_back("dut.test");
  peerWithDut(topo, "a", "dut.test"); // pod
  peerWithDut(topo, "b", "dut.test"); // pod
  peerWithDut(topo, "c", "dut.test"); // plane

  auto proof = proveAbr(topo, "dut.test");
  EXPECT_TRUE(proof.isAbr());
  EXPECT_EQ(proof.neighborsPerArea.at("pod"), 2);
  EXPECT_EQ(proof.neighborsPerArea.at("plane"), 1);
}

TEST(AreaVerificationTest, ProveAbrNotAbrWhenDutPeersInOneArea) {
  auto topo = makeTwoAreaTopology();
  topo.routers.emplace("dut.test", makeRouter("dut.test", 99999, "0"));
  topo.routerNames.emplace_back("dut.test");
  peerWithDut(topo, "a", "dut.test"); // pod only
  peerWithDut(topo, "b", "dut.test"); // pod only

  auto proof = proveAbr(topo, "dut.test");
  EXPECT_FALSE(proof.isAbr());
  EXPECT_THAT(proof.areas, ::testing::UnorderedElementsAre("pod"));
  EXPECT_EQ(proof.neighborsPerArea.at("pod"), 2);
}

TEST(AreaVerificationTest, ProveAbrIgnoresDutOwnAdjacencies) {
  /*
   * Only the DUT itself holds adjacencies (to a, c); the border nodes do NOT
   * point back. proveAbr must count the neighbor->DUT direction only, so this
   * is NOT an ABR — and it must not be tripped up by the DUT's own adjacencies.
   */
  auto topo = makeTwoAreaTopology();
  auto dut = makeRouter("dut.test", 99999, "0");
  VirtualAdjacency toA;
  toA.remoteRouterName = "a";
  dut.adjacencies.push_back(toA);
  VirtualAdjacency toC;
  toC.remoteRouterName = "c";
  dut.adjacencies.push_back(toC);
  topo.routers.emplace("dut.test", std::move(dut));
  topo.routerNames.emplace_back("dut.test");

  auto proof = proveAbr(topo, "dut.test");
  EXPECT_FALSE(proof.isAbr());
  EXPECT_TRUE(proof.areas.empty());
}

} // namespace openr
