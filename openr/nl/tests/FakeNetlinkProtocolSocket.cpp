/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/nl/tests/FakeNetlinkProtocolSocket.h"

namespace openr::fbnl {

folly::SemiFuture<int>
FakeNetlinkProtocolSocket::addRoute(const fbnl::Route& /* route */) {
  CHECK(false) << "Not implemented";
}

folly::SemiFuture<int>
FakeNetlinkProtocolSocket::deleteRoute(const fbnl::Route& /* route */) {
  CHECK(false) << "Not implemented";
}

folly::SemiFuture<std::vector<fbnl::Route>>
FakeNetlinkProtocolSocket::getRoutes(const fbnl::Route& /* filter */) {
  CHECK(false) << "Not implemented";
}

folly::SemiFuture<int>
FakeNetlinkProtocolSocket::addIfAddress(const fbnl::IfAddress& addr) {
  // Search for addr list of interface index (it must exists)
  auto it = ifAddrs_.find(addr.getIfIndex());
  if (it == ifAddrs_.end() or !addr.getPrefix().has_value()) {
    return folly::SemiFuture<int>(-ENXIO); // No such device or address
  }

  // Find if existing. Return EEXIST
  for (auto addrIt = it->second.begin(); addrIt != it->second.end(); ++addrIt) {
    if (addrIt->getPrefix() == addr.getPrefix()) {
      return folly::SemiFuture<int>(-EEXIST);
    }
  }

  // Non existing address. Add
  it->second.emplace_back(addr); // Add
  return folly::SemiFuture<int>(0);
}

folly::SemiFuture<int>
FakeNetlinkProtocolSocket::deleteIfAddress(const fbnl::IfAddress& addr) {
  // Search for addr list of interface index (it must exists)
  auto it = ifAddrs_.find(addr.getIfIndex());
  if (it == ifAddrs_.end() or !addr.getPrefix().has_value()) {
    return folly::SemiFuture<int>(-ENXIO); // No such device or address
  }

  // Find & delete
  for (auto addrIt = it->second.begin(); addrIt != it->second.end(); ++addrIt) {
    if (addrIt->getPrefix() == addr.getPrefix()) {
      it->second.erase(addrIt);
      return folly::SemiFuture<int>(0);
    }
  }

  // Address not available for deletion
  return folly::SemiFuture<int>(-EADDRNOTAVAIL);
}

folly::SemiFuture<std::vector<fbnl::IfAddress>>
FakeNetlinkProtocolSocket::getAllIfAddresses() {
  std::vector<fbnl::IfAddress> addrs;
  for (auto& [_, addrs_] : ifAddrs_) {
    addrs.insert(addrs.end(), addrs_.begin(), addrs_.end());
  }
  return folly::SemiFuture<std::vector<fbnl::IfAddress>>(std::move(addrs));
}

folly::SemiFuture<int>
FakeNetlinkProtocolSocket::addLink(const fbnl::Link& link) {
  // Check if link exists already
  if (links_.count(link.getIfIndex())) {
    return folly::SemiFuture<int>(-EEXIST);
  }

  links_.emplace(link.getIfIndex(), link);
  ifAddrs_.emplace(link.getIfIndex(), std::list<fbnl::IfAddress>());
  return folly::SemiFuture<int>(0);
}

folly::SemiFuture<std::vector<fbnl::Link>>
FakeNetlinkProtocolSocket::getAllLinks() {
  std::vector<fbnl::Link> links;
  for (auto& [_, link] : links_) {
    links.emplace_back(link);
  }
  return folly::SemiFuture<std::vector<fbnl::Link>>(std::move(links));
}

folly::SemiFuture<std::vector<fbnl::Neighbor>>
FakeNetlinkProtocolSocket::getAllNeighbors() {
  CHECK(false) << "Not implemented";
}

} // namespace openr::fbnl
