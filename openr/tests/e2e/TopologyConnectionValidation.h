/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

#include <folly/container/F14Map.h>
#include <openr/tests/e2e/TopologyTypes.h>

namespace openr::e2e {

void validateConnections(
    const folly::F14FastMap<std::string, NodeSpec>& nodes,
    const std::vector<LinkSpec>& links,
    const std::vector<SegmentSpec>& segments);

} // namespace openr::e2e
