/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <configerator/structs/neteng/config/gen-cpp2/routing_policy_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/policy/PolicyStructs.h>

namespace openr {
// Forward declaration
class PolicyManagerImpl;

/**
 * PolicyManager manages all policies defined in the config file.
 */
class PolicyManager {
 public:
  explicit PolicyManager(
      const neteng::config::routing_policy::PolicyConfig& config);
  ~PolicyManager();

  std::pair<std::shared_ptr<thrift::PrefixEntry>, std::string /*policy name*/>
  applyPolicy(
      const std::string& policyStatementName,
      const std::shared_ptr<thrift::PrefixEntry>& prefixEntry,
      const std::optional<OpenrPolicyActionData>& policyActionData =
          std::nullopt) noexcept;

  // PolicyManagerImpl uses forward declaration
  // Use shared_ptr because it works with incomplete type, where unique_ptr
  // requires full declaration
  std::shared_ptr<PolicyManagerImpl> impl_{nullptr};
};
} // namespace openr
