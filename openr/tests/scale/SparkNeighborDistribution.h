/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

namespace openr {

/*
 * A host network interface usable for SparkFaker packet injection, together
 * with the source addresses the fake neighbors pinned to it will use.
 */
struct UsableInterface {
  std::string ifName;
  int ifIndex{0};
  std::string v6Addr;
  std::string v4Addr;
};

/*
 * One fake neighbor assigned to a host interface. Carries everything
 * SparkFaker::addNeighbor() needs to wire the neighbor onto its interface.
 */
struct SparkNeighborPlacement {
  std::string neighborName; /* e.g. "spine-0" */
  std::string neighborIfName; /* "<neighborName>-to-dut" */
  std::string hostIfName; /* host interface to send/recv on */
  int hostIfIndex{0};
  std::string v6Addr; /* link-local source address on the host interface */
  std::string v4Addr;
};

/*
 * Distribute dutNeighborNames across usable interfaces in contiguous blocks.
 * Each interface gets floor(N / numInterfaces) neighbors and the LAST interface
 * absorbs the remainder, so every neighbor is placed on a usable interface.
 *
 * Returns exactly one placement per neighbor, in dutNeighborNames order.
 * Returns an empty result when there are no usable interfaces or no neighbors;
 * callers that require neighbors to be placed must treat an empty result for a
 * non-empty neighbor set as an error.
 *
 * Pure function: performs no syscalls and no I/O, so it is unit-testable
 * without real interfaces or a DUT.
 */
std::vector<SparkNeighborPlacement> distributeSparkNeighbors(
    const std::vector<UsableInterface>& usable,
    const std::vector<std::string>& dutNeighborNames);

} // namespace openr
