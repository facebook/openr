/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/decision/RibPolicy.h>

#include <fb303/ServiceData.h>
#include <folly/MapUtil.h>

namespace openr {

//
// RibPolicyStatement
//

RibPolicyStatement::RibPolicyStatement(const thrift::RibPolicyStatement& stmt)
    : name_(*stmt.name_ref()), action_(*stmt.action_ref()) {
  // Verify that at-least one action must be specified
  if (not stmt.action_ref()->set_weight_ref()) {
    thrift::OpenrError error;
    *error.message_ref() =
        "Missing policy_statement.action.set_weight attribute";
    throw error;
  }

  // Verify that at-least one match criteria must be specified
  if (not stmt.matcher_ref()->prefixes_ref()) {
    thrift::OpenrError error;
    *error.message_ref() =
        "Missing policy_statement.matcher.prefixes attribute";
    throw error;
  }

  // Populate the match fields
  for (const auto& tPrefix : *stmt.matcher_ref()->prefixes_ref()) {
    prefixSet_.insert(toIPNetwork(tPrefix));
  }
}

thrift::RibPolicyStatement
RibPolicyStatement::toThrift() const {
  thrift::RibPolicyStatement stmt;
  *stmt.name_ref() = name_;
  *stmt.action_ref() = action_;
  stmt.matcher_ref()->prefixes_ref() = std::vector<thrift::IpPrefix>();
  for (auto const& prefix : prefixSet_) {
    stmt.matcher_ref()->prefixes_ref()->emplace_back(toIpPrefix(prefix));
  }
  return stmt;
}

bool
RibPolicyStatement::match(const RibUnicastEntry& route) const {
  return prefixSet_.count(route.prefix) > 0;
}

bool
RibPolicyStatement::applyAction(RibUnicastEntry& route) const {
  if (not match(route)) {
    return false;
  }

  // Iterate over all next-hops. NOTE that we iterate over rvalue
  CHECK(action_.set_weight_ref().has_value());
  auto const& weightAction = action_.set_weight_ref().value();
  std::unordered_set<thrift::NextHopThrift> newNexthops;
  for (auto& nh : route.nexthops) {
    // Next-hop inherits a RibPolicy weight with the following precedence
    // 1. Neighbor weight
    // 2. Area weight
    // 3. Default weight
    auto new_weight = *weightAction.default_weight_ref();
    if (nh.area_ref()) {
      new_weight = folly::get_default(
          *weightAction.area_to_weight_ref(),
          nh.area_ref().value(),
          new_weight);
    }
    if (nh.neighborNodeName_ref()) {
      new_weight = folly::get_default(
          *weightAction.neighbor_to_weight_ref(),
          nh.neighborNodeName_ref().value(),
          new_weight);
    }
    if (new_weight > 0) {
      auto newNh = nh;
      newNh.weight_ref() = new_weight;
      newNexthops.emplace(std::move(newNh));
    }
    // We skip the next-hop with weight=0
  }

  // Retain existing next-hops if new next-hops is empty
  // NOTE: In future we may modify this code to also support dropping
  //       routes with no-invalid next-hops
  if (newNexthops.empty()) {
    LOG(WARNING) << "RibPolicy invalidated all next-hops for route to "
                 << folly::IPAddress::networkToString(route.prefix);
    facebook::fb303::fbData->addStatValue(
        "decision.rib_policy.invalidated_routes", 1, facebook::fb303::COUNT);
    return false;
  }

  // Update route next-hops
  route.nexthops = std::move(newNexthops);

  return true;
}

//
// RibPolicy
//

RibPolicy::RibPolicy(thrift::RibPolicy const& policy)
    : validUntilTs_(
          std::chrono::steady_clock::now() +
          std::chrono::seconds(*policy.ttl_secs_ref())) {
  if (policy.statements_ref()->empty()) {
    thrift::OpenrError error;
    *error.message_ref() = "Missing policy.statements attribute";
    throw error;
  }

  // Populate policy statements
  for (auto const& statement : *policy.statements_ref()) {
    policyStatements_.emplace_back(RibPolicyStatement(statement));
  }
}

thrift::RibPolicy
RibPolicy::toThrift() const {
  thrift::RibPolicy policy;

  // Set statements
  for (auto const& statement : policyStatements_) {
    policy.statements_ref()->emplace_back(statement.toThrift());
  }

  // Set ttl_secs
  policy.ttl_secs_ref() = std::chrono::duration_cast<std::chrono::seconds>(
                              validUntilTs_ - std::chrono::steady_clock::now())
                              .count();

  return policy;
}

std::chrono::milliseconds
RibPolicy::getTtlDuration() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      validUntilTs_ - std::chrono::steady_clock::now());
}

bool
RibPolicy::isActive() const {
  return getTtlDuration().count() > 0;
}

bool
RibPolicy::match(const RibUnicastEntry& route) const {
  for (auto const& statement : policyStatements_) {
    if (statement.match(route)) {
      return true;
    }
  }
  return false;
}

bool
RibPolicy::applyAction(RibUnicastEntry& route) const {
  for (auto const& statement : policyStatements_) {
    if (statement.applyAction(route)) {
      return true;
    }
  }
  return false;
}

RibPolicy::PolicyChange
RibPolicy::applyPolicy(std::unordered_map<folly::CIDRNetwork, RibUnicastEntry>&
                           unicastEntries) const {
  PolicyChange change;
  if (not isActive()) {
    return change;
  }
  auto iter = unicastEntries.begin();
  while (iter != unicastEntries.end()) {
    if (applyAction(iter->second)) {
      DCHECK(iter->second.nexthops.size()) << "Unexpected empty next-hops";
      change.updatedRoutes.push_back(iter->second.prefix);
      VLOG(2) << "RibPolicy transformed the route "
              << folly::IPAddress::networkToString(iter->second.prefix);
    }
    ++iter;
  }
  return change;
}

} // namespace openr
