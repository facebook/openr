/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/decision/SrPolicy.h"

namespace openr {

class SrPolicyImpl {};

SrPolicy::SrPolicy(
    const thrift::SrPolicy& config,
    const neteng::config::routing_policy::PolicyDefinitions& definitions) {}
SrPolicy::~SrPolicy() = default;

std::optional<thrift::RouteComputationRules>
SrPolicy::matchAndGetRules(
    const std::shared_ptr<thrift::PrefixEntry>& prefixEntry) const {
  return std::nullopt;
}

} // namespace openr
