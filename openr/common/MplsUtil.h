/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace openr {

/**
 * Validates that non-zero label is 20 bit only and other bits are not set
 * XXX: We can do more validation - e.g. reserved range, global vs local range
 */
inline bool
isMplsLabelValid(int32_t mplsLabel) {
  return (mplsLabel & 0xfff00000) == 0 and mplsLabel != 0;
}

} // namespace openr
