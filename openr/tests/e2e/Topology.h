/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <folly/container/F14Map.h>
#include <openr/tests/e2e/TopologyTypes.h>

namespace openr::e2e {

class Topology final {
 public:
  Topology(
      std::string name,
      const std::vector<NodeSpec>& nodes,
      std::vector<LinkSpec> links = {},
      std::vector<SegmentSpec> segments = {});

  const std::string&
  name() const {
    return name_;
  }
  const folly::F14FastMap<std::string, NodeSpec>&
  nodes() const {
    return nodes_;
  }
  const std::vector<std::string>&
  nodeNames() const {
    return nodeNames_;
  }
  const std::vector<LinkSpec>&
  links() const {
    return links_;
  }
  const std::vector<SegmentSpec>&
  segments() const {
    return segments_;
  }
  bool
  hasNode(const std::string& nodeName) const {
    return nodes_.contains(nodeName);
  }
  const NodeSpec& node(const std::string& nodeName) const;
  const LinkSpec& link(const std::string& linkName) const;
  const SegmentSpec& segment(const std::string& segmentName) const;
  size_t
  nodeCount() const {
    return nodes_.size();
  }
  size_t realNodeCount() const;
  size_t prefixCount() const;

 private:
  std::string name_;
  folly::F14FastMap<std::string, NodeSpec> nodes_;
  std::vector<std::string> nodeNames_;
  std::vector<LinkSpec> links_;
  std::vector<SegmentSpec> segments_;
};

} // namespace openr::e2e
