/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

extern "C" {
#include <authsae/ampe.h>
}
#include <linux/nl80211.h>

#include <set>
#include <string>
#include <vector>

#include <folly/MacAddress.h>
#include <folly/Optional.h>
#include <folly/Portability.h>

#include <openr/fbmeshd/802.11s/AuthsaeTypes.h>
#include <openr/fbmeshd/common/ErrorCodes.h>

namespace openr {
namespace fbmeshd {

class NetInterface final {
  // This class should never be copied; remove default copy/move
  NetInterface() = delete;
  NetInterface(const NetInterface&) = delete;
  NetInterface& operator=(const NetInterface&) = delete;

 public:
  explicit NetInterface(int phyIndex);

  NetInterface(NetInterface&&);

  status_t bringLinkUp();
  status_t bringLinkDown();
  FOLLY_NODISCARD bool isValid() const;
  FOLLY_NODISCARD int phyIndex() const;
  void addSupportedFrequency(uint32_t freq);
  FOLLY_NODISCARD bool isFrequencySupported(uint32_t freq) const;

  // Functions related to authsae-powered secure meshes
  ieee80211_supported_band* getSupportedBand(ieee80211_band band);
  FOLLY_NODISCARD authsae_mesh_node* getMeshConfig();
  FOLLY_NODISCARD const authsae_mesh_node* getMeshConfig() const;

  folly::Optional<std::string> maybeIfName;
  folly::Optional<int> maybeIfIndex;
  std::string phyName;
  std::string meshId;
  int frequency{0};
  int centerFreq1{0};
  int centerFreq2{0};
  int channelWidth{NL80211_CHAN_WIDTH_20};
  bool isMeshCapable{false};
  folly::Optional<folly::MacAddress> maybeMacAddress;

  // Encryption options
  bool isEncrypted{false};
  std::string encryptionPassword;
  std::vector<int> encryptionSaeGroups;
  int encryptionDebug;

  // 802.11s specific options
  uint16_t maxPeerLinks;
  folly::Optional<int32_t> maybeRssiThreshold;
  uint8_t ttl;
  uint8_t elementTtl;
  uint32_t hwmpActivePathTimeout;
  uint32_t hwmpRannInterval;

  friend std::ostream& operator<<(
      std::ostream& os, const NetInterface& netIntf);

 private:
  status_t setLinkFlags(int flags);

  int phyIndex_{0};
  std::set<uint32_t> frequencies_;

  // Configs for authsae-powered secure meshes
  authsae_mesh_node meshConfig_{};
  authsae_meshd_config meshdConfig_{};
};

std::ostream& operator<<(std::ostream& os, const NetInterface& netIntf);

} // namespace fbmeshd
} // namespace openr
