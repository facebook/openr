/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/gen/Base.h>
#include <folly/logging/xlog.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/link-monitor/InterfaceEntry.h>

namespace openr {

InterfaceEntry::InterfaceEntry(
    std::string const& ifName,
    std::chrono::milliseconds const& initBackoff,
    std::chrono::milliseconds const& maxBackoff,
    AsyncThrottle& updateCallback,
    folly::AsyncTimeout& updateTimeout)
    : backoff_(initBackoff, maxBackoff),
      updateCallback_(updateCallback),
      updateTimeout_(updateTimeout) {
  CHECK(!ifName.empty());
  // other attributes will be updated via:
  //  - updateAttrs()
  //  - updateAddr()
  info_.ifName = ifName;
}

bool
InterfaceEntry::updateAttrs(int ifIndex, bool isUp) {
  const bool wasActive = isActive();
  const bool wasUp = info_.isUp;
  bool isUpdated = false;
  isUpdated |= ((std::exchange(info_.ifIndex, ifIndex) != ifIndex) ? 1 : 0);
  isUpdated |= ((std::exchange(info_.isUp, isUp) != isUp) ? 1 : 0);

  // Update timestamp only at when link status changed
  if (wasUp != isUp) {
    info_.statusChangeTimestamp = getUnixTimeStampMs();
  }

  // Look for specific case of interface state transition to DOWN
  if (wasUp != isUp && wasUp) {
    // Penalize backoff on transitioning to DOWN state
    backoff_.reportError();
  }

  // Look for active to down transition
  if (wasActive && !isUp) {
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
  if (!info_.isUp) {
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
    isUpdated |= ((info_.networks.insert(ipNetwork).second) ? 1 : 0);
  } else {
    isUpdated |= (((info_.networks.erase(ipNetwork) == 1)) ? 1 : 0);
  }

  XLOG_IF(DBG1, isUpdated) << fmt::format(
      "{} {}/{} on interface {}, status: {}",
      isValid ? "Adding" : "Deleting",
      ipNetwork.first.str(),
      ipNetwork.second,
      info_.ifName,
      isUp() ? "UP" : "DOWN");

  if (isUpdated && isActive()) {
    updateCallback_();
  }

  return isUpdated;
}

std::vector<folly::CIDRNetwork>
InterfaceEntry::getGlobalUnicastNetworks(bool enableV4) const {
  std::vector<folly::CIDRNetwork> prefixes;
  for (auto const& [ip, mask] : info_.networks) {
    // Ignore irrelevant link addresses
    if (ip.isLoopback() || ip.isLinkLocal() || ip.isMulticast()) {
      continue;
    }

    // Ignore v4 address if not enabled
    if (ip.isV4() && !enableV4) {
      continue;
    }

    // Mask and add subnet for advertisement
    prefixes.emplace_back(ip.mask(mask), mask);
  }

  return prefixes;
}

} // namespace openr
