/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MockNetlinkSystemHandler.h"

#include <algorithm>
#include <functional>
#include <thread>
#include <utility>

#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/futures/Promise.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/BinaryProtocol.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/AddressUtil.h>

using namespace fbzmq;
using apache::thrift::FRAGILE;

namespace openr {
MockNetlinkSystemHandler::MockNetlinkSystemHandler(
    fbzmq::Context& context, const std::string& platformPublisherUrl) {
  VLOG(3) << "Building Mock NL Db";

  platformPublisher_ = std::make_unique<PlatformPublisher>(
      context, PlatformPublisherUrl{platformPublisherUrl});
}

void
MockNetlinkSystemHandler::getAllLinks(std::vector<thrift::Link>& linkDb) {
  VLOG(3) << "Query links from Netlink according to link name";
  SYNCHRONIZED(linkDb_) {
    for (const auto link : linkDb_) {
      thrift::Link linkEntry;
      linkEntry.ifName = link.first;
      linkEntry.ifIndex = link.second.ifIndex;
      linkEntry.isUp = link.second.isUp;
      for (const auto network : link.second.networks) {
        linkEntry.networks.push_back(thrift::IpPrefix(
            FRAGILE, toBinaryAddress(network.first), network.second));
      }
      linkDb.push_back(linkEntry);
    }
  }
}

void
MockNetlinkSystemHandler::getAllNeighbors(
    std::vector<thrift::NeighborEntry>& neighborDb) {
  VLOG(3) << "Query all reachable neighbors from Netlink";
  SYNCHRONIZED(neighborDb_) {
    for (const auto kv : neighborDb_) {
      thrift::NeighborEntry neighborEntry = thrift::NeighborEntry(
          FRAGILE,
          kv.first.first,
          toBinaryAddress(kv.first.second),
          kv.second.toString(),
          true);
      neighborDb.push_back(neighborEntry);
    }
  }
}

void
MockNetlinkSystemHandler::sendLinkEvent(
    const std::string& ifName, const uint64_t ifIndex, const bool isUp) {
  // Update linkDb_
  SYNCHRONIZED(linkDb_) {
    if (!linkDb_.count(ifName)) {
      LinkAttributes newLinkEntry;
      newLinkEntry.isUp = isUp;
      newLinkEntry.ifIndex = ifIndex;
      linkDb_.emplace(ifName, newLinkEntry);
    } else {
      linkDb_[ifName].isUp = isUp;
      linkDb_[ifName].ifIndex = ifIndex;
    }
  }

  // Pass message thru zmq to subscribers
  platformPublisher_->publishLinkEvent(thrift::LinkEntry(
      FRAGILE, ifName, ifIndex, isUp, Constants::kDefaultAdjWeight));
}

void
MockNetlinkSystemHandler::sendAddrEvent(
    const std::string& ifName, const std::string& prefix, const bool isValid) {
  const auto ipNetwork = folly::IPAddress::createNetwork(prefix, -1, false);

  // Update linkDb_
  SYNCHRONIZED(linkDb_) {
    if (isValid) {
      linkDb_[ifName].networks.insert(ipNetwork);
    } else {
      linkDb_[ifName].networks.erase(ipNetwork);
    }
  }

  platformPublisher_->publishAddrEvent(thrift::AddrEntry(
      FRAGILE,
      ifName,
      thrift::IpPrefix(
          FRAGILE,
          toBinaryAddress(ipNetwork.first),
          ipNetwork.second
      ),
      isValid));
}

void
MockNetlinkSystemHandler::stop() {
  platformPublisher_->stop();
}

} // namespace openr
