/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gflags/gflags.h>

#include <folly/IPAddressV6.h>
#include <folly/MacAddress.h>
#include <folly/Subprocess.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/TimeoutManager.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>

DECLARE_bool(enable_peer_pinger);

DECLARE_int32(ping_interval_s);

class PeerPinger : public folly::AsyncTimeout {
  // This class should never be copied; remove default copy/move
  PeerPinger() = delete;
  PeerPinger(const PeerPinger&) = delete;
  PeerPinger(PeerPinger&&) = delete;
  PeerPinger& operator=(const PeerPinger&) = delete;
  PeerPinger& operator=(PeerPinger&&) = delete;

 public:
  PeerPinger(folly::EventBase* evb, openr::fbmeshd::Nl80211Handler& nlHandler);

  ~PeerPinger() override;

  void run();

  void stop();

  void syncPeers();

  void timeoutExpired() noexcept override;

  std::unordered_map<folly::MacAddress, uint32_t> getLinkMetrics();

 private:
  void pingPeer(const folly::MacAddress& peer);

  void parsePingOutput(folly::StringPiece s, folly::MacAddress peer);

  // update link metrics with the newest ping results
  void updateLinkMetrics();

  std::unordered_set<folly::MacAddress> peers_;

  // collected set of ping delay data for each peer
  std::unordered_map<folly::MacAddress, std::vector<float>> pingData_;

  // latest ping-based link metrics
  std::unordered_map<folly::MacAddress, uint32_t> linkMetrics_;

  folly::EventBase* evb_;

  // netlink handler used to request peers from the kernel
  openr::fbmeshd::Nl80211Handler& nlHandler_;
};
