/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Open-source implementation of the createScaleTopology() seam declared in
 * TopologyFactory.h. It supports the generic topology types (fabric, ring,
 * grid) via GenericTopologies; any other type is rejected with a SetupError.
 * Other builds may link a different implementation of this seam, so exactly one
 * definition of each seam symbol exists per build.
 */

#include <openr/tests/scale/TopologyFactory.h>

#include <fmt/format.h>

#include <openr/tests/scale/GenericTopologies.h>

namespace openr {

Topology
createScaleTopology(const thrift::ScaleTestConfig& cfg) {
  if (auto topology = createGenericTopology(cfg)) {
    return std::move(*topology);
  }
  thrift::SetupError se;
  se.reason() = thrift::SetupErrorReason::TOPOLOGY_INVALID;
  se.message() = fmt::format(
      "Topology type '{}' is not available in this build",
      *cfg.topology()->type());
  throw se;
}

std::optional<Topology>
createScaleTopologyFromParams(const ScaleTopologyParams& params) {
  return createGenericTopologyFromParams(params);
}

} // namespace openr
