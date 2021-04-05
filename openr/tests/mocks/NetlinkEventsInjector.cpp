/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

  linkDb_.withRLock([&](auto& linkDb) {
    for (const auto& [_, info] : linkDb) {
      ifDb.emplace_back(info);
    }
  });
}

void
NetlinkEventsInjector::sendLinkEvent(
    const std::string& ifName, const uint64_t ifIndex, const bool isUp) {
  // Update cached linkDb_ for link event
  linkDb_.withWLock([&](auto& linkDb) {
    auto it = linkDb.find(ifName);
    if (it == linkDb.end()) {
      InterfaceInfo info(ifName, isUp, ifIndex, {});
      linkDb.emplace(ifName, info);
    } else {
      it->second.isUp = isUp;
      CHECK_EQ(it->second.ifIndex, ifIndex) << fmt::format(
          "Interface index changed from {} to {}", it->second.ifIndex, ifIndex);
    }
  });

  // Send event to NetlinkProtocolSocket
  fbnl::LinkBuilder builder;
  auto link = builder.setLinkName(ifName)
                  .setIfIndex(ifIndex)
                  .setFlags(isUp ? IFF_RUNNING : 0)
                  .build();
  nlSock_->addLink(link).get();
}

void
NetlinkEventsInjector::sendAddrEvent(
    const std::string& ifName, const std::string& prefix, const bool isValid) {
  const auto network = folly::IPAddress::createNetwork(prefix, -1, false);

  // Update cached linkDb_ for address event
  std::optional<int> ifIndex;
  linkDb_.withWLock([&](auto& linkDb) {
    auto& link = linkDb.at(ifName);
    ifIndex = link.ifIndex;
    if (isValid) {
      link.networks.insert(network);
    } else {
      link.networks.erase(network);
    }
  });

  // Send event to NetlinkProtocolSocket
  CHECK(ifIndex.has_value()) << fmt::format("Unknown interface: {}", ifName);

  fbnl::IfAddressBuilder builder;
  auto addr = builder.setIfIndex(ifIndex.value())
                  .setPrefix(network)
                  .setValid(isValid)
                  .build();
  if (isValid) {
    nlSock_->addIfAddress(addr).get();
  } else {
    nlSock_->deleteIfAddress(addr).get();
  }
}

} // namespace openr
