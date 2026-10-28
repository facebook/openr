/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <openr/tests/e2e/Topology.h>
#include <openr/tests/e2e/TopologyMutationTypes.h>

namespace openr::e2e {

std::vector<DirectAction> planDirectMutations(
    const Topology& before, const Topology& after);

} // namespace openr::e2e
