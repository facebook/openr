/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <utility>

#include <openr/tests/scale/Session.h>
#include <openr/tests/scale/if/gen-cpp2/ScaleTestServer_types.h>

namespace openr::test {

// Pure-topology config: no DUT connect, no fake kvstore, no spark — suitable
// for ctor-only / pure-logic tests.
thrift::ScaleTestConfig MakeTestConfig();

// Walks the session's topology and returns the first (router, adjacency)
// pair found. Throws std::runtime_error if the topology has no adjacencies.
// Used by downLink/upLink tests.
std::pair<std::string, std::string> FindAdjacentPair(const Session& session);

} // namespace openr::test
