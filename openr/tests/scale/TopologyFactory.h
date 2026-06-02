/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include <string>

#include <openr/tests/scale/VirtualRouter.h>

#include <openr/tests/scale/if/gen-cpp2/ScaleTestServer_types.h>

namespace openr {

/*
 * Builds the simulated Topology described by cfg.topology(). This is a
 * build-time seam: the implementation is selected at link time, so different
 * builds can support different topology types. Throws thrift::SetupError
 * (reason TOPOLOGY_INVALID) if cfg is missing required fields or names a
 * topology type this build does not support.
 */
Topology createScaleTopology(const thrift::ScaleTestConfig& cfg);

/*
 * Flag-derived parameters for the standalone ScaleTestServer binary, which
 * builds its topology directly from command-line gflags rather than from a
 * thrift::ScaleTestConfig.
 */
struct ScaleTopologyParams {
  std::string type;
  int numSpines{0};
  int numLeaves{0};
  int numSuperSpines{0};
  int numPods{0};
  int numPrefixesPerNode{0};
  int numSites{0};
};

/*
 * Builds a topology from flag-derived parameters (same build-time seam as
 * createScaleTopology()). Returns std::nullopt if params.type is not a topology
 * type this build supports, so the CLI can report the error and exit without
 * relying on exceptions.
 */
std::optional<Topology> createScaleTopologyFromParams(
    const ScaleTopologyParams& params);

} // namespace openr
