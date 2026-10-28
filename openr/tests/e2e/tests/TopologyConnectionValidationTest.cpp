/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string>
#include <utility>

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>
#include <gtest/gtest.h>
#include <openr/tests/e2e/TopologyConnectionValidation.h>

namespace openr::e2e {

namespace {

folly::F14FastMap<std::string, NodeSpec>
nodes() {
  NodeSpec r1;
  r1.name = "r1";
  r1.areas = {"pod", "plane"};
  NodeSpec r2 = r1;
  r2.name = "r2";
  NodeSpec r3 = r1;
  r3.name = "r3";
  return {{r1.name, r1}, {r2.name, r2}, {r3.name, r3}};
}

LinkSpec
link(std::string firstArea = "pod", std::string secondArea = "pod") {
  LinkSpec result;
  result.name = "link";
  result.first.nodeName = "r1";
  result.first.interfaceName = "if1";
  result.first.area = std::move(firstArea);
  result.second.nodeName = "r2";
  result.second.interfaceName = "if2";
  result.second.area = std::move(secondArea);
  return result;
}

} // namespace

TEST(TopologyConnectionValidationTest, Link_InvalidAttributes_RejectsInput) {
  EXPECT_THROW(
      validateConnections(nodes(), {link("pod", "plane")}, {}), TopologyError);
  auto family = link();
  family.first.addresses = {folly::IPAddress::createNetwork("192.0.2.1/31")};
  family.second.addresses = {
      folly::IPAddress::createNetwork("2001:db8::1/127")};
  EXPECT_THROW(validateConnections(nodes(), {family}, {}), TopologyError);
  auto metric = link();
  metric.first.metric = 0;
  EXPECT_THROW(validateConnections(nodes(), {metric}, {}), TopologyError);
}

TEST(TopologyConnectionValidationTest, Connections_InvalidReuse_RejectsInput) {
  auto second = link();
  second.name = "second";
  second.second.nodeName = "r3";
  second.second.interfaceName = "if3";
  second.second.area = "pod";
  EXPECT_THROW(
      validateConnections(nodes(), {link(), second}, {}), TopologyError);

  SegmentSpec segment;
  segment.name = "lan";
  segment.endpoints = {link().first, link("plane", "plane").second};
  EXPECT_THROW(validateConnections(nodes(), {}, {segment}), TopologyError);
  segment.endpoints = {link().first};
  EXPECT_THROW(validateConnections(nodes(), {}, {segment}), TopologyError);
}

} // namespace openr::e2e
