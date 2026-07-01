/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/if/gen-cpp2/Types_types.h>
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

/*
 * Two-area topology: a,b in "pod"; c,d in "plane". No adjacencies/prefixes are
 * needed — buildKeyValsByArea buckets each router's adj: key by its own area.
 */
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

std::set<std::string>
keysOf(const thrift::KeyVals& kv) {
  std::set<std::string> out;
  for (const auto& [k, v] : kv) {
    out.insert(k);
  }
  return out;
}

} // namespace

TEST(KvStoreThriftInjectorTest, BuildKeyValsByAreaBucketsByRouterArea) {
  const auto topo = makeTwoAreaTopology();
  const auto byArea = KvStoreThriftInjector::buildKeyValsByArea(topo);

  EXPECT_EQ(2, byArea.size());
  ASSERT_TRUE(byArea.count("pod"));
  ASSERT_TRUE(byArea.count("plane"));
  EXPECT_THAT(
      keysOf(byArea.at("pod")),
      ::testing::UnorderedElementsAre("adj:a", "adj:b"));
  EXPECT_THAT(
      keysOf(byArea.at("plane")),
      ::testing::UnorderedElementsAre("adj:c", "adj:d"));
}

TEST(KvStoreThriftInjectorTest, BuildKeyValsFlatIsUnionOfAreaBuckets) {
  const auto topo = makeTwoAreaTopology();
  EXPECT_THAT(
      keysOf(KvStoreThriftInjector::buildKeyVals(topo)),
      ::testing::UnorderedElementsAre("adj:a", "adj:b", "adj:c", "adj:d"));
}

} // namespace openr
