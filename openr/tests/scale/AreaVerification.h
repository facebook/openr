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
