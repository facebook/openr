/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <folly/container/F14Map.h>
#include <openr/tests/e2e/Topology.h>
#include <openr/tests/e2e/TopologyConnections.h>

namespace openr::e2e {

class TopologyBuilder final {
 public:
  explicit TopologyBuilder(std::string name);

  TopologyBuilder& node(NodeSpec node);
  TopologyBuilder& router(std::string name, NodeOptions options = {});
  TopologyBuilder& peer(std::string name, NodeOptions options = {});
  TopologyBuilder& realNode(std::string name, NodeOptions options = {});
  TopologyBuilder& syntheticNode(std::string name, NodeOptions options = {});
  TopologyBuilder& prefix(std::string prefix, PrefixOptions options = {});
  TopologyBuilder& prefix(
      const std::string& node, std::string prefix, PrefixOptions options = {});
  TopologyBuilder& link(LinkSpec link);
  TopologyBuilder& link(
      const std::string& first,
      const std::string& second,
      const LinkOptions& options = {});
  TopologyBuilder& segment(SegmentSpec segment);
  TopologyBuilder& sharedSegment(
      std::string name,
      const std::vector<std::string>& nodes,
      const SegmentOptions& options = {});
  Topology build();

 private:
  NodeSpec& findNode(const std::string& name);
  size_t nextLinkOrdinal(
      const std::string& first, const std::string& second) const;

  std::string name_;
  std::vector<NodeSpec> nodes_;
  std::vector<LinkSpec> links_;
  std::vector<SegmentSpec> segments_;
  std::optional<std::string> lastNode_;
  folly::F14FastMap<std::string, size_t> nodeIndexes_;
  folly::F14FastMap<std::pair<std::string, std::string>, size_t> linkCounts_;
};

} // namespace openr::e2e
