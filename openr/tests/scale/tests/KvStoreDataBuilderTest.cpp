/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <set>
#include <string>

#include <fmt/format.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/tests/scale/KvStoreDataBuilder.h>
#include <openr/tests/scale/VirtualRouter.h>

namespace openr {

namespace {

VirtualRouter
makeRouter(const std::string& name, int id) {
  VirtualRouter r;
  r.nodeName = name;
  r.nodeId = id;
  r.nodeLabel = id;
  return r;
}

VirtualAdjacency
adjTo(const std::string& remote) {
  VirtualAdjacency adj;
  adj.localIfName = fmt::format("if-to-{}", remote);
  adj.remoteRouterName = remote;
  adj.remoteIfName = "if-back";
  adj.metric = 1;
  adj.isUp = true;
  return adj;
}

// Build a star topology: "a" adjacent to b, c, d (all up).
Topology
makeStarTopology() {
  Topology topo;
  for (const auto& [name, id] : std::vector<std::pair<std::string, int>>{
           {"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}}) {
    topo.routers.emplace(name, makeRouter(name, id));
    topo.routerNames.push_back(name);
  }
  topo.routers.at("a").adjacencies = {adjTo("b"), adjTo("c"), adjTo("d")};
  return topo;
}

// Deserialize an adj:<node> Value into the set of its remaining adjacency
// remote-node names.
std::set<std::string>
remoteNamesOf(const thrift::Value& value) {
  apache::thrift::CompactSerializer serializer;
  auto adjDb = readThriftObjStr<thrift::AdjacencyDatabase>(
      value.value().value(), serializer);
  std::set<std::string> remotes;
  for (const auto& adj : *adjDb.adjacencies()) {
    remotes.insert(*adj.otherNodeName());
  }
  return remotes;
}

// Area that an adj:<node> Value's AdjacencyDatabase is tagged with.
std::string
areaOf(const thrift::Value& value) {
  apache::thrift::CompactSerializer serializer;
  auto adjDb = readThriftObjStr<thrift::AdjacencyDatabase>(
      value.value().value(), serializer);
  return *adjDb.area();
}

} // namespace

TEST(KvStoreDataBuilderTest, BuildAdjKeyValueWithLinksDownOmitsTheWholeSet) {
  // The set-based builder must drop EVERY listed neighbor (this is what keeps
  // an endpoint symmetric when it has multiple operator-downed links).
  const auto topo = makeStarTopology();
  auto [key, value] = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
      topo.routers.at("a"), topo, {"b", "c"}, /*version=*/7);

  EXPECT_EQ(key, "adj:a");
  EXPECT_EQ(*value.version(), 7);
  EXPECT_THAT(remoteNamesOf(value), ::testing::UnorderedElementsAre("d"));
}

TEST(KvStoreDataBuilderTest, BuildAdjKeyValueWithLinksDownEmptySetKeepsAll) {
  const auto topo = makeStarTopology();
  auto [key, value] = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
      topo.routers.at("a"), topo, /*adjsToRemove=*/{}, /*version=*/1);
  EXPECT_THAT(
      remoteNamesOf(value), ::testing::UnorderedElementsAre("b", "c", "d"));
}

TEST(KvStoreDataBuilderTest, BuildAdjKeyValueWithLinkDownDropsExactlyOne) {
  // The single-name overload delegates to the set version and drops only that
  // one neighbor.
  const auto topo = makeStarTopology();
  auto [key, value] = KvStoreDataBuilder::buildAdjKeyValueWithLinkDown(
      topo.routers.at("a"), topo, "b", /*version=*/2);
  EXPECT_THAT(remoteNamesOf(value), ::testing::UnorderedElementsAre("c", "d"));
}

TEST(KvStoreDataBuilderTest, BuildersTagRouterArea) {
  // adj keys must be tagged with the router's own area so the DUT files them in
  // the right per-area KvStore.
  auto topo = makeStarTopology();
  topo.routers.at("a").area = "pod";

  auto [k1, linksDown] = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
      topo.routers.at("a"), topo, {"b"}, /*version=*/1);
  EXPECT_EQ(areaOf(linksDown), "pod");

  auto [k2, overloaded] = KvStoreDataBuilder::buildOverloadedAdjKeyValue(
      topo.routers.at("a"), topo, /*version=*/1);
  EXPECT_EQ(areaOf(overloaded), "pod");
}

TEST(KvStoreDataBuilderTest, BuildRemovedNodeAdjKeyValueHonorsArea) {
  auto [key, value] = KvStoreDataBuilder::buildRemovedNodeAdjKeyValue(
      "a", /*version=*/9, /*area=*/"plane");
  EXPECT_EQ(key, "adj:a");
  EXPECT_EQ(areaOf(value), "plane");
}

TEST(KvStoreDataBuilderTest, BuildRemovedNodeAdjKeyValueDefaultsToDefaultArea) {
  // Omitting the area must preserve the historical single-area "0" behavior.
  auto [key, value] =
      KvStoreDataBuilder::buildRemovedNodeAdjKeyValue("a", /*version=*/1);
  EXPECT_EQ(areaOf(value), "0");
}

} // namespace openr
