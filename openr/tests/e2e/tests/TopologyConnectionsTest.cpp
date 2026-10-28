/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>

#include <gtest/gtest.h>
#include <openr/tests/e2e/TopologyConnections.h>

namespace openr::e2e {

using namespace std::chrono_literals;

TEST(TopologyConnectionsTest, MakeLink_OverridesEndpointsIndependently) {
  LinkOptions options;
  options.name = "primary";
  options.metric = 10;
  options.first.interfaceName = "r1-primary";
  options.first.metric = 20;
  options.first.latency = 7ms;
  options.second.interfaceName = "r2-primary";
  options.second.metric = 30;

  const auto link = makeLink("r1", "r2", 2, options);

  EXPECT_EQ("primary", link.name);
  EXPECT_EQ("r1-primary", link.first.interfaceName);
  EXPECT_EQ(20, link.first.metric);
  EXPECT_EQ(7ms, link.first.latency);
  EXPECT_EQ("r2-primary", link.second.interfaceName);
  EXPECT_EQ(30, link.second.metric);
}

TEST(TopologyConnectionsTest, MakeSharedSegment_AppliesCommonOptions) {
  SegmentOptions options;
  options.area = "pod";
  options.metric = 25;

  const auto segment = makeSharedSegment("lan", {"r1", "r2", "r3"}, options);

  ASSERT_EQ(3, segment.endpoints.size());
  EXPECT_EQ("if_r1_lan", segment.endpoints.at(0).interfaceName);
  EXPECT_EQ("r2", segment.endpoints.at(1).nodeName);
  EXPECT_EQ("pod", segment.endpoints.at(2).area);
  EXPECT_EQ(25, segment.endpoints.at(2).metric);
}

} // namespace openr::e2e
