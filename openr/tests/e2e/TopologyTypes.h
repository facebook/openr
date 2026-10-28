/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <folly/IPAddress.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr::thrift {
class OpenrConfig;
}

namespace openr::e2e {
enum class NodeKind { REAL, SYNTHETIC };
struct PrefixSpec {
  folly::CIDRNetwork prefix;
  thrift::PrefixType type{thrift::PrefixType::LOOPBACK};
  std::string area;
  std::vector<std::string> tags;
};
struct NodeSpec {
  std::string name;
  NodeKind kind{NodeKind::SYNTHETIC};
  std::vector<std::string> areas{"0"};
  std::vector<PrefixSpec> prefixes;
  std::optional<int32_t> nodeLabel;
  bool overloaded{false};
  bool operational{true};
  std::function<void(thrift::OpenrConfig&)> customizeConfig;
};
struct NodeOptions {
  std::vector<std::string> areas{"0"};
  std::optional<int32_t> nodeLabel;
  bool overloaded{false};
  std::function<void(thrift::OpenrConfig&)> customizeConfig;
};
struct PrefixOptions {
  std::optional<std::string> area;
  thrift::PrefixType type{thrift::PrefixType::LOOPBACK};
  std::vector<std::string> tags;
};
struct LinkEndpointSpec {
  std::string nodeName;
  std::string interfaceName;
  std::optional<int32_t> interfaceIndex;
  std::vector<folly::CIDRNetwork> addresses;
  std::string area{"0"};
  int32_t metric{1};
  std::chrono::milliseconds latency{1};
  bool overloaded{false};
  std::optional<int32_t> adjacencyLabel;
};

struct LinkSpec {
  std::string name;
  LinkEndpointSpec first;
  LinkEndpointSpec second;
  bool operational{true};
};

struct SegmentSpec {
  std::string name;
  std::vector<LinkEndpointSpec> endpoints;
};

class TopologyError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

} // namespace openr::e2e
