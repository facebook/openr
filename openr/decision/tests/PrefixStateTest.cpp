/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/PrefixState.h>

using namespace openr;

class PrefixStateTestFixture : public ::testing::Test {
 protected:
  static thrift::IpPrefix
  getAddrFromSeed(size_t seed, bool isV4) {
    CHECK_LE(seed, 255);
    return toIpPrefix(fmt::format(
        "{}10.0.0.{}/{}", isV4 ? "" : "::ffff:", seed, isV4 ? 32 : 128));
  }

  PrefixState state_;
  std::unordered_map<folly::CIDRNetwork, PrefixEntries> initialEntries_;

  void
  SetUp() override {
    auto const numNodes = getNumNodes();
    for (size_t i = 0; i < numNodes; ++i) {
      std::string nodeName = std::to_string(i);
      for (auto const& [key, entry] : createPrefixDbForNode(nodeName, i)) {
        EXPECT_FALSE(state_.updatePrefix(key, *entry).empty());
        initialEntries_[key.getCIDRNetwork()].emplace(
            std::piecewise_construct,
            std::forward_as_tuple(key.getNodeName(), key.getPrefixArea()),
            std::forward_as_tuple(entry));
      }
    }
    comparePrefixMaps(initialEntries_, state_.prefixes());
  }

  virtual size_t
  getNumNodes() const {
    return 2;
  }

  virtual std::vector<
      std::pair<PrefixKey, std::shared_ptr<thrift::PrefixEntry>>>
  createPrefixDbForNode(std::string const& name, size_t prefixSeed) const {
    return {
        createPrefixKeyAndEntry(name, getAddrFromSeed(prefixSeed, false)),
        createPrefixKeyAndEntry(name, getAddrFromSeed(prefixSeed, true))};
  }

  void
  comparePrefixMaps(
      const std::unordered_map<folly::CIDRNetwork, PrefixEntries>& map1,
      const std::unordered_map<folly::CIDRNetwork, PrefixEntries>& map2) {
    EXPECT_EQ(map1.size(), map2.size());
    for (const auto& [key, innerMap1] : map1) {
      const auto& iter = map2.find(key);
      EXPECT_TRUE(iter != map2.end());
      if (iter == map2.end())
        continue;
      const auto& innerMap2 = iter->second;
      EXPECT_EQ(innerMap1.size(), innerMap2.size());
      for (const auto& [k, v] : innerMap1) {
        const auto& innerIter = innerMap2.find(k);
        EXPECT_TRUE(innerIter != innerMap2.end());
        if (innerIter == innerMap2.end())
          continue;
        EXPECT_EQ(*innerIter->second, *v);
      }
    }
    return;
  }
};

TEST_F(PrefixStateTestFixture, basicOperation) {
  auto& [nodeArea, entry] = *(initialEntries_.begin()->second.begin());
  const PrefixKey key(
      nodeArea.first, toIPNetwork(entry->get_prefix()), nodeArea.second);
  EXPECT_TRUE(state_.updatePrefix(key, *entry).empty());

  entry->type_ref() = thrift::PrefixType::BREEZE;
  EXPECT_THAT(
      state_.updatePrefix(key, *entry),
      testing::UnorderedElementsAre(key.getCIDRNetwork()));
  EXPECT_TRUE(state_.updatePrefix(key, *entry).empty());
  EXPECT_EQ(
      *state_.prefixes().at(toIPNetwork(entry->get_prefix())).at(nodeArea),
      *entry);

  entry->forwardingType_ref() = thrift::PrefixForwardingType::SR_MPLS;
  EXPECT_THAT(
      state_.updatePrefix(key, *entry),
      testing::UnorderedElementsAre(key.getCIDRNetwork()));
  EXPECT_TRUE(state_.updatePrefix(key, *entry).empty());
  EXPECT_EQ(
      *state_.prefixes().at(toIPNetwork(entry->get_prefix())).at(nodeArea),
      *entry);
}

/**
 * Verifies `getReceivedRoutesFiltered` with all filter combinations
 */
TEST(PrefixState, GetReceivedRoutes) {
  PrefixState state;

  //
  // Add prefix entries
  // prefix1 -> (node0, area0), (node0, area1), (node1, area1)
  //
  const auto prefixEntry = createPrefixEntry(toIpPrefix("10.0.0.0/8"));
  PrefixKey k1("node0", toIPNetwork(prefixEntry.get_prefix()), "area0");
  state.updatePrefix(k1, prefixEntry);
  PrefixKey k2("node0", toIPNetwork(prefixEntry.get_prefix()), "area1");
  state.updatePrefix(k2, prefixEntry);
  PrefixKey k3("node1", toIPNetwork(prefixEntry.get_prefix()), "area1");
  state.updatePrefix(k3, prefixEntry);

  thrift::NodeAndArea bestKey;
  bestKey.node_ref() = "";
  bestKey.area_ref() = "";

  //
  // Empty filter
  //
  {
    thrift::ReceivedRouteFilter filter;
    auto routes = state.getReceivedRoutesFiltered(filter);
    ASSERT_EQ(1, routes.size());

    auto& routeDetail = routes.at(0);
    EXPECT_EQ(*prefixEntry.prefix_ref(), *routeDetail.prefix_ref());
    EXPECT_EQ(bestKey, *routeDetail.bestKey_ref());
    EXPECT_EQ(0, routeDetail.bestKeys_ref()->size());
    EXPECT_EQ(3, routeDetail.routes_ref()->size());
  }

  //
  // Filter on prefix
  //
  {
    thrift::ReceivedRouteFilter filter;
    filter.prefixes_ref() =
        std::vector<thrift::IpPrefix>{*prefixEntry.prefix_ref()};

    auto routes = state.getReceivedRoutesFiltered(filter);
    ASSERT_EQ(1, routes.size());

    auto& routeDetail = routes.at(0);
    EXPECT_EQ(*routeDetail.prefix_ref(), *prefixEntry.prefix_ref());
    EXPECT_EQ(*routeDetail.bestKey_ref(), bestKey);
    EXPECT_EQ(0, routeDetail.bestKeys_ref()->size());
    EXPECT_EQ(3, routeDetail.routes_ref()->size());
  }

  //
  // Filter on non-existing prefix
  //
  {
    thrift::ReceivedRouteFilter filter;
    filter.prefixes_ref() =
        std::vector<thrift::IpPrefix>({toIpPrefix("11.0.0.0/8")});

    auto routes = state.getReceivedRoutesFiltered(filter);
    EXPECT_EQ(0, routes.size());
  }

  //
  // Filter with empty prefix list. Should return empty list
  //
  {
    thrift::ReceivedRouteFilter filter;
    filter.prefixes_ref() = std::vector<thrift::IpPrefix>();

    filter.prefixes_ref()->clear();
    auto routes = state.getReceivedRoutesFiltered(filter);
    EXPECT_EQ(0, routes.size());
  }

  //
  // Filter on the prefix and node-name
  //
  {
    thrift::ReceivedRouteFilter filter;
    filter.prefixes_ref() =
        std::vector<thrift::IpPrefix>{*prefixEntry.prefix_ref()};
    filter.nodeName_ref() = "node1";

    auto routes = state.getReceivedRoutesFiltered(filter);
    ASSERT_EQ(1, routes.size());

    auto& routeDetail = routes.at(0);
    EXPECT_EQ(*routeDetail.prefix_ref(), *prefixEntry.prefix_ref());
    EXPECT_EQ(*routeDetail.bestKey_ref(), bestKey);
    EXPECT_EQ(0, routeDetail.bestKeys_ref()->size());
    ASSERT_EQ(1, routeDetail.routes_ref()->size());

    auto& route = routeDetail.routes_ref()->at(0);
    EXPECT_EQ("node1", route.key_ref()->node_ref().value());
    EXPECT_EQ("area1", route.key_ref()->area_ref().value());
  }

  //
  // Filter on the area-name
  //
  {
    thrift::ReceivedRouteFilter filter;
    filter.areaName_ref() = "area0";

    auto routes = state.getReceivedRoutesFiltered(filter);
    ASSERT_EQ(1, routes.size());

    auto& routeDetail = routes.at(0);
    EXPECT_EQ(*routeDetail.prefix_ref(), *prefixEntry.prefix_ref());
    EXPECT_EQ(*routeDetail.bestKey_ref(), bestKey);
    EXPECT_EQ(0, routeDetail.bestKeys_ref()->size());
    ASSERT_EQ(1, routeDetail.routes_ref()->size());

    auto& route = routeDetail.routes_ref()->at(0);
    EXPECT_EQ("node0", route.key_ref()->node_ref().value());
    EXPECT_EQ("area0", route.key_ref()->area_ref().value());
  }

  //
  // Filter on unknown area or node
  //
  {
    thrift::ReceivedRouteFilter filter;
    filter.areaName_ref() = "unknown";

    auto routes = state.getReceivedRoutesFiltered(filter);
    ASSERT_EQ(0, routes.size());
  }
}

/**
 * Verifies the test case with empty entries. Other cases are exercised above
 */
TEST(PrefixState, FilterReceivedRoutes) {
  std::vector<thrift::ReceivedRouteDetail> routes;
  PrefixEntries prefixEntries;
  thrift::ReceivedRouteFilter filter;
  PrefixState::filterAndAddReceivedRoute(
      routes,
      filter.nodeName_ref(),
      filter.areaName_ref(),
      folly::CIDRNetwork(),
      prefixEntries);
  EXPECT_TRUE(routes.empty());
}

/**
 * Test PrefixState::hasConflictingForwardingInfo
 */
TEST(PrefixState, HasConflictingForwardingInfo) {
  std::unordered_map<NodeAndArea, std::shared_ptr<thrift::PrefixEntry>>
      prefixEntries;

  thrift::PrefixEntry pIpSpf, pMplsSpf, pMplsKspf;
  pIpSpf.forwardingType_ref() = thrift::PrefixForwardingType::IP;
  pIpSpf.forwardingAlgorithm_ref() = thrift::PrefixForwardingAlgorithm::SP_ECMP;
  pMplsSpf.forwardingType_ref() = thrift::PrefixForwardingType::SR_MPLS;
  pMplsSpf.forwardingAlgorithm_ref() =
      thrift::PrefixForwardingAlgorithm::SP_ECMP;
  pMplsKspf.forwardingType_ref() = thrift::PrefixForwardingType::SR_MPLS;
  pMplsKspf.forwardingAlgorithm_ref() =
      thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP;

  //
  // Empty case
  //
  EXPECT_FALSE(PrefixState::hasConflictingForwardingInfo(prefixEntries));

  //
  // Single entry (doesn't conflict)
  //
  prefixEntries = {{{"0", "0"}, std::make_shared<thrift::PrefixEntry>(pIpSpf)}};
  EXPECT_FALSE(PrefixState::hasConflictingForwardingInfo(prefixEntries));

  //
  // Multiple entries conflicting type
  //
  prefixEntries = {
      {{"0", "0"}, std::make_shared<thrift::PrefixEntry>(pIpSpf)},
      {{"1", "1"}, std::make_shared<thrift::PrefixEntry>(pMplsSpf)}};
  EXPECT_TRUE(PrefixState::hasConflictingForwardingInfo(prefixEntries));

  //
  // Multiple entries conflicting algorithm
  //
  prefixEntries = {
      {{"0", "0"}, std::make_shared<thrift::PrefixEntry>(pMplsSpf)},
      {{"1", "1"}, std::make_shared<thrift::PrefixEntry>(pMplsKspf)}};
  EXPECT_TRUE(PrefixState::hasConflictingForwardingInfo(prefixEntries));

  //
  // Multiple entries (no conflicts)
  //
  prefixEntries = {
      {{"0", "0"}, std::make_shared<thrift::PrefixEntry>(pMplsSpf)},
      {{"1", "1"}, std::make_shared<thrift::PrefixEntry>(pMplsSpf)}};
  EXPECT_FALSE(PrefixState::hasConflictingForwardingInfo(prefixEntries));
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
