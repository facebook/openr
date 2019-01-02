/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include <folly/IPAddress.h>
#include <folly/String.h>

#include <openr/if/gen-cpp2/Lsdb_types.h>

namespace openr {

/**
 * We hold interface information (status and addresses) in this object.
 * We can create objects with both status and addresses together or only from
 * link status information
 *
 * Piecemeal updates in response to link event or address events are supported
 * We assume address events can never arrive before a link event
 *
 * Interface must always be sent on creation and on updates (link or address)
 */
class InterfaceEntry final {
 public:
  InterfaceEntry() = default;
  ~InterfaceEntry() = default;

  InterfaceEntry(const InterfaceEntry&) = default;
  InterfaceEntry(InterfaceEntry&&) = default;

  InterfaceEntry& operator=(const InterfaceEntry&) = default;
  InterfaceEntry& operator=(InterfaceEntry&&) = default;

  // Creating entries when we have all link information
  InterfaceEntry(
      int ifIndex,
      bool isUp,
      uint64_t weight,
      const std::unordered_set<folly::CIDRNetwork>& networks)
      : ifIndex_(ifIndex),
        isUp_(isUp),
        weight_(weight),
        networks_(networks) {}

  // Creating entries only from link status information
  InterfaceEntry(int ifIndex, bool isUp) : ifIndex_(ifIndex), isUp_(isUp) {}

  // Update methods for link and address events
  bool
  updateEntry(int ifIndex, bool isUp, uint64_t weight) {
    bool isUpdated = false;
    isUpdated |= std::exchange(ifIndex_, ifIndex) != ifIndex;
    isUpdated |= std::exchange(isUp_, isUp) != isUp;
    isUpdated |= std::exchange(weight_, weight) != weight;
    return isUpdated;
  }

  bool
  updateEntry(const folly::CIDRNetwork& ipNetwork, bool isValid) {
    bool isUpdated = false;
    if (isValid) {
      isUpdated |= (networks_.insert(ipNetwork)).second;
    } else {
      isUpdated |= (networks_.erase(ipNetwork) == 1);
    }
    return isUpdated;
  }

  // Used to check for updates if doing a re-sync
  bool
  operator==(const InterfaceEntry& interfaceEntry) {
    return (
        (ifIndex_ == interfaceEntry.getIfIndex()) &&
        (isUp_ == interfaceEntry.isUp()) &&
        (networks_ == interfaceEntry.getNetworks()) &&
        (weight_ == interfaceEntry.getWeight()));
  }

  friend std::ostream&
  operator<<(std::ostream& out, const InterfaceEntry& interfaceEntry) {
    out << "Interface data: " << (interfaceEntry.isUp() ? "UP" : "DOWN")
        << " ifIndex: " << interfaceEntry.getIfIndex()
        << " weight: " << interfaceEntry.getWeight() << " IPv6ll: "
        << folly::join(", ", interfaceEntry.getV6LinkLocalAddrs())
        << " IPv4: " << folly::join(", ", interfaceEntry.getV4Addrs());
    return out;
  }

  bool
  isUp() const {
    return isUp_;
  }
  int
  getIfIndex() const {
    return ifIndex_;
  }
  uint64_t
  getWeight() const {
    return weight_;
  }

  // returns const references for optimization
  const std::unordered_set<folly::CIDRNetwork>&
  getNetworks() const {
    return networks_;
  }

  std::unordered_set<folly::IPAddress>
  getV4Addrs() const {
    std::unordered_set<folly::IPAddress> v4Addrs;
    for (auto const& ntwk : networks_) {
      if (ntwk.first.isV4()) {
        v4Addrs.insert(ntwk.first);
      }
    }
    return v4Addrs;
  }
  std::unordered_set<folly::IPAddress>
  getV6LinkLocalAddrs() const {
    std::unordered_set<folly::IPAddress> v6Addrs;
    for (auto const& ntwk : networks_) {
      if (ntwk.first.isV6() && ntwk.first.isLinkLocal()) {
          v6Addrs.insert(ntwk.first);
      }
    }
    return v6Addrs;
  }

  // Create the Interface info for Interface request
  thrift::InterfaceInfo getInterfaceInfo() const;

 private:
  int ifIndex_{0};
  bool isUp_{false};
  uint64_t weight_{1};

  // We keep the set of IPs and push to Spark
  // Spark really cares about one, but we let
  // Spark handle that
  std::unordered_set<folly::CIDRNetwork> networks_;
};

} // namespace openr
