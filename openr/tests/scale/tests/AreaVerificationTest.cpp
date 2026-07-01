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

} // namespace openr
