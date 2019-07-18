/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Portability.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Types.h>
#include <openr/fbmeshd/802.11s/Nl80211Handler.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/kvstore/KvStoreClient.h>

namespace openr {
namespace fbmeshd {

class PeerSelector final {
  // This class should never be copied; remove default copy/move
  PeerSelector() = delete;
  PeerSelector(const PeerSelector&) = delete;
  PeerSelector(PeerSelector&&) = delete;
  PeerSelector& operator=(const PeerSelector&) = delete;
  PeerSelector& operator=(PeerSelector&&) = delete;

 public:
  PeerSelector(
      fbzmq::ZmqEventLoop& zmqLoop,
      Nl80211HandlerInterface& nlHandler,
      int minRssiThreshold);

  ~PeerSelector();

  bool shouldAddCandidate(StationInfo& cand);
  void onPeerAddedOrRemoved();

  /**
   * Order the peers according to preference from best to worst.
   * Threshold is a soft limit which this method may raise if needed.
   */
  void rankPeers(std::vector<StationInfo>& newPeers, size_t* threshold);

 private:
  /**
   * This method is periodically called to get the current available
   * peer set.  Based on this data the peer selector may adjust the
   * rssi filter and delete peers.
   */
  void poll();

  /**
   * Returns true if the set of peers changed based on the polling
   * function and the cached copy
   */
  FOLLY_NODISCARD bool membershipChanged(std::vector<StationInfo>& peers);

  /**
   * This method is called whenever the set of established 11s peers
   * changes.
   */
  void processMembershipChange(std::vector<StationInfo>& newPeers);

  // internal copy of peer list, for change detection
  std::map<folly::MacAddress, StationInfo> currentPeers_;

  // netlink handler used to request metrics from the kernel
  Nl80211HandlerInterface& nlHandler_;

  // lowest threshold we will use
  int32_t minRssiThreshold_;

  // current threshold
  int32_t rssiThreshold_;

  // last time we evicted a peer
  std::chrono::steady_clock::time_point lastEviction_{
      std::chrono::steady_clock::now() - 10min};

  // last time we reduced rssi threshold
  std::chrono::steady_clock::time_point lastThresholdReduction_{
      std::chrono::steady_clock::now() - 10min};

  // polling interval for `poll`
  const std::chrono::seconds interval_;

  std::unique_ptr<fbzmq::ZmqTimeout> timer_;
}; // PeerSelector

} // namespace fbmeshd
} // namespace openr
