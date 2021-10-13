/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/IPAddress.h>
#include <folly/logging/xlog.h>
#include <openr/common/PrependLabelAllocator.h>
#include <optional>

namespace openr {

template <class T>
PrependLabelAllocator<T>::PrependLabelAllocator(
    std::shared_ptr<const Config> config) {
  if (config->isSegmentRoutingEnabled() &&
      config->isSegmentRoutingConfigured()) {
    const auto& srConfig = config->getSegmentRoutingConfig();
    CHECK(srConfig.prepend_label_ranges_ref().has_value());
    labelRangeV4_.first =
        *srConfig.prepend_label_ranges_ref()->v4_ref()->start_label_ref();
    labelRangeV4_.second =
        *srConfig.prepend_label_ranges_ref()->v4_ref()->end_label_ref();
    labelRangeV6_.first =
        *srConfig.prepend_label_ranges_ref()->v6_ref()->start_label_ref();
    labelRangeV6_.second =
        *srConfig.prepend_label_ranges_ref()->v6_ref()->end_label_ref();
  }
}

template <class T>
int32_t
PrependLabelAllocator<T>::getNewMplsLabel(bool isV4) {
  // Return label from free range if any available
  if (isV4) {
    if (freedMplsLabelsV4_.size()) {
      int32_t ret = freedMplsLabelsV4_.back();
      freedMplsLabelsV4_.pop_back();
      return ret;
    }
    // Return next label from the v4 range
    CHECK_LE(nextMplsLabelV4_, labelRangeV4_.second)
        << "V4: Exhausted static MPLS range";
    return nextMplsLabelV4_++;
  } else {
    if (freedMplsLabelsV6_.size()) {
      int32_t ret = freedMplsLabelsV6_.back();
      freedMplsLabelsV6_.pop_back();
      return ret;
    }
    // Return next label from the v6 range
    CHECK_LE(nextMplsLabelV6_, labelRangeV6_.second)
        << "V6: Exhausted static MPLS range";
    return nextMplsLabelV6_++;
  }
}

template <class T>
void
PrependLabelAllocator<T>::freeMplsLabel(
    bool isV4, int32_t label, const std::string& nh_str) {
  auto labelRange = getPrependLabelRange(isV4);
  const std::string ip_family = fmt::format("IPv{}", isV4 ? 4 : 6);
  CHECK(label >= labelRange.first and label <= labelRange.second)
      << " label " << label << " assignment to " << ip_family
      << " address is incorrect for " << nh_str;
  if (isV4) {
    freedMplsLabelsV4_.emplace_back(label);
  } else {
    freedMplsLabelsV6_.emplace_back(label);
  }
}

template <class T>
const std::pair<int32_t, int32_t>
PrependLabelAllocator<T>::getPrependLabelRange(bool isV4) {
  if (isV4) {
    return labelRangeV4_;
  } else {
    return labelRangeV6_;
  }
}

template <class T>
std::optional<int32_t>
PrependLabelAllocator<T>::decrementRefCount(const std::set<T>& nextHopSet) {
  std::optional<int32_t> oldLabel = std::nullopt;
  if (nextHopSet.size()) {
    auto& [refCount, label] = nextHopSetToLabel_.at(nextHopSet);
    refCount--;
    CHECK_GE(refCount, 0) << "Reference count can never be negative";
    if (refCount == 0) {
      oldLabel = label;
      CHECK_GT(nextHopSet.size(), 0) << "Nexthop set must have a valid entry";
      const auto& nh = *nextHopSet.begin();
      nextHopSetToLabel_.erase(nextHopSet);
      if (oldLabel) {
        XLOG(DBG1) << "De-allocating label " << oldLabel.value()
                   << " used for nextHopSet consisting of";
        for (auto const& nhEntry : nextHopSet) {
          XLOG(DBG1) << " " << toString(nhEntry);
        }
        freeMplsLabel(isAddressFamilyV4(nh), oldLabel.value(), toString(nh));
      }
    }
  }
  return oldLabel;
}

template <class T>
std::pair<std::optional<int32_t>, bool>
PrependLabelAllocator<T>::incrementRefCount(const std::set<T>& nextHopSet) {
  std::optional<int32_t> newOrCurrentLabel = std::nullopt;
  auto isNewLabel = false;
  if (nextHopSet.size()) {
    auto& [refCount, label] = nextHopSetToLabel_[nextHopSet];
    refCount++; // Increase ref-count
    if (label == 0) {
      CHECK_GT(nextHopSet.size(), 0) << "Nexthop set must have a valid entry";
      const auto& nh = *nextHopSet.begin();
      // Create a new label
      label = getNewMplsLabel(isAddressFamilyV4(nh));
      XLOG(DBG1) << "Allocating label " << label
                 << " for nexthop set consisting of";
      for (auto const& nhEntry : nextHopSet) {
        XLOG(DBG1) << " " << toString(nhEntry);
      }
      isNewLabel = true;
    }
    newOrCurrentLabel = label;
  }
  return std::make_pair(newOrCurrentLabel, isNewLabel);
}

// explicit instantiations for our use cases
template class PrependLabelAllocator<folly::IPAddress>;
template class PrependLabelAllocator<thrift::NextHopThrift>;
} // namespace openr
