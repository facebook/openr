/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>

#include <openr/common/NetworkUtil.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>

#pragma once

namespace openr {

/**
 * Represents `thrift::RibPolicyStatement`. Implements the selection and
 * transformation criteria.
 */
class RibPolicyStatement {
 public:
  explicit RibPolicyStatement(
      const thrift::RibPolicyStatement& policyStatement);

  /**
   * Get RibPolicyStatement object in thrift format
   */
  thrift::RibPolicyStatement toThrift() const;

  /**
   * Checks if route qualifies the match criteria for policy statement
   */
  bool match(const thrift::UnicastRoute& route) const;

  /**
   * Transform route. This API apply action on the route only if it is selected.
   *
   * @returns boolean indicating if route is transformed or not.
   */
  bool applyAction(thrift::UnicastRoute& route) const;

 private:
  const std::string name_;

  // Unordered set for efficient lookup on matching
  // NOTE: The matching requires the same prefix representation (fully
  // qualified) We should be using `folly::CIDRNetwork` instead once we migrate
  // to C++ `RibRoute` struct from `thrift::UnicastRoute`
  std::unordered_set<thrift::IpPrefix> prefixSet_;

  // PolicyAction operation
  const thrift::RibRouteAction action_;
};

/**
 * Represents `thrift::RibPolicy`. Defines efficient data structures for
 * efficient processing of policy. Provides APIs for easier code intengration
 * for route policing.
 *
 * Refer to `struct RibPolicy` in `OpenrCtrl.thrift` for more documentation.
 */
class RibPolicy {
 public:
  explicit RibPolicy(thrift::RibPolicy const& ribPolicy);

  /**
   * Get RibPolicy object in thrift format
   */
  thrift::RibPolicy toThrift() const;

  /**
   * Returns number of milliseconds this policy is valid for
   */
  std::chrono::milliseconds getTtlDuration() const;

  /**
   * Is RibPolicy still active
   */
  bool isActive() const;

  /**
   * Checks if route qualifies the match criteria for policy. First successful
   * match with a PolicyStatement will be returned.
   */
  bool match(const thrift::UnicastRoute& route) const;

  /**
   * Transform route. This API apply action on the route only if it is selected.
   *
   * @returns boolean indicating if route is transformed or not.
   */
  bool applyAction(thrift::UnicastRoute& route) const;

 private:
  // List of policy statements
  std::vector<RibPolicyStatement> policyStatements_;

  // Validity
  const std::chrono::steady_clock::time_point validUntilTs_;
};

} // namespace openr
