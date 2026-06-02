/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>

#include <openr/tests/scale/TopologyFactory.h>

#include <openr/tests/scale/if/gen-cpp2/ScaleTestServer_types.h>

namespace openr {

/*
 * Builds a topology of one of the generic types ("fabric", "ring", or "grid")
 * via TopologyGenerator. Shared by the per-build implementations of the
 * createScaleTopology() seam.
 *
 * Returns std::nullopt if cfg.topology()->type() is not one of these generic
 * types, so the caller can decide how to handle it. Throws thrift::SetupError
 * (reason TOPOLOGY_INVALID) when the type IS generic but a required field is
 * unset.
 */
std::optional<Topology> createGenericTopology(
    const thrift::ScaleTestConfig& cfg);

/*
 * Flag-derived counterpart for the standalone ScaleTestServer binary. Returns
 * std::nullopt if params.type is not a generic type.
 */
std::optional<Topology> createGenericTopologyFromParams(
    const ScaleTopologyParams& params);

} // namespace openr
