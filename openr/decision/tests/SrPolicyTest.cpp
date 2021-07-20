/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/decision/SpfSolver.h>

using namespace testing;

namespace openr {

TEST(SrPolicyTest, BasicTest) {
  // Test with two SrPolicyConfigs
  {
    std::vector<openr::thrift::SrPolicy> srPolicyConfigs;

    // Create configuration for two SR Policies
    thrift::SrPolicy srPolicyCfg1;
    srPolicyCfg1.name_ref() = "SR Policy 1";
    srPolicyConfigs.emplace_back(srPolicyCfg1);
    thrift::SrPolicy srPolicyCfg2;
    srPolicyCfg2.name_ref() = "SR Policy 2";
    srPolicyConfigs.emplace_back(srPolicyCfg2);

    SpfSolver spfSolver(
        "node1", false, false, false, false, false, false, srPolicyConfigs);
    EXPECT_EQ(2, spfSolver.getNumSrPolicies());
  }

  // Test empty SrPolicyConfigs
  {
    std::vector<openr::thrift::SrPolicy> srPolicyConfigs;

    SpfSolver spfSolver(
        "node1", false, false, false, false, false, false, srPolicyConfigs);
    EXPECT_EQ(0, spfSolver.getNumSrPolicies());
  }
}

TEST(SrPolicyTest, NoMatchDefaultRules) {
  // Create a single policy
  std::vector<openr::thrift::SrPolicy> srPolicyConfigs;
  thrift::SrPolicy srPolicyCfg1;
  srPolicyCfg1.name_ref() = "SR Policy 1";
  srPolicyConfigs.emplace_back(srPolicyCfg1);
  SpfSolver spfSolver(
      "node1", false, false, false, false, false, false, srPolicyConfigs);

  // Create a dummy routes
  PrefixEntries prefixEntries;
  NodeAndArea nodeAndArea{"node1", "area1"};
  auto route = std::make_shared<thrift::PrefixEntry>();
  route->tags_ref() = {"tag1", "tag2"};
  route->area_stack_ref() = {"area1", "area2"};
  prefixEntries.emplace(nodeAndArea, route);

  // Create dummy best route selection results
  BestRouteSelectionResult results;
  results.bestNodeArea = nodeAndArea;
  results.allNodeAreas = {nodeAndArea};

  // Create dummy areas
  std::unordered_map<std::string, LinkState> areaLinkStates;
  LinkState area1{"area1"};
  LinkState area2{"area2"};
  areaLinkStates.emplace("area1", area1);
  areaLinkStates.emplace("area2", area2);

  // Try to match on the dummy route, default rules should be returned
  auto rules = spfSolver.getRouteComputationRules(
      prefixEntries, results, areaLinkStates);
  EXPECT_EQ(
      thrift::RouteSelectionAlgorithm::SHORTEST_DISTANCE,
      *rules.routeSelectionAlgo_ref());
  EXPECT_EQ(1, rules.areaPathComputationRules_ref()->size());
  thrift::AreaPathComputationRules expectedDefaultRules;
  expectedDefaultRules.forwardingAlgo_ref() =
      thrift::PrefixForwardingAlgorithm::SP_ECMP;
  expectedDefaultRules.forwardingType_ref() = thrift::PrefixForwardingType::IP;
  EXPECT_EQ(
      expectedDefaultRules, rules.areaPathComputationRules_ref()->at("area1"));
  EXPECT_FALSE(rules.prependLabelRules_ref().has_value());
}

} // namespace openr
