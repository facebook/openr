/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

#include <folly/container/F14Set.h>
#include <openr/tests/e2e/AdjacencyMaterializer.h>
#include <openr/tests/e2e/Topology.h>

namespace openr::e2e {

std::vector<SerializedKvStoreKey> materializePrefixes(
    const Topology& topology,
    const folly::F14FastSet<std::string>& selectedNodes = {});

} // namespace openr::e2e
