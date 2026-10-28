/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>

#include <openr/tests/e2e/TopologyTypes.h>

namespace openr::e2e {

void allocateLinkAddresses(LinkSpec& link, size_t subnet);
void allocateSegmentAddresses(SegmentSpec& segment, size_t subnet);

} // namespace openr::e2e
