/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/MplsUtil.h>
#include <openr/config/Config.h>

namespace openr {

// This class manages allocation and deallocation of MPLS labels (specifically
// prepend labels). These labels are advertised as a part of routes so that
// remote nodes can program routes with MPLS label and forward MPLS traffic.
// An MPLS label (or prepend label) could be associated with a nexthop
// group for a given prefix and can serve as a way to stitch LSPs between two
// different areas or openr domains. While a prepend label is advertised
// to remote nodes/areas, a corresponding MPLS label route can be programmed
// in local nodes.
class PrependLabelAllocator {
 public:
  explicit PrependLabelAllocator(std::shared_ptr<const Config> config);
  ~PrependLabelAllocator() = default;

  /**
   * Return an available mpls label from previously freed labels from previous
   * iteration or allocation. Otherwise, generate next label next from the range
   * and allocate.
   */
  int32_t getNewMplsLabel(bool isV4);

  /**
   * Free previously allocated label considering the address family (ipv4 or
   * ipv6)
   */
  void freeMplsLabel(bool isV4, int32_t label, const std::string& nh_str);

  /**
   * Return the NH to label mapping
   */
  std::map<std::set<folly::IPAddress>, std::pair<int32_t, int32_t>>&
  getNextHopSetToLabel() {
    return nextHopSetToLabel_;
  }

  /**
   * Decrement reference count of the label associated with the nexthop set
   */
  int32_t decrementRefCount(const std::set<folly::IPAddress>& nextHopSet);

  /**
   * Increment reference count of the label associated with the nexthop set
   */
  void incrementRefCount(const std::set<folly::IPAddress>& nextHopSet);

 private:
  // Get the configured prepend labels
  const std::pair<int32_t, int32_t> getPrependLabelRange(bool isV4);

  // NextHopSet -> [RefCount, Label] mapping.
  std::map<std::set<folly::IPAddress>, std::pair<int32_t, int32_t>>
      nextHopSetToLabel_;

  /*
   * Next MPLS label available for use per address family (ipv4/ipv6).
   */
  int32_t nextMplsLabelV4_{MplsConstants::kSrV4StaticMplsRouteRange.first};
  int32_t nextMplsLabelV6_{MplsConstants::kSrV6StaticMplsRouteRange.first};

  /**
   * The available MPLS labels freed by prefix withdrawals. The
   * list is ordered, and the last element is the one most recently freed one.
   */
  std::vector<int32_t> freedMplsLabelsV4_;
  std::vector<int32_t> freedMplsLabelsV6_;

  // Initialize for backward compatibility.
  std::pair<int32_t, int32_t> labelRangeV4_{
      MplsConstants::kSrV4StaticMplsRouteRange.first,
      MplsConstants::kSrV4StaticMplsRouteRange.second};
  std::pair<int32_t, int32_t> labelRangeV6_{
      MplsConstants::kSrV6StaticMplsRouteRange.first,
      MplsConstants::kSrV6StaticMplsRouteRange.second};
};
} // namespace openr
