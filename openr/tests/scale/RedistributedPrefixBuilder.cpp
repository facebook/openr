/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/RedistributedPrefixBuilder.h>

#include <stdexcept>

#include <openr/common/LsdbUtil.h>

namespace openr {

void
redistributePrefixOnce(
    thrift::PrefixEntry& entry, const std::string& fromArea) {
  entry.area_stack()->emplace_back(fromArea);
  entry.metrics()->distance() = entry.metrics()->distance().value() + 1;
  entry.type() = thrift::PrefixType::RIB;
  /*
   * Reset every non-transitive attribute, matching production
   * PrefixManager::resetNonTransitiveAttrs: these describe how this router
   * forwards and must not carry across an area boundary.
   */
  entry.forwardingType() = thrift::PrefixForwardingType::IP;
  entry.forwardingAlgorithm() = thrift::PrefixForwardingAlgorithm::SP_ECMP;
  entry.minNexthop().reset();
  entry.weight().reset();
}

thrift::PrefixEntry
buildRedistributedPrefixEntry(
    const thrift::IpPrefix& prefix,
    const std::vector<std::string>& traversedAreas) {
  if (traversedAreas.empty()) {
    throw std::invalid_argument(
        "buildRedistributedPrefixEntry requires a non-empty traversedAreas");
  }
  /*
   * Start from a freshly originated prefix (area_stack empty, distance 0) and
   * redistribute it once per area it traversed, so the end state matches what a
   * real chain of ABRs would have produced rather than being hand-assembled.
   */
  auto entry = createPrefixEntry(
      prefix,
      thrift::PrefixType::LOOPBACK,
      "" /* data */,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP);
  for (const auto& area : traversedAreas) {
    redistributePrefixOnce(entry, area);
  }
  return entry;
}

void
addRedistributedPrefixesToRouter(
    VirtualRouter& abrProxy,
    const std::vector<thrift::IpPrefix>& prefixes,
    const std::vector<std::string>& traversedAreas) {
  abrProxy.advertisedPrefixes.reserve(
      abrProxy.advertisedPrefixes.size() + prefixes.size());
  for (const auto& prefix : prefixes) {
    abrProxy.advertisedPrefixes.push_back(
        buildRedistributedPrefixEntry(prefix, traversedAreas));
  }
}

} // namespace openr
