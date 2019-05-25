/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Constants.h"

using namespace openr::fbmeshd;

// Cipher suite selectors (see 802.11-2016 section 9.4.2.25.2 Cipher suites
// and Table 9-131 Cipher suite selectors)
constexpr unsigned int Constants::CIPHER_CCMP;
constexpr unsigned int Constants::CIPHER_AES_CMAC;

constexpr int32_t Constants::kMinRssiThreshold;

constexpr std::chrono::seconds Constants::kMaxPeerInactiveTime;

constexpr int32_t Constants::kMeshSparkCmdPort;
constexpr int32_t Constants::kMeshSparkReportPort;

constexpr std::chrono::seconds Constants::kDefaultHalfLife;
constexpr std::chrono::seconds Constants::kDefaultMaxSuppressLimit;

constexpr std::array<uint8_t, 3> Constants::kFacebookOui;
