/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <glog/logging.h>
#include <openr/if/gen-cpp2/Network_types.h>

namespace openr {

/**
 * Validates that non-zero label is 20 bit only and other bits are not set
 * XXX: We can do more validation - e.g. reserved range, global vs local range
 */
inline bool
isMplsLabelValid(int32_t mplsLabel) {
  return (mplsLabel & 0xfff00000) == 0 && mplsLabel != 0;
}

/**
 * Validates mplsAction object and fatals
 */
inline void
checkMplsAction(thrift::MplsAction const& mplsAction) {
  switch (*mplsAction.action()) {
  case thrift::MplsActionCode::PUSH:
    // Swap label shouldn't be set
    CHECK(!mplsAction.swapLabel().has_value());
    // Push labels should be set
    CHECK(mplsAction.pushLabels().has_value());
    // there should be atleast one push label
    CHECK(!mplsAction.pushLabels()->empty());
    for (auto const& label : mplsAction.pushLabels().value()) {
      CHECK(isMplsLabelValid(label));
    }
    break;
  case thrift::MplsActionCode::SWAP:
    // Swap label should be set
    CHECK(mplsAction.swapLabel().has_value());
    CHECK(isMplsLabelValid(mplsAction.swapLabel().value()));
    // Push labels shouldn't be set
    CHECK(!mplsAction.pushLabels().has_value());
    break;
  case thrift::MplsActionCode::PHP:
  case thrift::MplsActionCode::POP_AND_LOOKUP:
    // Swap label should not be set
    CHECK(!mplsAction.swapLabel().has_value());
    CHECK(!mplsAction.pushLabels().has_value());
    break;
  default:
    CHECK(false) << "Unknown action code";
  }
}

class MplsConstants {
 public:
  MplsConstants() {
    // Sanity checks on Segment Routing labels
    const int32_t maxLabel = MplsConstants::kMaxSrLabel;
    CHECK_GT(kSrGlobalRange.first, 0);
    CHECK_LT(kSrGlobalRange.second, maxLabel);
    CHECK_GT(kSrLocalRange.first, 0);
    CHECK_LT(kSrLocalRange.second, maxLabel);
    CHECK_LT(kSrGlobalRange.first, kSrGlobalRange.second);
    CHECK_LT(kSrLocalRange.first, kSrLocalRange.second);

    // Local and Global range must be exclusive of each other
    CHECK(
        (kSrGlobalRange.second < kSrLocalRange.first) ||
        (kSrGlobalRange.first > kSrLocalRange.second))
        << "Overlapping global/local segment routing label space.";
  }
  // Maximum label size
  static constexpr int32_t kMaxSrLabel{(1 << 20) - 1};

  // Segment Routing namespace constants. Local and Global ranges are exclusive
  static constexpr std::pair<int32_t /* low */, int32_t /* high */>
      kSrGlobalRange{101, 999};
  static constexpr std::pair<int32_t /* low */, int32_t /* high */>
      kSrLocalRange{50000, 59999};
  static constexpr std::pair<int32_t /* low */, int32_t /* high */>
      kSrV4StaticMplsRouteRange{60000, 64999};
  static constexpr std::pair<int32_t /* low */, int32_t /* high */>
      kSrV6StaticMplsRouteRange{65000, 69999};
};

} // namespace openr
