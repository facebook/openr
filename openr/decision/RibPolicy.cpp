/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/decision/RibPolicy.h>

#include <folly/MapUtil.h>

namespace openr {

//
// RibPolicyStatement
//

RibPolicyStatement::RibPolicyStatement(const thrift::RibPolicyStatement& stmt)
    : name_(stmt.name), action_(stmt.action) {
  // Verify that at-least one action must be specified
  if (not stmt.action.set_weight_ref()) {
    thrift::OpenrError error;
    error.message = "Missing policy_statement.action.set_weight attribute";
    throw error;
  }

  // Verify that at-least one match criteria must be specified
  if (not stmt.matcher.prefixes_ref()) {
    thrift::OpenrError error;
    error.message = "Missing policy_statement.matcher.prefixes attribute";
    throw error;
  }

  // Populate the match fields
  prefixSet_.insert(
      stmt.matcher.prefixes_ref()->begin(), stmt.matcher.prefixes_ref()->end());
}

thrift::RibPolicyStatement
RibPolicyStatement::toThrift() const {
  thrift::RibPolicyStatement stmt;
  stmt.name = name_;
  stmt.action = action_;
  stmt.matcher.prefixes_ref() = std::vector<thrift::IpPrefix>();
  for (auto const& prefix : prefixSet_) {
    stmt.matcher.prefixes_ref()->emplace_back(prefix);
  }
  return stmt;
}

bool
RibPolicyStatement::match(const thrift::UnicastRoute& route) const {
  return prefixSet_.count(route.dest) > 0;
}

bool
RibPolicyStatement::applyAction(thrift::UnicastRoute& route) const {
  if (not match(route)) {
    return false;
  }

  // Iterate over all next-hops. NOTE that we iterate over rvalue
  CHECK(action_.set_weight_ref().has_value());
  auto const& weightAction = action_.set_weight_ref().value();
  std::vector<thrift::NextHopThrift> newNextHops;
  for (auto& nh : route.nextHops) {
    auto new_weight = weightAction.default_weight;
    if (nh.area_ref()) {
      new_weight = folly::get_default(
          weightAction.area_to_weight,
          nh.area_ref().value(),
          weightAction.default_weight);
    }
    if (new_weight > 0) {
      nh.weight = new_weight;
      newNextHops.emplace_back(std::move(nh));
    }
    // We skip the next-hop with weight=0
  }
  route.nextHops = std::move(newNextHops);

  return true;
}

//
// RibPolicy
//

RibPolicy::RibPolicy(thrift::RibPolicy const& policy)
    : validUntilTs_(
          std::chrono::steady_clock::now() +
          std::chrono::seconds(policy.ttl_secs)) {
  if (policy.statements.empty()) {
    thrift::OpenrError error;
    error.message = "Missing policy.statements attribute";
    throw error;
  }

  // Populate policy statements
  for (auto const& statement : policy.statements) {
    policyStatements_.emplace_back(RibPolicyStatement(statement));
  }
}

thrift::RibPolicy
RibPolicy::toThrift() const {
  thrift::RibPolicy policy;

  // Set statements
  for (auto const& statement : policyStatements_) {
    policy.statements.emplace_back(statement.toThrift());
  }

  // Set ttl_secs
  policy.ttl_secs = std::chrono::duration_cast<std::chrono::seconds>(
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
RibPolicy::match(const thrift::UnicastRoute& route) const {
  for (auto const& statement : policyStatements_) {
    if (statement.match(route)) {
      return true;
    }
  }
  return false;
}

bool
RibPolicy::applyAction(thrift::UnicastRoute& route) const {
  for (auto const& statement : policyStatements_) {
    if (statement.applyAction(route)) {
      return true;
    }
  }
  return false;
}

} // namespace openr
