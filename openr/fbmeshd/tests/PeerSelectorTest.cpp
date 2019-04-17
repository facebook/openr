/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>

#include <gflags/gflags.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/fbmeshd/802.11s/PeerSelector.h>
#include <openr/fbmeshd/tests/MockNl80211Handler.h>

#define BAD_RSSI -62

using namespace openr::fbmeshd;
using ::testing::_;
using ::testing::Return;

DECLARE_uint32(peer_selector_max_allowed);
DECLARE_uint32(peer_selector_min_gate_connections);

class PeerSelectorTest : public ::testing::Test {
 public:
  PeerSelectorTest() = default;

 protected:
  fbzmq::ZmqEventLoop zmqLoop_;

  MockNl80211Handler nlHandler_;
  PeerSelector peerSelector_{zmqLoop_, nlHandler_, -80, false};

  // keep bad rssi first to test sorting
  std::vector<StationInfo> testStations_ = {
      {folly::MacAddress{"02:00:00:00:01:00"},
       std::chrono::milliseconds(0),
       BAD_RSSI,
       false},
      {folly::MacAddress{"02:00:00:00:02:00"},
       std::chrono::milliseconds(0),
       -30,
       false}};
};

TEST_F(PeerSelectorTest, NegativeMaxDisables) {
  FLAGS_peer_selector_max_allowed = static_cast<uint32_t>(-1);

  MockNl80211Handler nlh;

  // should only be called once by destructor
  EXPECT_CALL(nlh, setPeerSelector(_)).Times(1);

  PeerSelector foo{zmqLoop_, nlh, -80, false};
}

TEST_F(PeerSelectorTest, PeersRankedBestFirst) {
  size_t threshold{1};
  peerSelector_.rankPeers(testStations_, &threshold);

  int lastSignal{0};

  // signal level should be monotonically decreasing
  for (auto& peer : testStations_) {
    ASSERT_LE(peer.signalAvgDbm, lastSignal);
    lastSignal = peer.signalAvgDbm;
  }
}

TEST_F(PeerSelectorTest, PeerUpdateAdjustsThreshold) {
  FLAGS_peer_selector_max_allowed = 1;
  FLAGS_peer_selector_min_gate_connections = 0;

  EXPECT_CALL(nlHandler_, getStationsInfo())
      .Times(1)
      .WillOnce(Return(testStations_));

  EXPECT_CALL(nlHandler_, setRssiThreshold(BAD_RSSI + 1)).Times(1);

  peerSelector_.onPeerAddedOrRemoved();
}
