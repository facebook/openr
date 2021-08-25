/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/policy/PolicyManager.h>

namespace openr {

class PolicyManagerImpl {};

PolicyManager::PolicyManager(
    const neteng::config::routing_policy::PolicyConfig& config) {}
PolicyManager::~PolicyManager() = default;

std::pair<std::shared_ptr<thrift::PrefixEntry>, std::string /*policy name*/>
PolicyManager::applyPolicy(
    const std::string& policyStatementName,
    const std::shared_ptr<thrift::PrefixEntry>& prefixEntry,
    const std::optional<OpenrPolicyActionData>& policyActionData) noexcept {
  return {prefixEntry, "Always Allow"};
}

} // namespace openr
