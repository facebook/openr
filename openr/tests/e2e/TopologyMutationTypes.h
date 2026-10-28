/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <openr/tests/e2e/TopologyTypes.h>

namespace openr::e2e {

struct SetLinkState {
  std::string link;
  bool operational{true};
};

struct SetLinkMetric {
  std::string link;
  std::optional<std::string> node;
  int32_t metric{1};
};

struct SetLinkLatency {
  std::string link;
  std::optional<std::string> node;
  std::chrono::milliseconds latency{1};
};

struct SetLinkOverload {
  std::string link;
  std::optional<std::string> node;
  bool overloaded{false};
};

struct SetNodeState {
  std::string node;
  bool operational{true};
};

struct SetNodeOverload {
  std::string node;
  bool overloaded{false};
};

struct AdvertisePrefix {
  std::string node;
  PrefixSpec prefix;
};

struct WithdrawPrefix {
  std::string node;
  folly::CIDRNetwork prefix;
  std::string area;
  thrift::PrefixType type{thrift::PrefixType::LOOPBACK};
};

struct SetPartition {
  std::vector<std::string> links;
  bool partitioned{true};
};

using TopologyMutation = std::variant<
    SetLinkState,
    SetLinkMetric,
    SetLinkLatency,
    SetLinkOverload,
    SetNodeState,
    SetNodeOverload,
    AdvertisePrefix,
    WithdrawPrefix,
    SetPartition>;

enum class DirectActionTarget { NODE, VIRTUAL_NETWORK };

struct DirectAction {
  DirectActionTarget target{DirectActionTarget::NODE};
  std::string node;
  TopologyMutation mutation;
};

} // namespace openr::e2e
