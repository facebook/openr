/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PeerSelector.h"

DEFINE_uint32(
    peer_selector_max_allowed,
    std::numeric_limits<uint32_t>::max(),
    "max number of peers to allow");

DEFINE_uint32(
    peer_selector_eviction_period_s, 60, "how frequently we can evict peers");

DEFINE_uint32(
    peer_selector_min_gate_connections,
    2,
    "how many gate connections we try to maintain");

DEFINE_int32(
    peer_selector_poll_interval_s,
    1,
    "how often to poll the list of peers (disabled if no peer limit)");

using namespace openr::fbmeshd;

/*
 * How peers are selected, ordered from best to worst.
 */
struct PeerSelectorOrdering {
  bool
  operator()(StationInfo a, StationInfo b) {
    return a.signalAvgDbm > b.signalAvgDbm;
  }
} sortOrder;

PeerSelector::PeerSelector(
    fbzmq::ZmqEventLoop& zmqLoop,
    Nl80211HandlerInterface& nlHandler,
    int minRssiThreshold)
    : nlHandler_{nlHandler},
      minRssiThreshold_{minRssiThreshold},
      rssiThreshold_{minRssiThreshold},
      interval_(std::chrono::seconds(FLAGS_peer_selector_poll_interval_s)) {
  if (FLAGS_peer_selector_max_allowed == std::numeric_limits<uint32_t>::max()) {
    return;
  }

  nlHandler.setPeerSelector(this);

  timer_ =
      fbzmq::ZmqTimeout::make(&zmqLoop, [this]() mutable noexcept { poll(); });
  timer_->scheduleTimeout(interval_, true);
}

PeerSelector::~PeerSelector() {
  nlHandler_.setPeerSelector(nullptr);
}

bool
PeerSelector::shouldAddCandidate(StationInfo& cand) {
  VLOG(7) << "new candidate: "
          << " peer: " << cand.macAddress << " rssi: " << (int)cand.signalAvgDbm
          << " gate-connected: " << cand.isConnectedToGate;

  size_t threshold = FLAGS_peer_selector_max_allowed;

  if (currentPeers_.size() < threshold) {
    VLOG(7) << "accepting: ct " << currentPeers_.size() << " < " << threshold;
    return true;
  }

  std::vector<StationInfo> peers;
  peers.push_back(cand);
  for (auto const& it : currentPeers_) {
    peers.push_back(it.second);
  }
  rankPeers(peers, &threshold);

  size_t i{};
  for (auto it = peers.begin(); it != peers.end() && i < threshold; ++it, ++i) {
    if (it->macAddress == cand.macAddress) {
      VLOG(7) << "accepting: peer ranked " << i << " / " << threshold;
      return true;
    }
  }

  VLOG(7) << "rejecting due to threshold";
  return false;
}

void
PeerSelector::onPeerAddedOrRemoved() {
  poll();
}

bool
PeerSelector::membershipChanged(std::vector<StationInfo>& newPeers) {
  if (newPeers.size() != currentPeers_.size()) {
    return true;
  }

  // if newPeers has any mac address that's not in current peer set then
  // we added and removed same number of peers
  for (auto& it : newPeers) {
    if (currentPeers_.find(it.macAddress) == currentPeers_.end()) {
      return true;
    }
  }
  return false;
}

void
PeerSelector::poll() {
  // get current list of peers and metrics
  std::vector<StationInfo> peers = nlHandler_.getStationsInfo();
  if (membershipChanged(peers)) {
    processMembershipChange(peers);
  } else if (peers.size() < FLAGS_peer_selector_max_allowed) {
    auto timeSinceThresholdUpdate =
        std::chrono::steady_clock::now() - lastThresholdReduction_;
    // reduce threshold again if it's been a while with no new peers
    if (timeSinceThresholdUpdate > 1min) {
      VLOG(9) << "readjusting rssi threshold";
      processMembershipChange(peers);
    }
  }
}

void
PeerSelector::rankPeers(std::vector<StationInfo>& peers, size_t* threshold) {
  // sort peers by RSSI
  std::sort(peers.begin(), peers.end(), sortOrder);

  // make sure we have at least min_gate_connections, which may
  // require raising the threshold
  int needed = FLAGS_peer_selector_min_gate_connections;
  for (size_t i = 0; i < peers.size() && needed > 0; i++) {
    if (peers[i].isConnectedToGate) {
      needed--;
      // copy this one at the end and grow threshold if needed
      if (i >= *threshold && *threshold < peers.size()) {
        auto here = std::next(peers.begin(), i);
        auto tail = std::next(peers.begin(), *threshold);
        std::iter_swap(tail, here);
        ++*threshold;
      }
    }
  }
}

void
PeerSelector::processMembershipChange(std::vector<StationInfo>& newPeers) {
  VLOG(8) << "membership changed, new size: " << newPeers.size();

  auto secsSinceEviction = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - lastEviction_)
                               .count();

  if (newPeers.size() > FLAGS_peer_selector_max_allowed &&
      secsSinceEviction < FLAGS_peer_selector_eviction_period_s) {
    VLOG(9) << "ignoring peer updates until end of eviction period in "
            << (FLAGS_peer_selector_eviction_period_s - secsSinceEviction)
            << "s";
    return;
  }

  // peer count larger than threshold?
  if (newPeers.size() > FLAGS_peer_selector_max_allowed) {
    // look for a peer to kick out.  we sort the list by desired
    // criteria, then pick the best up to our max.
    size_t threshold = FLAGS_peer_selector_max_allowed;
    rankPeers(newPeers, &threshold);
    if (threshold >= newPeers.size()) {
      currentPeers_.clear();
      for (const auto& it : newPeers) {
        currentPeers_[it.macAddress] = it;
      }
      return;
    }

    VLOG(5) << "truncating peer list to " << threshold << " elements (original "
            << "size: " << newPeers.size() << ")";

    for (const auto& peer : newPeers) {
      VLOG(5) << " peer: " << peer.macAddress
              << " rssi: " << (int)peer.signalAvgDbm
              << " gate-connected: " << peer.isConnectedToGate;
    }

    auto it = std::next(newPeers.begin(), threshold);

    // increase rssi threshold after each eviction by either 1
    // or up the level of the first evicted peer + 1.
    rssiThreshold_ += 1 + std::max(0, it->signalAvgDbm - rssiThreshold_);

    // kernel requires threshold <= -1 (=0 disables)
    rssiThreshold_ = std::min(-1, rssiThreshold_);
    nlHandler_.setRssiThreshold(rssiThreshold_);

    // evict peers from kernel
    for (; it != newPeers.end(); it++) {
      nlHandler_.deleteStation(it->macAddress);
    }
    lastEviction_ = std::chrono::steady_clock::now();
    newPeers.resize(FLAGS_peer_selector_max_allowed);

  } else if (newPeers.size() < FLAGS_peer_selector_max_allowed) {
    // peer count too small, lower rssi threshold to get more peers
    lastThresholdReduction_ = std::chrono::steady_clock::now();
    rssiThreshold_ = std::max(rssiThreshold_ - 10, minRssiThreshold_);
    nlHandler_.setRssiThreshold(rssiThreshold_);
  }

  currentPeers_.clear();
  for (const auto& it : newPeers) {
    currentPeers_[it.macAddress] = it;
  }
}
