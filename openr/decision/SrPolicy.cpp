/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/decision/SrPolicy.h"

namespace openr {

SrPolicy::SrPolicy(const thrift::SrPolicy& config)
    : name_(*config.name_ref()),
      description_(*config.description_ref()),
      rules_(*config.rules_ref()) {}

SrPolicy::~SrPolicy() = default;

std::optional<thrift::RouteComputationRules>
SrPolicy::matchAndGetRules(
    const std::shared_ptr<thrift::PrefixEntry>& prefixEntry) const {
  // TODO: Walk matchers and check if they all match. If they do return
  // rules
  return std::nullopt;
}

} // namespace openr
