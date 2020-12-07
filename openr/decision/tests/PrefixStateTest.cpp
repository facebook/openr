/**
 * Copyright (c) 2014-present, Facebook, Inc.
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
    return toIpPrefix(folly::sformat(
        "{}10.0.0.{}/{}", isV4 ? "" : "::ffff:", seed, isV4 ? 32 : 128));
  }

  PrefixState state_;
  std::unordered_map<
      std::string,
      std::vector<std::pair<PrefixKey, thrift::PrefixEntry>>>
      prefixKeys_;

  void
  SetUp() override {
    auto const numNodes = getNumNodes();
    for (size_t i = 0; i < numNodes; ++i) {
      std::string nodeName = std::to_string(i);
      prefixKeys_[nodeName] = createPrefixDbForNode(nodeName, i);
      for (auto const& [key, entry] : prefixKeys_[nodeName]) {
        EXPECT_FALSE(state_.updatePrefix(key, entry).empty());
      }
    }
  }

  virtual size_t
  getNumNodes() const {
    return 2;
  }

  virtual std::vector<std::pair<PrefixKey, thrift::PrefixEntry>>
  createPrefixDbForNode(std::string const& name, size_t prefixSeed) const {
    return {createPrefixKeyAndEntry(name, getAddrFromSeed(prefixSeed, false)),
            createPrefixKeyAndEntry(name, getAddrFromSeed(prefixSeed, true))};
  }
};

TEST_F(PrefixStateTestFixture, basicOperation) {
  for (auto const& [nodeName, db] : state_.getPrefixDatabases()) {
    std::vector<thrift::PrefixEntry> entries;
    for (auto const& [_, entry] : prefixKeys_.at(nodeName)) {
      entries.push_back(entry);
    }
    EXPECT_THAT(
        db.get_prefixEntries(), testing::UnorderedElementsAreArray(entries));
  }

  auto [key, entry] = prefixKeys_.begin()->second.at(0);
  EXPECT_TRUE(state_.updatePrefix(key, entry).empty());

  entry.set_type(thrift::PrefixType::BREEZE);
  EXPECT_THAT(
      state_.updatePrefix(key, entry),
      testing::UnorderedElementsAre(key.getIpPrefix()));
  EXPECT_TRUE(state_.updatePrefix(key, entry).empty());
  EXPECT_THAT(
      state_.getPrefixDatabases().at(key.getNodeName()).get_prefixEntries(),
      testing::Contains(entry));

  entry.set_forwardingType(thrift::PrefixForwardingType::SR_MPLS);
  EXPECT_THAT(
      state_.updatePrefix(key, entry),
      testing::UnorderedElementsAre(key.getIpPrefix()));
  EXPECT_TRUE(state_.updatePrefix(key, entry).empty());
  EXPECT_THAT(
      state_.getPrefixDatabases().at(key.getNodeName()).get_prefixEntries(),
      testing::Contains(entry));

  // remove all prefixes for a node
  for (auto const& [key, _] : prefixKeys_.begin()->second) {
    EXPECT_THAT(
        state_.deletePrefix(key),
        testing::UnorderedElementsAre(key.getIpPrefix()));
  }
  EXPECT_EQ(0, state_.getPrefixDatabases().count(prefixKeys_.begin()->first));
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
      thrift::IpPrefix(),
      prefixEntries);
  EXPECT_TRUE(routes.empty());
}

/**
 * Test PrefixState::hasConflictingForwardingInfo
 */
TEST(PrefixState, HasConflictingForwardingInfo) {
  std::unordered_map<NodeAndArea, thrift::PrefixEntry> prefixEntries;

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
  prefixEntries = {{{"0", "0"}, pIpSpf}};
  EXPECT_FALSE(PrefixState::hasConflictingForwardingInfo(prefixEntries));

  //
  // Multiple entries conflicting type
  //
  prefixEntries = {{{"0", "0"}, pIpSpf}, {{"1", "1"}, pMplsSpf}};
  EXPECT_TRUE(PrefixState::hasConflictingForwardingInfo(prefixEntries));

  //
  // Multiple entries conflicting algorithm
  //
  prefixEntries = {{{"0", "0"}, pMplsSpf}, {{"1", "1"}, pMplsKspf}};
  EXPECT_TRUE(PrefixState::hasConflictingForwardingInfo(prefixEntries));

  //
  // Multiple entries (no conflicts)
  //
  prefixEntries = {{{"0", "0"}, pMplsSpf}, {{"1", "1"}, pMplsSpf}};
  EXPECT_FALSE(PrefixState::hasConflictingForwardingInfo(prefixEntries));
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
