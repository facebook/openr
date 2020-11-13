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
#include <openr/config/tests/Utils.h>
#include <openr/decision/LinkState.h>

#include <openr/decision/tests/DecisionTestUtils.h>

using namespace testing;

TEST(HoldableValueTest, BasicOperation) {
  openr::HoldableValue<bool> hv{true};
  EXPECT_TRUE(hv.value());
  EXPECT_FALSE(hv.hasHold());
  EXPECT_FALSE(hv.decrementTtl());
  const openr::LinkStateMetric holdUpTtl = 10, holdDownTtl = 5;
  EXPECT_FALSE(hv.updateValue(false, holdUpTtl, holdDownTtl));
  for (openr::LinkStateMetric i = 0; i < holdUpTtl - 1; ++i) {
    EXPECT_TRUE(hv.hasHold());
    EXPECT_TRUE(hv.value());
    EXPECT_FALSE(hv.decrementTtl());
  }
  // expire the hold
  EXPECT_TRUE(hv.decrementTtl());
  EXPECT_FALSE(hv.hasHold());
  EXPECT_FALSE(hv.value());

  // expect no hold since the value didn't change
  EXPECT_FALSE(hv.updateValue(false, holdUpTtl, holdDownTtl));
  EXPECT_FALSE(hv.hasHold());
  EXPECT_FALSE(hv.value());

  // change is bringing down now
  EXPECT_FALSE(hv.updateValue(true, holdUpTtl, holdDownTtl));
  for (openr::LinkStateMetric i = 0; i < holdDownTtl - 1; ++i) {
    EXPECT_TRUE(hv.hasHold());
    EXPECT_FALSE(hv.value());
    EXPECT_FALSE(hv.decrementTtl());
  }
  // expire the hold
  EXPECT_TRUE(hv.decrementTtl());
  EXPECT_FALSE(hv.hasHold());
  EXPECT_TRUE(hv.value());

  // change twice within ttl
  EXPECT_FALSE(hv.updateValue(false, holdUpTtl, holdDownTtl));
  EXPECT_TRUE(hv.hasHold());
  EXPECT_TRUE(hv.value());
  EXPECT_FALSE(hv.decrementTtl());

  EXPECT_TRUE(hv.updateValue(true, holdUpTtl, holdDownTtl));
  EXPECT_FALSE(hv.hasHold());
  EXPECT_TRUE(hv.value());

  // test with LinkMetric
  openr::HoldableValue<openr::LinkStateMetric> hvLsm{10};
  EXPECT_EQ(10, hvLsm.value());
  EXPECT_FALSE(hvLsm.hasHold());
  EXPECT_FALSE(hvLsm.decrementTtl());

  // change is bringing up
  EXPECT_FALSE(hvLsm.updateValue(5, holdUpTtl, holdDownTtl));
  for (openr::LinkStateMetric i = 0; i < holdUpTtl - 1; ++i) {
    EXPECT_TRUE(hvLsm.hasHold());
    EXPECT_EQ(10, hvLsm.value());
    EXPECT_FALSE(hvLsm.decrementTtl());
  }
  // expire the hold
  EXPECT_TRUE(hvLsm.decrementTtl());
  EXPECT_FALSE(hvLsm.hasHold());
  EXPECT_EQ(5, hvLsm.value());
}

TEST(LinkTest, BasicOperation) {
  std::string n1 = "node1";
  auto adj1 =
      openr::createAdjacency(n1, "if1", "if2", "fe80::2", "10.0.0.2", 1, 1, 1);
  std::string n2 = "node2";
  auto adj2 =
      openr::createAdjacency(n2, "if2", "if1", "fe80::1", "10.0.0.1", 1, 2, 1);

  openr::Link l1(kTestingAreaName, n1, adj1, n2, adj2);
  EXPECT_EQ(kTestingAreaName.t, l1.getArea());
  EXPECT_EQ(n2, l1.getOtherNodeName(n1));
  EXPECT_EQ(n1, l1.getOtherNodeName(n2));
  EXPECT_THROW(l1.getOtherNodeName("node3"), std::invalid_argument);

  EXPECT_EQ(*adj1.ifName_ref(), l1.getIfaceFromNode(n1));
  EXPECT_EQ(*adj2.ifName_ref(), l1.getIfaceFromNode(n2));
  EXPECT_THROW(l1.getIfaceFromNode("node3"), std::invalid_argument);

  EXPECT_EQ(*adj1.metric_ref(), l1.getMetricFromNode(n1));
  EXPECT_EQ(*adj2.metric_ref(), l1.getMetricFromNode(n2));
  EXPECT_THROW(l1.getMetricFromNode("node3"), std::invalid_argument);

  EXPECT_EQ(*adj1.adjLabel_ref(), l1.getAdjLabelFromNode(n1));
  EXPECT_EQ(*adj2.adjLabel_ref(), l1.getAdjLabelFromNode(n2));
  EXPECT_THROW(l1.getAdjLabelFromNode("node3"), std::invalid_argument);

  EXPECT_FALSE(l1.getOverloadFromNode(n1));
  EXPECT_FALSE(l1.getOverloadFromNode(n2));
  EXPECT_TRUE(l1.isUp());
  EXPECT_THROW(l1.getOtherNodeName("node3"), std::invalid_argument);

  EXPECT_TRUE(l1.setMetricFromNode(n1, 2, 0, 0));
  EXPECT_EQ(2, l1.getMetricFromNode(n1));

  EXPECT_TRUE(l1.setOverloadFromNode(n2, true, 0, 0));
  EXPECT_FALSE(l1.getOverloadFromNode(n1));
  EXPECT_TRUE(l1.getOverloadFromNode(n2));
  EXPECT_FALSE(l1.isUp());

  // compare equivalent links
  openr::Link l2(kTestingAreaName, n2, adj2, n1, adj1);
  EXPECT_TRUE(l1 == l2);
  EXPECT_FALSE(l1 < l2);
  EXPECT_FALSE(l2 < l1);

  // compare non equal links
  std::string n3 = "node3";
  auto adj3 =
      openr::createAdjacency(n2, "if3", "if2", "fe80::3", "10.0.0.3", 1, 1, 1);
  openr::Link l3(kTestingAreaName, n1, adj1, n3, adj3);
  EXPECT_FALSE(l1 == l3);
  EXPECT_TRUE(l1 < l3 || l3 < l1);
}

TEST(LinkStateTest, BasicOperation) {
  std::string n1 = "node1";
  std::string n2 = "node2";
  std::string n3 = "node3";
  auto adj12 =
      openr::createAdjacency(n2, "if2", "if1", "fe80::2", "10.0.0.2", 1, 1, 1);
  auto adj13 =
      openr::createAdjacency(n3, "if3", "if1", "fe80::3", "10.0.0.3", 1, 1, 1);
  auto adj21 =
      openr::createAdjacency(n1, "if1", "if2", "fe80::1", "10.0.0.1", 1, 1, 1);
  auto adj23 =
      openr::createAdjacency(n3, "if3", "if2", "fe80::3", "10.0.0.3", 1, 1, 1);
  auto adj31 =
      openr::createAdjacency(n1, "if1", "if3", "fe80::1", "10.0.0.1", 1, 1, 1);
  auto adj32 =
      openr::createAdjacency(n2, "if2", "if3", "fe80::2", "10.0.0.2", 1, 1, 1);

  openr::Link l1(kTestingAreaName, n1, adj12, n2, adj21);
  openr::Link l2(kTestingAreaName, n2, adj23, n3, adj32);
  openr::Link l3(kTestingAreaName, n3, adj31, n1, adj13);

  auto adjDb1 = openr::createAdjDb(n1, {adj12, adj13}, 1);
  auto adjDb2 = openr::createAdjDb(n2, {adj21, adj23}, 2);
  auto adjDb3 = openr::createAdjDb(n3, {adj31, adj32}, 3);

  openr::LinkState state{kTestingAreaName};

  EXPECT_EQ(kTestingAreaName.t, state.getArea());

  EXPECT_FALSE(state.updateAdjacencyDatabase(adjDb1, 0, 0).topologyChanged);
  EXPECT_TRUE(state.updateAdjacencyDatabase(adjDb2, 0, 0).topologyChanged);
  EXPECT_TRUE(state.updateAdjacencyDatabase(adjDb3, 0, 0).topologyChanged);

  EXPECT_THAT(
      state.linksFromNode(n1), UnorderedElementsAre(Pointee(l1), Pointee(l3)));
  EXPECT_THAT(
      state.linksFromNode(n2), UnorderedElementsAre(Pointee(l1), Pointee(l2)));
  EXPECT_THAT(
      state.linksFromNode(n3), UnorderedElementsAre(Pointee(l2), Pointee(l3)));
  EXPECT_THAT(state.linksFromNode("node4"), testing::IsEmpty());

  EXPECT_FALSE(state.isNodeOverloaded(n1));
  adjDb1.isOverloaded_ref() = true;
  EXPECT_TRUE(state.updateAdjacencyDatabase(adjDb1, 0, 0).topologyChanged);
  EXPECT_TRUE(state.isNodeOverloaded(n1));
  EXPECT_FALSE(state.updateAdjacencyDatabase(adjDb1, 0, 0).topologyChanged);
  adjDb1.isOverloaded_ref() = false;
  EXPECT_TRUE(state.updateAdjacencyDatabase(adjDb1, 0, 0).topologyChanged);
  EXPECT_FALSE(state.isNodeOverloaded(n1));

  adjDb1 = openr::createAdjDb(n1, {adj13}, 1);
  EXPECT_TRUE(state.updateAdjacencyDatabase(adjDb1, 0, 0).topologyChanged);
  EXPECT_THAT(state.linksFromNode(n1), UnorderedElementsAre(Pointee(l3)));
  EXPECT_THAT(state.linksFromNode(n2), UnorderedElementsAre(Pointee(l2)));
  EXPECT_THAT(
      state.linksFromNode(n3), UnorderedElementsAre(Pointee(l2), Pointee(l3)));

  EXPECT_TRUE(state.deleteAdjacencyDatabase(n1).topologyChanged);
  EXPECT_THAT(state.linksFromNode(n1), testing::IsEmpty());
  EXPECT_THAT(state.linksFromNode(n2), UnorderedElementsAre(Pointee(l2)));
  EXPECT_THAT(state.linksFromNode(n3), UnorderedElementsAre(Pointee(l2)));
}

TEST(LinkStateTest, pathAInPathB) {
  auto l1 =
      std::make_shared<openr::Link>(kTestingAreaName, "1", "1/2", "2", "2/1");
  auto l2 =
      std::make_shared<openr::Link>(kTestingAreaName, "2", "2/3", "3", "3/2");
  auto l3 =
      std::make_shared<openr::Link>(kTestingAreaName, "1", "1/3", "3", "3/1");
  openr::LinkState::Path p1, p2;

  EXPECT_TRUE(openr::LinkState::pathAInPathB(p1, p2));
  EXPECT_TRUE(openr::LinkState::pathAInPathB(p2, p1));

  p1.push_back(l1);

  EXPECT_FALSE(openr::LinkState::pathAInPathB(p1, p2));
  EXPECT_TRUE(openr::LinkState::pathAInPathB(p2, p1));

  p2.push_back(l1);

  EXPECT_TRUE(openr::LinkState::pathAInPathB(p1, p2));
  EXPECT_TRUE(openr::LinkState::pathAInPathB(p2, p1));

  p1.push_back(l2);

  EXPECT_FALSE(openr::LinkState::pathAInPathB(p1, p2));
  EXPECT_TRUE(openr::LinkState::pathAInPathB(p2, p1));

  p1.push_back(l3);
  p2.push_back(l2);

  EXPECT_FALSE(openr::LinkState::pathAInPathB(p1, p2));
  EXPECT_TRUE(openr::LinkState::pathAInPathB(p2, p1));

  p1.clear();
  p2.clear();

  p1.push_back(l3);
  p1.push_back(l2);

  p2.push_back(l1);

  EXPECT_FALSE(openr::LinkState::pathAInPathB(p1, p2));
  EXPECT_FALSE(openr::LinkState::pathAInPathB(p2, p1));
}

TEST(LinkStateTest, getKthPaths) {
  {
    //      10
    //   1------2
    //   |      |\
    //  5|   15 | | 35
    //   |      |/
    //   3------4
    //      20
    auto linkState = openr::getLinkState({
        {1, {{2, 10}, {3, 5}}},
        {2, {{1, 10}, {4, 15}, {4, 35}}},
        {3, {{1, 5}, {4, 20}}},
        {4, {{2, 15}, {3, 20}, {2, 35}}},
    });

    auto firstPaths = linkState.getKthPaths("2", "4", 1);
    EXPECT_EQ(firstPaths.size(), 1);
    EXPECT_EQ(firstPaths.at(0).size(), 1);
    EXPECT_EQ(firstPaths.at(0).at(0)->getMetricFromNode("2"), 15);

    auto secondPaths = linkState.getKthPaths("2", "4", 2);
    EXPECT_EQ(secondPaths.size(), 2);
    EXPECT_THAT(secondPaths, UnorderedElementsAre(SizeIs(3), SizeIs(1)));
    // both paths are dist 35
    for (auto const& path : secondPaths) {
      std::string nextNode = "2";
      openr::LinkStateMetric dist = 0;
      for (auto const& link : path) {
        dist += link->getMetricFromNode(nextNode);
        nextNode = link->getOtherNodeName(nextNode);
      }
      EXPECT_EQ(dist, 35);
    }
  }

  {
    // full mesh with parellel links, metric is hop count
    //
    //   1=========2
    //  || \\   // ||
    //  ||  \\ //  ||
    //  ||   \X/   ||
    //  ||  // \\  ||
    //  || //   \\ ||
    //   3=========4
    //
    auto linkState = openr::getLinkState({
        {1, {2, 2, 3, 3, 4, 4}},
        {2, {1, 1, 3, 3, 4, 4}},
        {3, {1, 1, 2, 2, 4, 4}},
        {4, {1, 1, 2, 2, 3, 3}},
    });

    auto firstPaths = linkState.getKthPaths("2", "4", 1);
    EXPECT_EQ(firstPaths.size(), 2);
    EXPECT_THAT(firstPaths, Each(SizeIs(1)));

    auto secondPaths = linkState.getKthPaths("2", "4", 2);
    EXPECT_EQ(secondPaths.size(), 4);
    EXPECT_THAT(secondPaths, Each(SizeIs(2)));

    // verify edge disjoint between all paths
    openr::LinkState::LinkSet set;
    auto allPaths = firstPaths;
    allPaths.insert(allPaths.end(), secondPaths.begin(), secondPaths.end());
    for (auto const& path : allPaths) {
      for (auto const& link : path) {
        EXPECT_TRUE(set.insert(link).second);
      }
    }
  }
}

TEST(LinkStateTest, getHopCounts) {
  {
    // box
    //
    //   1--2
    //   |  |
    //   3--4
    //
    auto linkState = openr::getLinkState({
        {1, {2, 3}},
        {2, {1, 4}},
        {3, {1, 4}},
        {4, {2, 3}},
    });

    EXPECT_EQ(1, linkState.getHopsFromAToB("1", "2"));
    EXPECT_EQ(2, linkState.getHopsFromAToB("1", "4"));
    EXPECT_EQ(2, linkState.getMaxHopsToNode("1"));
  }

  {
    // line
    //
    //   1-2-3-4-5
    //
    auto linkState = openr::getLinkState({
        {1, {2}},
        {2, {1, 3}},
        {3, {2, 4}},
        {4, {3, 5}},
        {5, {4}},
    });

    EXPECT_EQ(1, linkState.getHopsFromAToB("1", "2"));
    EXPECT_EQ(3, linkState.getHopsFromAToB("1", "4"));
    EXPECT_EQ(1, linkState.getHopsFromAToB("2", "3"));
    EXPECT_EQ(4, linkState.getMaxHopsToNode("1"));
    EXPECT_EQ(3, linkState.getMaxHopsToNode("2"));
    EXPECT_EQ(2, linkState.getMaxHopsToNode("3"));
  }

  {
    // disconnect line
    //
    //   1-2-3-4 5
    //
    auto linkState = openr::getLinkState({
        {1, {2}},
        {2, {1, 3}},
        {3, {2, 4}},
        {4, {3}},
        {5, {}},
    });

    EXPECT_FALSE(linkState.getHopsFromAToB("1", "5").has_value());
    EXPECT_EQ(1, linkState.getHopsFromAToB("2", "3"));
    EXPECT_EQ(3, linkState.getMaxHopsToNode("1"));
    EXPECT_EQ(0, linkState.getMaxHopsToNode("5"));
  }
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
