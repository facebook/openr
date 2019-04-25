/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/IPAddressV6.h>
#include <folly/MacAddress.h>
#include <folly/Subprocess.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/TimeoutManager.h>

class PeerPinger : public folly::AsyncTimeout {
  // This class should never be copied; remove default copy/move
  PeerPinger() = delete;
  PeerPinger(const PeerPinger&) = delete;
  PeerPinger(PeerPinger&&) = delete;
  PeerPinger& operator=(const PeerPinger&) = delete;
  PeerPinger& operator=(PeerPinger&&) = delete;

 public:
  explicit PeerPinger(folly::EventBase* evb);
  ~PeerPinger() override;

  void run();

  void stop();

  void addPeer(const folly::MacAddress& peerMacAddr);

  void removePeer(const folly::MacAddress& peerMacAddr);

  void timeoutExpired() noexcept override;

 private:
  void pingPeer(const folly::MacAddress& peer);

  void parsePingOutput(folly::StringPiece s);

  std::unordered_set<folly::MacAddress> peers_;

  folly::EventBase* evb_;
};
