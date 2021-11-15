/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

// Forward declaration
class SrPolicyImpl;

/**
 * Implementation of a single SR Policy
 */
class SrPolicy {
 public:
  explicit SrPolicy(
      const thrift::SrPolicy& srPolicyConfig,
      const neteng::config::routing_policy::PolicyDefinitions& definitions);
  ~SrPolicy();

  // Walks all SR Policy matchers. If they all match then the SR Policy rules
  // are returned.
  std::optional<thrift::RouteComputationRules> matchAndGetRules(
      const std::shared_ptr<thrift::PrefixEntry>& prefixEntry) const;

  // SrPolicyMactcher uses forward declaration
  // Use shared_ptr because it works with incomplete type, where unique_ptr
  // requires full declaration
  std::shared_ptr<SrPolicyImpl> impl_{nullptr};
};
} // namespace openr
