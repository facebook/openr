/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/IPAddress.h>
#include <folly/String.h>
#include <folly/io/async/AsyncTimeout.h>

#include <openr/common/AsyncThrottle.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

/**
 * Holds interface attributes along with complex information like backoffs. All
 * updates made into this object is reflected asynchronously via throttled
 * callback or timers provided in constructor.
 *
 * - Any change will always trigger throttled callback
 * - Interface transition from Active to Inactive schedules immediate timeout
 *   for fast reactions to down events.
 */
class InterfaceEntry final {
 public:
  InterfaceEntry(
      std::string const& ifName,
      std::chrono::milliseconds const& initBackoff,
      std::chrono::milliseconds const& maxBackoff,
      AsyncThrottle& updateCallback,
      folly::AsyncTimeout& updateTimeout);

  // Update attributes
  bool updateAttrs(int ifIndex, bool isUp);

  // Update addresses
  bool updateAddr(folly::CIDRNetwork const& ipNetwork, bool isValid);

  // Is interface active. Interface is active only when it is in UP state and
  // it's not backed off
  bool isActive();

  // Get backoff time
  std::chrono::milliseconds getBackoffDuration() const;

  // Used to check for updates if doing a re-sync
  bool
  operator==(const InterfaceEntry& interfaceEntry) {
    return (
        (info_.ifName == interfaceEntry.getIfName()) &&
        (info_.ifIndex == interfaceEntry.getIfIndex()) &&
        (info_.isUp == interfaceEntry.isUp()) &&
        (info_.networks == interfaceEntry.getNetworks()));
  }

  std::string
  getIfName() const {
    return info_.ifName;
  }

  int
  getIfIndex() const {
    return info_.ifIndex;
  }

  bool
  isUp() const {
    return info_.isUp;
  }

  // returns const references for optimization
  const std::unordered_set<folly::CIDRNetwork>&
  getNetworks() const {
    return info_.networks;
  }

  // create InterfaceInfo object for message passing
  InterfaceInfo
  getInterfaceInfo() const {
    return info_;
  }

  // Utility function to retrieve re-distribute addresses
  std::vector<folly::CIDRNetwork> getGlobalUnicastNetworks(bool enableV4) const;

 private:
  // Backoff variables
  ExponentialBackoff<std::chrono::milliseconds> backoff_;

  // Update callback
  AsyncThrottle& updateCallback_;
  folly::AsyncTimeout& updateTimeout_;

  // Data-structure representing interface information
  InterfaceInfo info_;
};

} // namespace openr
