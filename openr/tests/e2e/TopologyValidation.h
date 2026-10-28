/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>
#include <openr/tests/e2e/TopologyTypes.h>

namespace openr::e2e {

void validateNetwork(
    const folly::CIDRNetwork& network,
    const std::string& owner,
    bool requireCanonical);
void validateTopology(
    const std::string& name,
    const folly::F14FastMap<std::string, NodeSpec>& nodes);

} // namespace openr::e2e
