/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>

namespace openr {
namespace fbmeshd {

class MockNl80211Handler : public Nl80211HandlerInterface {
 public:
  MOCK_METHOD0(getPeers, std::vector<folly::MacAddress>());
  MOCK_METHOD0(getStationsInfo, std::vector<StationInfo>());
  MOCK_METHOD1(setRssiThreshold, void(int32_t rssiThreshold));
  MOCK_METHOD1(deleteStation, void(folly::MacAddress peer));
  MOCK_METHOD0(lookupMeshNetif, NetInterface&());
};

} // namespace fbmeshd
} // namespace openr
