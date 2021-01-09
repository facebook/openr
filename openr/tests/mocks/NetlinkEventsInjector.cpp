/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/mocks/NetlinkEventsInjector.h>
#include <glog/logging.h>
#include <openr/common/NetworkUtil.h>
#include <openr/nl/NetlinkTypes.h>

extern "C" {
#include <net/if.h>
}

namespace openr {

NetlinkEventsInjector::NetlinkEventsInjector(
    fbnl::MockNetlinkProtocolSocket* nlSock)
    : nlSock_(nlSock) {}

void
NetlinkEventsInjector::getAllLinks(InterfaceDatabase& ifDb) {
  VLOG(3) << "Query links from Netlink according to link name";
  SYNCHRONIZED(linkDb_) {
    for (const auto& link : linkDb_) {
      InterfaceInfo info(
          link.first,
          link.second.isUp,
          link.second.ifIndex,
          link.second.networks);
      ifDb.emplace_back(std::move(info));
    }
  }
}

void
NetlinkEventsInjector::sendLinkEvent(
    const std::string& ifName, const uint64_t ifIndex, const bool isUp) {
  // Update linkDb_
  SYNCHRONIZED(linkDb_) {
    if (!linkDb_.count(ifName)) {
      fbnl::LinkAttribute newLinkEntry;
      newLinkEntry.isUp = isUp;
      newLinkEntry.ifIndex = ifIndex;
      linkDb_.emplace(ifName, newLinkEntry);
    } else {
      auto& link = linkDb_.at(ifName);
      link.isUp = isUp;
      CHECK_EQ(link.ifIndex, ifIndex) << "Interface index changed";
    }
  }

  // Send event to NetlinkProtocolSocket
  fbnl::LinkBuilder builder;
  builder.setLinkName(ifName);
  builder.setIfIndex(ifIndex);
  builder.setFlags(isUp ? IFF_RUNNING : 0);
  nlSock_->addLink(builder.build()).get();
}

void
NetlinkEventsInjector::sendAddrEvent(
    const std::string& ifName, const std::string& prefix, const bool isValid) {
  const auto ipNetwork = folly::IPAddress::createNetwork(prefix, -1, false);

  // Update linkDb_
  std::optional<int> ifIndex;
  SYNCHRONIZED(linkDb_) {
    auto& link = linkDb_.at(ifName);
    ifIndex = link.ifIndex;
    if (isValid) {
      link.networks.insert(ipNetwork);
    } else {
      link.networks.erase(ipNetwork);
    }
  }

  // Send event to NetlinkProtocolSocket
  CHECK(ifIndex.has_value()) << "Unknown interface";
  fbnl::IfAddressBuilder builder;
  builder.setIfIndex(ifIndex.value());
  builder.setPrefix(ipNetwork);
  builder.setValid(isValid);
  if (isValid) {
    nlSock_->addIfAddress(builder.build()).get();
  } else {
    nlSock_->deleteIfAddress(builder.build()).get();
  }
}

} // namespace openr
