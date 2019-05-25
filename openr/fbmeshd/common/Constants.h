/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace openr {
namespace fbmeshd {

class Constants final {
  // This class is static, remove default copy/move
  Constants() = delete;
  Constants(const Constants&) = delete;
  Constants(Constants&&) = delete;
  Constants& operator=(const Constants&) = delete;
  Constants& operator=(Constants&&) = delete;

 public:
  // Cipher suite selectors (see 802.11-2016 section 9.4.2.25.2 Cipher suites
  // and Table 9-131 Cipher suite selectors)
  static constexpr unsigned int CIPHER_CCMP{0x000FAC04};
  static constexpr unsigned int CIPHER_AES_CMAC{0x000FAC06};

  // Lowest RSSI threshold we will try meshing with
  static constexpr int kMinRssiThreshold{-100};

  // Maximum time a peer can be inactive before being removed from list of
  // neighbors
  static constexpr std::chrono::seconds kMaxPeerInactiveTime{120};

  // The port mesh spark recives commands on
  static constexpr int32_t kMeshSparkCmdPort{60018};

  // The port mesh spark reports on
  static constexpr int32_t kMeshSparkReportPort{60019};

  // Gateway Connectivity Manager Robustness default
  static constexpr unsigned int kDefaultRobustness{2};

  // Gateway Connectivity Manager Route Dampener defaults
  static constexpr unsigned int kDefaultPenalty{1000};
  static constexpr unsigned int kDefaultSuppressLimit{2000};
  static constexpr unsigned int kDefaultReuseLimit{750};
  static constexpr std::chrono::seconds kDefaultHalfLife{1 * 60};
  static constexpr std::chrono::seconds kDefaultMaxSuppressLimit{3 * 60};

  // Facebook OUI
  static constexpr std::array<uint8_t, 3> kFacebookOui{0xa4, 0x0e, 0x2b};
};

} // namespace fbmeshd
} // namespace openr
