/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <folly/IPAddress.h>
#include <openr/tests/e2e/TopologyTypes.h>

namespace openr::e2e {

struct LinkEndpointOptions {
  std::optional<std::string> interfaceName;
  std::optional<int32_t> interfaceIndex;
  std::vector<folly::CIDRNetwork> addresses;
  std::optional<std::string> area;
  std::optional<int32_t> metric;
  std::optional<std::chrono::milliseconds> latency;
  std::optional<bool> overloaded;
  std::optional<int32_t> adjacencyLabel;
};

struct LinkOptions {
  std::string name;
  std::string area{"0"};
  int32_t metric{1};
  std::chrono::milliseconds latency{1};
  bool overloaded{false};
  std::optional<int32_t> adjacencyLabel;
  LinkEndpointOptions first;
  LinkEndpointOptions second;
};

struct SegmentOptions {
  std::string area{"0"};
  int32_t metric{1};
  std::chrono::milliseconds latency{1};
  bool overloaded{false};
  std::optional<int32_t> adjacencyLabel;
};

LinkSpec makeLink(
    const std::string& firstNode,
    const std::string& secondNode,
    size_t ordinal,
    const LinkOptions& options);
SegmentSpec makeSharedSegment(
    std::string name,
    const std::vector<std::string>& nodeNames,
    const SegmentOptions& options);

} // namespace openr::e2e
