/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/tests/scale/TopologyGenerator.h>

namespace openr {

/*
 * Per-area key comparison between the topology injected into the DUT and what
 * the DUT actually holds in each area's KvStoreDb. Used to verify that a
 * multi-area topology landed in the right areas, and only those areas.
 */
struct AreaKeyDiff {
  /* Keys expected in this area but not observed on the DUT. */
  std::vector<std::string> missing;
  /* Keys observed on the DUT for this area but not expected. */
  std::vector<std::string> extra;

  bool
  consistent() const {
    return missing.empty() && extra.empty();
  }
};

/*
 * The distinct OpenR areas referenced by a topology (the set of
 * VirtualRouter::area values).
 */
std::set<std::string> areasInTopology(const Topology& topology);

/*
 * Evidence that a given DUT node is acting as an Area Border Router (ABR): the
 * set of distinct areas it bridges, and how many border neighbors it has in
 * each.
 *
 * The DUT's own adjacencies are owned by its LinkMonitor on the real device and
 * are not present in the simulated topology; instead DutPatcher adds one
 * neighbor->DUT adjacency on each border node. So the areas the DUT bridges are
 * deduced from the areas of the routers that hold an adjacency to it.
 */
struct AbrProof {
  /* Distinct areas the DUT bridges. >= 2 means it is genuinely an ABR. */
  std::set<std::string> areas;
  /* Count of the DUT's border neighbors in each area. */
  std::map<std::string, int> neighborsPerArea;

  bool
  isAbr() const {
    return areas.size() >= 2;
  }
};

/*
 * Derive the ABR proof for `dutName` from the (already DUT-patched) topology:
 * scan every other router for an adjacency back to the DUT and bucket that
 * router's area. A DUT bridging >= 2 areas is what drives the per-area
 * LinkState path in the DUT's Decision module (one LinkState created per area),
 * which is the behavior this harness exists to exercise.
 */
AbrProof proveAbr(const Topology& topology, const std::string& dutName);

/*
 * Compare expected vs observed key-vals per area. Every area present in either
 * map is compared: keys present only in `expected` are reported as `missing`
 * and keys present only in `observed` as `extra`. Only key presence is
 * compared, not values.
 */
std::map<std::string, AreaKeyDiff> diffKeysByArea(
    const std::map<std::string, thrift::KeyVals>& expected,
    const std::map<std::string, thrift::KeyVals>& observed);

/*
 * True iff every area diff is consistent (no missing and no extra keys).
 */
bool allAreasConsistent(const std::map<std::string, AreaKeyDiff>& diffs);

} // namespace openr
