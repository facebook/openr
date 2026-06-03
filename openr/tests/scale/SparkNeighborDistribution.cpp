/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/SparkNeighborDistribution.h>

#include <fmt/format.h>

namespace openr {

std::vector<SparkNeighborPlacement>
distributeSparkNeighbors(
    const std::vector<UsableInterface>& usable,
    const std::vector<std::string>& dutNeighborNames) {
  std::vector<SparkNeighborPlacement> placements;
  if (usable.empty() || dutNeighborNames.empty()) {
    return placements;
  }

  const int numNeighbors = static_cast<int>(dutNeighborNames.size());
  const int neighborsPerInterface =
      numNeighbors / static_cast<int>(usable.size());
  placements.reserve(numNeighbors);

  int neighborIdx = 0;
  for (size_t i = 0; i < usable.size(); ++i) {
    const auto& iface = usable[i];
    /*
     * The last interface absorbs the remainder so no neighbor is dropped when
     * the count does not divide evenly across interfaces.
     */
    const int neighborsOnThisIf = (i == usable.size() - 1)
        ? (numNeighbors - neighborIdx)
        : neighborsPerInterface;
    for (int j = 0; j < neighborsOnThisIf && neighborIdx < numNeighbors;
         ++j, ++neighborIdx) {
      const auto& neighborName = dutNeighborNames[neighborIdx];
      placements.push_back(
          {neighborName,
           fmt::format("{}-to-dut", neighborName),
           iface.ifName,
           iface.ifIndex,
           iface.v6Addr,
           iface.v4Addr});
    }
  }
  return placements;
}

} // namespace openr
