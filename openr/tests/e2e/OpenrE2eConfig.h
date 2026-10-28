/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>

#include <openr/if/gen-cpp2/OpenrConfig_types.h>

namespace openr::e2e {

thrift::OpenrConfig makeOpenrE2eConfig(
    const std::string& nodeName,
    uint16_t fibPort,
    uint16_t kvStorePort,
    const std::string& persistentStorePath);

} // namespace openr::e2e
