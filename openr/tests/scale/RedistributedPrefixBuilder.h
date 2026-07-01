/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <vector>

#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/tests/scale/VirtualRouter.h>

namespace openr {

/*
 * Apply, in place, the exact transform an Area Border Router performs when it
 * re-originates a prefix out of `fromArea` into another area (mirrors
 * PrefixManager::redistributePrefixesAcrossAreas):
 *   1. append fromArea to area_stack (index 0 stays the originating area),
 *   2. increment metrics.distance by 1 (one ABR hop),
 *   3. normalize the prefix type to RIB,
 *   4. reset ALL non-transitive attributes (forwardingType to IP,
 *      forwardingAlgorithm to SP_ECMP, and clear minNexthop and weight).
 */
void redistributePrefixOnce(
    thrift::PrefixEntry& entry, const std::string& fromArea);

/*
 * Synthesize a prefix as a non-ABR DUT in some downstream area would receive
 * it: the prefix originated in `traversedAreas.front()` and crossed one ABR per
 * element of `traversedAreas`, in order. The result carries the full traversal
 * history -- type=RIB, area_stack == traversedAreas, metrics.distance ==
 * traversedAreas.size() -- exactly as a chain of PrefixManagers would have
 * produced it hop by hop. `traversedAreas` must be non-empty (a prefix with no
 * redistribution is just an originated prefix, which the topology already
 * advertises).
 */
thrift::PrefixEntry buildRedistributedPrefixEntry(
    const thrift::IpPrefix& prefix,
    const std::vector<std::string>& traversedAreas);

/*
 * Append redistributed copies of `prefixes` to `abrProxy`'s advertised set, so
 * the existing per-area injection path advertises them into the proxy's area as
 * if an upstream ABR had redistributed them along `traversedAreas`. `abrProxy`
 * is whatever node in the DUT's area plays the role of the redistributing ABR;
 * this lets a single-area (non-ABR) DUT still see inter-area prefixes.
 */
void addRedistributedPrefixesToRouter(
    VirtualRouter& abrProxy,
    const std::vector<thrift::IpPrefix>& prefixes,
    const std::vector<std::string>& traversedAreas);

} // namespace openr
