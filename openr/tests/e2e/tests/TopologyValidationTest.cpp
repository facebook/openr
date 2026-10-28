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
#include <openr/tests/e2e/TopologyValidation.h>

namespace openr::e2e {

namespace {

folly::F14FastMap<std::string, NodeSpec>
nodesWithPrefix(const std::string& prefix, std::string area = "0") {
  NodeSpec node;
  node.name = "r1";
  node.areas = {"0", "pod"};
  PrefixSpec spec;
  spec.prefix = folly::IPAddress::createNetwork(prefix);
  spec.area = std::move(area);
  node.prefixes.push_back(std::move(spec));
  folly::F14FastMap<std::string, NodeSpec> nodes;
  const auto nodeName = node.name;
  nodes.emplace(nodeName, std::move(node));
  return nodes;
}

} // namespace

TEST(TopologyValidationTest, ValidateTopology_EmptyTopology_RejectsInput) {
  const folly::F14FastMap<std::string, NodeSpec> nodes;
  EXPECT_THROW(validateTopology("", nodes), TopologyError);
  EXPECT_THROW(validateTopology("empty", nodes), TopologyError);
}

TEST(TopologyValidationTest, ValidateTopology_InvalidPrefix_RejectsInput) {
  auto nonCanonical = nodesWithPrefix("10.0.0.0/16");
  nonCanonical.at("r1").prefixes.front().prefix.first =
      folly::IPAddress{"10.1.1.1"};
  EXPECT_THROW(validateTopology("non-canonical", nonCanonical), TopologyError);
  EXPECT_THROW(
      validateTopology(
          "unknown-area", nodesWithPrefix("10.0.0.0/8", "missing")),
      TopologyError);
}

TEST(TopologyValidationTest, ValidateTopology_DuplicatePrefixKey_RejectsTypes) {
  auto nodes = nodesWithPrefix("10.0.0.0/8");
  auto duplicate = nodes.at("r1").prefixes.front();
  duplicate.type = thrift::PrefixType::BGP;
  nodes.at("r1").prefixes.push_back(std::move(duplicate));
  EXPECT_THROW(validateTopology("duplicate", nodes), TopologyError);
}

TEST(TopologyValidationTest, ValidateTopology_InvalidNode_RejectsInput) {
  NodeSpec node;
  node.name = "r1";
  node.areas = {"pod", "pod"};
  folly::F14FastMap<std::string, NodeSpec> nodes{{node.name, node}};
  EXPECT_THROW(validateTopology("areas", nodes), TopologyError);
  nodes.at("r1").areas = {"pod"};
  nodes.at("r1").nodeLabel = 0;
  EXPECT_THROW(validateTopology("label", nodes), TopologyError);
}

} // namespace openr::e2e
