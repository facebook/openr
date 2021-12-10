/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>

namespace openr {

// OpenrPolicyActionData - capture any data that's needed to apply a action
// but can NOT be pre-configured in Policy Configuration
// (e.g some data needs to be dynamically derived on the fly).
struct OpenrPolicyActionData {
  explicit OpenrPolicyActionData(int64_t weight) : weight(weight) {}

  // Prefix Entry weight (AKA BGP link bandwidth attribute).
  std::optional<int64_t> weight;

  bool
  operator==(const OpenrPolicyActionData& other) const {
    return weight == other.weight;
  }
};

/**
 * Additional information for policy matching
 *
 * Additional information that should not be transmitted and reused
 * apart of policy matching
 */

struct OpenrPolicyMatchData {
  unsigned int igpCost = 0;
  explicit OpenrPolicyMatchData(unsigned int igpCost = 0) : igpCost(igpCost) {}

  bool
  operator==(const OpenrPolicyMatchData& other) const {
    return igpCost == other.igpCost;
  }
};

} // namespace openr
