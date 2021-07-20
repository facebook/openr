/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/decision/LinkState.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>

namespace openr {

/**
 * Implementation of a single SR Policy
 */
class SrPolicy {
 public:
  explicit SrPolicy(const thrift::SrPolicy& config);
  ~SrPolicy();

  // Walks all SR Policy matchers. If they all match then the SR Policy rules
  // are returned.
  std::optional<thrift::RouteComputationRules> matchAndGetRules(
      const std::shared_ptr<thrift::PrefixEntry>& prefixEntry) const;

 private:
  // SR Policy name
  const std::string name_;
  // SR Policy description
  const std::string description_;
  // SR Policy rules
  const thrift::RouteComputationRules rules_;
};
} // namespace openr
