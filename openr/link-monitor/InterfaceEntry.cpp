/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InterfaceEntry.h"
#include <folly/gen/Base.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>

namespace openr {

InterfaceEntry::InterfaceEntry(
    std::string const& ifName,
    std::chrono::milliseconds const& initBackoff,
    std::chrono::milliseconds const& maxBackoff,
    AsyncThrottle& updateCallback,
    folly::AsyncTimeout& updateTimeout)
    : ifName_(ifName),
      backoff_(initBackoff, maxBackoff),
      updateCallback_(updateCallback),
      updateTimeout_(updateTimeout) {}

bool
InterfaceEntry::updateAttrs(int ifIndex, bool isUp, uint64_t weight) {
  const bool wasActive = isActive();
  const bool wasUp = isUp_;
  bool isUpdated = false;
  isUpdated |= std::exchange(ifIndex_, ifIndex) != ifIndex;
  isUpdated |= std::exchange(isUp_, isUp) != isUp;
  isUpdated |= std::exchange(weight_, weight) != weight;

  // Look for specific case of interface state transition to DOWN
  if (wasUp != isUp and wasUp) {
    // Penalize backoff on transitioning to DOWN state
    backoff_.reportError();
  }

  // Look for active to down transition
  if (wasActive and not isUp) {
    // Schedule immediate timeout for fast propagation of link-down event
    updateTimeout_.scheduleTimeout(Constants::kLinkImmediateTimeout);
  }

  if (isUpdated) {
    updateCallback_();
  }

  return isUpdated;
}

bool
InterfaceEntry::isActive() {
  if (not isUp_) {
    return false;
  }

  const auto lastErrorTime = backoff_.getLastErrorTime();
  const auto now = std::chrono::steady_clock::now();
  if (now - lastErrorTime > backoff_.getMaxBackoff()) {
    backoff_.reportSuccess();
  }
  return backoff_.canTryNow();
}

std::chrono::milliseconds
InterfaceEntry::getBackoffDuration() const {
  return backoff_.getTimeRemainingUntilRetry();
}

bool
InterfaceEntry::updateAddr(folly::CIDRNetwork const& ipNetwork, bool isValid) {
  bool isUpdated = false;
  if (isValid) {
    isUpdated |= networks_.insert(ipNetwork).second;
  } else {
    isUpdated |= networks_.erase(ipNetwork) == 1;
  }

  if (isUpdated) {
    VLOG(1) << (isValid ? "Adding " : "Deleting ")
            << folly::sformat("{}/{}", ipNetwork.first.str(), ipNetwork.second)
            << " on interface " << ifName_
            << ", status: " << (isUp() ? "UP" : "DOWN");
  }

  if (isUpdated and isActive()) {
    updateCallback_();
  }

  return isUpdated;
}

std::unordered_set<folly::IPAddress>
InterfaceEntry::getV4Addrs() const {
  std::unordered_set<folly::IPAddress> v4Addrs;
  for (auto const& ntwk : networks_) {
    if (ntwk.first.isV4()) {
      v4Addrs.insert(ntwk.first);
    }
  }
  return v4Addrs;
}

std::unordered_set<folly::IPAddress>
InterfaceEntry::getV6LinkLocalAddrs() const {
  std::unordered_set<folly::IPAddress> v6Addrs;
  for (auto const& ntwk : networks_) {
    if (ntwk.first.isV6() && ntwk.first.isLinkLocal()) {
      v6Addrs.insert(ntwk.first);
    }
  }
  return v6Addrs;
}

std::vector<thrift::PrefixEntry>
InterfaceEntry::getGlobalUnicastNetworks(bool enableV4) const {
  std::vector<thrift::PrefixEntry> prefixes;
  for (auto const& ntwk : networks_) {
    auto const& ip = ntwk.first;
    // Ignore irrelevant ip addresses.
    if (ip.isLoopback() || ip.isLinkLocal() || ip.isMulticast()) {
      continue;
    }

    // Ignore v4 address if not enabled
    if (ip.isV4() and not enableV4) {
      continue;
    }

    auto prefixEntry = openr::thrift::PrefixEntry();
    prefixEntry.prefix =
        toIpPrefix(std::make_pair(ip.mask(ntwk.second), ntwk.second));
    prefixEntry.type = thrift::PrefixType::LOOPBACK;
    prefixEntry.data = "";
    prefixEntry.forwardingType = thrift::PrefixForwardingType::IP;
    prefixEntry.ephemeral_ref().reset();
    prefixes.push_back(prefixEntry);
  }

  return prefixes;
}

thrift::InterfaceInfo
InterfaceEntry::getInterfaceInfo() const {
  std::vector<thrift::IpPrefix> networks;
  for (const auto& network : networks_) {
    networks.emplace_back(toIpPrefix(network));
  }

  return createThriftInterfaceInfo(isUp_, ifIndex_, networks);
}

} // namespace openr
