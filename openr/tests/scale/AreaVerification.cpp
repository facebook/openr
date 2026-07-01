/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/AreaVerification.h>

namespace openr {

std::set<std::string>
areasInTopology(const Topology& topology) {
  std::set<std::string> areas;
  for (const auto& [name, router] : topology.routers) {
    areas.insert(router.area);
  }
  return areas;
}

AbrProof
proveAbr(const Topology& topology, const std::string& dutName) {
  AbrProof proof;
  for (const auto& [name, router] : topology.routers) {
    if (name == dutName) {
      continue; // the DUT's own adj DB is owned by its LinkMonitor
    }
    for (const auto& adj : router.adjacencies) {
      if (adj.remoteRouterName == dutName) {
        proof.areas.insert(router.area);
        ++proof.neighborsPerArea[router.area];
        break; // count each border neighbor at most once
      }
    }
  }
  return proof;
}

std::map<std::string, AreaKeyDiff>
diffKeysByArea(
    const std::map<std::string, thrift::KeyVals>& expected,
    const std::map<std::string, thrift::KeyVals>& observed) {
  std::set<std::string> areas;
  for (const auto& [area, keyVals] : expected) {
    areas.insert(area);
  }
  for (const auto& [area, keyVals] : observed) {
    areas.insert(area);
  }

  const thrift::KeyVals kEmpty;
  std::map<std::string, AreaKeyDiff> diffs;
  for (const auto& area : areas) {
    AreaKeyDiff diff;
    auto expIt = expected.find(area);
    const auto& exp = expIt != expected.end() ? expIt->second : kEmpty;
    auto obsIt = observed.find(area);
    const auto& obs = obsIt != observed.end() ? obsIt->second : kEmpty;

    for (const auto& [key, value] : exp) {
      if (!obs.count(key)) {
        diff.missing.push_back(key);
      }
    }
    for (const auto& [key, value] : obs) {
      if (!exp.count(key)) {
        diff.extra.push_back(key);
      }
    }
    diffs.emplace(area, std::move(diff));
  }
  return diffs;
}

bool
allAreasConsistent(const std::map<std::string, AreaKeyDiff>& diffs) {
  for (const auto& [area, diff] : diffs) {
    if (!diff.consistent()) {
      return false;
    }
  }
  return true;
}

} // namespace openr
