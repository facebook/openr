/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/GenericTopologies.h>

#include <openr/tests/scale/TopologyGenerator.h>

namespace openr {

namespace {

[[noreturn]] void
failInvalid(std::string msg) {
  thrift::SetupError se;
  se.reason() = thrift::SetupErrorReason::TOPOLOGY_INVALID;
  se.message() = std::move(msg);
  throw se;
}

/*
 * Returns the value if > 0, otherwise the supplied default. Used to map the
 * flag-derived params (which default to 0 when unset) onto the optional fabric
 * knobs.
 */
int
valueOr(int value, int fallback) {
  return value > 0 ? value : fallback;
}

} // namespace

std::optional<Topology>
createGenericTopology(const thrift::ScaleTestConfig& cfg) {
  const auto& t = *cfg.topology();
  const auto& type = *t.type();
  const int numPrefixesPerNode =
      t.numPrefixesPerNode().value_or(kDefaultPrefixesPerRouter);

  if (type == "fabric") {
    if (!t.numPods().has_value()) {
      failInvalid("TopologyConfig.numPods must be set for 'fabric'");
    }
    if (!t.numSpines().has_value()) {
      failInvalid(
          "TopologyConfig.numSpines (used as numPlanes) must be set for 'fabric'");
    }
    return TopologyGenerator::createFabric(
        *t.numPods(),
        *t.numSpines(),
        t.numSuperSpines().value_or(kDefaultSswsPerPlane),
        t.numLeaves().value_or(kDefaultRswsPerPod),
        numPrefixesPerNode);
  }
  if (type == "ring") {
    if (!t.numSpines().has_value()) {
      failInvalid(
          "TopologyConfig.numSpines (used as ring size) must be set for 'ring'");
    }
    return TopologyGenerator::createRing(*t.numSpines(), numPrefixesPerNode);
  }
  if (type == "grid") {
    if (!t.numSpines().has_value()) {
      failInvalid(
          "TopologyConfig.numSpines (used as grid dimension) must be set for 'grid'");
    }
    return TopologyGenerator::createGrid(*t.numSpines(), numPrefixesPerNode);
  }
  return std::nullopt;
}

std::optional<Topology>
createGenericTopologyFromParams(const ScaleTopologyParams& p) {
  const int numPrefixesPerNode =
      valueOr(p.numPrefixesPerNode, kDefaultPrefixesPerRouter);

  if (p.type == "fabric") {
    return TopologyGenerator::createFabric(
        p.numPods,
        p.numSpines,
        valueOr(p.numSuperSpines, kDefaultSswsPerPlane),
        valueOr(p.numLeaves, kDefaultRswsPerPod),
        numPrefixesPerNode);
  }
  if (p.type == "ring") {
    return TopologyGenerator::createRing(p.numSpines, numPrefixesPerNode);
  }
  if (p.type == "grid") {
    return TopologyGenerator::createGrid(p.numSpines, numPrefixesPerNode);
  }
  return std::nullopt;
}

} // namespace openr
