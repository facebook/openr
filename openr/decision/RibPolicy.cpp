/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/decision/RibPolicy.h>

#include <fb303/ServiceData.h>
#include <folly/MapUtil.h>
#include <folly/logging/xlog.h>

namespace openr {

//
// RibPolicyStatement
//

RibPolicyStatement::RibPolicyStatement(const thrift::RibPolicyStatement& stmt)
    : name_(*stmt.name()),
      action_(*stmt.action()),
      counterID_(stmt.counterID().to_optional()) {
  // Verify that at-least one action must be specified
  if (!stmt.action()->set_weight()) {
    thrift::OpenrError error;
    *error.message() = "Missing policy_statement.action.set_weight attribute";
    throw error;
  }

  // Verify that at-least one match criteria must be specified
  if (!stmt.matcher()->prefixes() && !stmt.matcher()->tags()) {
    thrift::OpenrError error;
    *error.message() =
        "Missing policy_statement.matcher.prefixes or policy_statement.matcher.tags attribute";
    throw error;
  }

  // Populate the match fields
  if (stmt.matcher()->prefixes()) {
    for (const auto& tPrefix : *stmt.matcher()->prefixes()) {
      prefixSet_.insert(toIPNetwork(tPrefix));
    }
  }
  if (stmt.matcher()->tags()) {
    for (const auto& tTag : *stmt.matcher()->tags()) {
      tagSet_.insert(tTag);
    }
  }
}

thrift::RibPolicyStatement
RibPolicyStatement::toThrift() const {
  thrift::RibPolicyStatement stmt;
  *stmt.name() = name_;
  *stmt.action() = action_;
  stmt.counterID().from_optional(counterID_);
  if (!prefixSet_.empty()) {
    stmt.matcher()->prefixes() = std::vector<thrift::IpPrefix>();
    for (auto const& prefix : prefixSet_) {
      stmt.matcher()->prefixes()->emplace_back(toIpPrefix(prefix));
    }
  }
  if (!tagSet_.empty()) {
    stmt.matcher()->tags() = std::vector<std::string>();
    for (auto const& tag : tagSet_) {
      stmt.matcher()->tags()->emplace_back(tag);
    }
  }

  return stmt;
} // namespace openr

bool
RibPolicyStatement::match(const RibUnicastEntry& route) const {
  if (tagSet_.empty() && prefixSet_.empty()) {
    return false;
  }

  // Attempt to match the route on tags if populated in the RibPolicy statement
  bool tagMatch{false};
  if (tagSet_.empty()) {
    tagMatch = true;
  } else {
    for (const auto& tag : tagSet_) {
      // Find a match with at least one tag in the RibPolicyStatement
      const auto tagFoundIt = route.bestPrefixEntry.tags()->find(tag);
      if (tagFoundIt != route.bestPrefixEntry.tags()->end()) {
        tagMatch = true;
        break;
      }
    }
  }

  // Attempt to match the route on prefix if populated in the RibPolicy
  // statement
  bool prefixMatch{false};
  if (prefixSet_.empty()) {
    prefixMatch = true;
  } else {
    // Find a match with at least one prefix in the RibPolicyStatement
    prefixMatch = prefixSet_.count(route.prefix) > 0;
  }

  // Verify both tag and prefix matchers are successful
  return tagMatch && prefixMatch;
}

bool
RibPolicyStatement::applyAction(RibUnicastEntry& route) const {
  if (!match(route)) {
    return false;
  }

  // Assign RibPolicyStatement route counter ID to the route
  route.counterID = counterID_;

  // Iterate over all next-hops. NOTE that we iterate over rvalue
  CHECK(action_.set_weight().has_value());
  auto const& weightAction = action_.set_weight().value();
  std::unordered_set<thrift::NextHopThrift> newNexthops;
  for (auto& nh : route.nexthops) {
    // Next-hop inherits a RibPolicy weight with the following precedence
    // 1. Neighbor weight
    // 2. Area weight
    // 3. Default weight
    auto new_weight = *weightAction.default_weight();
    if (nh.area()) {
      new_weight = folly::get_default(
          *weightAction.area_to_weight(), nh.area().value(), new_weight);
    }
    if (nh.neighborNodeName()) {
      new_weight = folly::get_default(
          *weightAction.neighbor_to_weight(),
          nh.neighborNodeName().value(),
          new_weight);
    }
    if (new_weight > 0) {
      auto newNh = nh;
      newNh.weight() = new_weight;
      newNexthops.emplace(std::move(newNh));
    }
    // We skip the next-hop with weight=0
  }

  // Retain existing next-hops if new next-hops is empty
  // NOTE: In future we may modify this code to also support dropping
  //       routes with no-invalid next-hops
  if (newNexthops.empty()) {
    XLOG(WARNING) << "RibPolicy invalidated all next-hops for route to "
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
          std::chrono::seconds(*policy.ttl_secs())) {
  if (policy.statements()->empty()) {
    thrift::OpenrError error;
    *error.message() = "Missing policy.statements attribute";
    throw error;
  }

  // Populate policy statements
  for (auto const& statement : *policy.statements()) {
    policyStatements_.emplace_back(statement);
  }
}

thrift::RibPolicy
RibPolicy::toThrift() const {
  thrift::RibPolicy policy;

  // Set statements
  for (auto const& statement : policyStatements_) {
    policy.statements()->emplace_back(statement.toThrift());
  }

  // Set ttl_secs
  policy.ttl_secs() = std::chrono::duration_cast<std::chrono::seconds>(
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
  if (!isActive()) {
    return change;
  }
  auto iter = unicastEntries.begin();
  while (iter != unicastEntries.end()) {
    if (applyAction(iter->second)) {
      DCHECK(iter->second.nexthops.size()) << "Unexpected empty next-hops";
      change.updatedRoutes.push_back(iter->second.prefix);
      XLOG(DBG2) << "RibPolicy transformed the route "
                 << folly::IPAddress::networkToString(iter->second.prefix);
    }
    ++iter;
  }
  return change;
}

} // namespace openr
