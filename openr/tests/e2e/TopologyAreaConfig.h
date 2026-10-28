/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/tests/e2e/Topology.h>

namespace openr::e2e {

std::vector<thrift::AreaConfig> makeTopologyAreaConfigs(
    const Topology& topology, const NodeSpec& router);

std::vector<thrift::InterfaceMetricStatic> makeTopologyInterfaceMetrics(
    const Topology& topology, const NodeSpec& router);

} // namespace openr::e2e
