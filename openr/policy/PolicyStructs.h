/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>

namespace openr {

// OpenrPolicyActionData - capture any data that's needed to apply a action
// but can NOT be pre-configured in Policy Configuration
// (e.g some data needs to be dynamically derived on the fly).
struct OpenrPolicyActionData {
  explicit OpenrPolicyActionData(int32_t prependLabel)
      : prependLabel(prependLabel) {}

  // Prepend label to append to the label.
  std::optional<int32_t> prependLabel;
};

} // namespace openr
