/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/fbmeshd/routing/SyncRoutes80211s.h"

#include <sys/ioctl.h>

#include <chrono>

#include <folly/MacAddress.h>
#include <folly/system/ThreadName.h>

using namespace std::chrono_literals;
using namespace openr::fbmeshd;

namespace {

const auto kSyncRoutesInterval{1s};

folly::IPAddressV6
getIPV6FromMacAddress(const char* prefix, folly::MacAddress macAddress) {
  folly::ByteArray16 bytes;
  const auto* macBytes = macAddress.bytes();
  memcpy(&bytes.front(), prefix, 8);
  bytes[8] = uint8_t(macBytes[0] ^ 0x02);
  bytes[9] = macBytes[1];
  bytes[10] = macBytes[2];
  bytes[11] = 0xff;
  bytes[12] = 0xfe;
  bytes[13] = macBytes[3];
  bytes[14] = macBytes[4];
  bytes[15] = macBytes[5];

  return folly::IPAddressV6::fromBinary(bytes);
}

folly::IPAddressV6
getTaygaIPV6FromMacAddress(folly::MacAddress macAddress) {
  return getIPV6FromMacAddress("\xfd\x00\x00\x00\x00\x00\x00\x00", macAddress);
}

folly::IPAddressV6
getMeshIPV6FromMacAddress(folly::MacAddress macAddress) {
  return getIPV6FromMacAddress("\xfc\x00\x00\x00\x00\x00\x00\x00", macAddress);
}

bool
isInterfaceUp(std::string interface) {
  VLOG(8) << folly::sformat("::{}(interface: {})", __func__, interface);

  int sock = socket(PF_INET6, SOCK_DGRAM, IPPROTO_IP);

  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strcpy(ifr.ifr_name, interface.c_str());

  int ret = ioctl(sock, SIOCGIFFLAGS, &ifr);
  close(sock);

  bool up = false;
  if (ret >= 0) {
    up = ifr.ifr_flags & IFF_UP;
  } else {
    LOG(WARNING) << "Failed to get SIOCGIFFLAGS, interface may not exist";
    return false;
  }

  VLOG(10) << folly::sformat(
      "Detected interface {} is {}", interface, up ? "up" : "down");
  return up;
}

} // namespace

SyncRoutes80211s::SyncRoutes80211s(
    Routing* routing,
    std::unique_ptr<openr::rnl::NetlinkProtocolSocket> nlProtocolSocket,
    folly::MacAddress nodeAddr,
    const std::string& interface)
    : routing_{routing},
      nodeAddr_{nodeAddr},
      interface_{interface},
      netlinkSocket_{this, nullptr, std::move(nlProtocolSocket)} {
  // Set timer to sync routes
  syncRoutesTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { doSyncRoutes(); });
  syncRoutesTimer_->scheduleTimeout(kSyncRoutesInterval, true);
}

void
SyncRoutes80211s::doSyncRoutes() {
  VLOG(8) << folly::sformat("SyncRoutes80211s::{}()", __func__);

  auto meshIfIndex = netlinkSocket_.getIfIndex(interface_).get();
  auto isGate = routing_->getGatewayStatus();
  auto meshPaths = routing_->getMeshPaths();

  openr::rnl::NlUnicastRoutes unicastRouteDb;
  openr::rnl::NlLinkRoutes linkRouteDb;
  std::vector<rnl::IfAddress> meshAddrs;

  const auto kTaygaIfName{"tayga"};
  auto taygaIfIndex = netlinkSocket_.getIfIndex(kTaygaIfName).get();
  bool taygaIfUp = isInterfaceUp(kTaygaIfName);

  folly::Optional<std::pair<folly::MacAddress, uint32_t>> bestGate;
  bool isCurrentGateStillAlive = false;
  for (const auto& mpathIt : meshPaths) {
    const auto& mpath = mpathIt.second;

    if (mpath.nextHop == folly::MacAddress::ZERO) {
      continue;
    }

    auto destination = std::make_pair<folly::IPAddress, uint8_t>(
        getTaygaIPV6FromMacAddress(mpath.dst), 128);
    // Ensure tayga interface is present and up
    if (taygaIfIndex != 0 && taygaIfUp) {
      unicastRouteDb.emplace(
          destination,
          rnl::RouteBuilder{}
              .setDestination(destination)
              .setProtocolId(98)
              .addNextHop(rnl::NextHopBuilder{}
                              .setGateway(folly::IPAddressV6{
                                  folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
                                  mpath.nextHop})
                              .setIfIndex(meshIfIndex)
                              .build())
              .build());
    }
    destination = std::make_pair<folly::IPAddress, uint8_t>(
        getMeshIPV6FromMacAddress(mpath.dst), 128);
    unicastRouteDb.emplace(
        destination,
        rnl::RouteBuilder{}
            .setDestination(destination)
            .setProtocolId(98)
            .addNextHop(rnl::NextHopBuilder{}
                            .setGateway(folly::IPAddressV6{
                                folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
                                mpath.nextHop})
                            .setIfIndex(meshIfIndex)
                            .build())
            .build());

    if (mpath.expTime > std::chrono::steady_clock::now() && mpath.isGate) {
      if (currentGate_ && currentGate_->first == mpath.dst) {
        isCurrentGateStillAlive = true;
        currentGate_->second = mpath.metric;
      }
      if (!bestGate || bestGate->second > mpath.metric) {
        bestGate = std::make_pair(mpath.dst, mpath.metric);
      }
    }
  }
  if (bestGate) {
    VLOG(10) << "Best gate: " << bestGate->first
             << " with metric: " << bestGate->second;
  } else {
    VLOG(10) << "No gate found";
  }
  if (currentGate_ && isCurrentGateStillAlive) {
    if (bestGate->second < currentGate_->second) {
      currentGate_ = bestGate;
    }
  } else {
    currentGate_ = bestGate;
  }
  if (currentGate_) {
    VLOG(10) << "Current gate: " << currentGate_->first
             << " with metric: " << currentGate_->second;
  } else {
    VLOG(10) << "No current gate found";
  }

  auto destination =
      folly::CIDRNetwork{getTaygaIPV6FromMacAddress(nodeAddr_), 128};

  // Ensure tayga interface is present and up
  if (taygaIfIndex != 0 && taygaIfUp) {
    linkRouteDb.emplace(
        std::make_pair(destination, kTaygaIfName),
        rnl::RouteBuilder{}
            .setDestination(destination)
            .setProtocolId(98)
            .setRouteIfIndex(taygaIfIndex)
            .setRouteIfName(kTaygaIfName)
            .buildLinkRoute());

    destination = folly::CIDRNetwork{folly::IPAddressV4{"172.16.0.0"}, 16};
    linkRouteDb.emplace(
        std::make_pair(destination, kTaygaIfName),
        rnl::RouteBuilder{}
            .setDestination(destination)
            .setProtocolId(98)
            .setRouteIfIndex(taygaIfIndex)
            .setRouteIfName(kTaygaIfName)
            .buildLinkRoute());
  }

  meshAddrs.push_back(rnl::IfAddressBuilder{}
                          .setPrefix(folly::CIDRNetwork{
                              getMeshIPV6FromMacAddress(nodeAddr_), 64})
                          .setIfIndex(meshIfIndex)
                          .build());

  netlinkSocket_.syncIfAddress(
      meshIfIndex, meshAddrs, AF_INET6, RT_SCOPE_UNIVERSE);

  if (isGateBeforeRouteSync_ != isGate) {
    netlinkSocket_.syncUnicastRoutes(98, std::move(unicastRouteDb)).get();
    netlinkSocket_.syncLinkRoutes(98, std::move(linkRouteDb)).get();
  }

  destination = std::make_pair<folly::IPAddress, uint8_t>(
      folly::IPAddressV6{"fd00:ffff::"}, 96);

  // Ensure tayga interface is present and up
  if (taygaIfIndex != 0 && taygaIfUp) {
    if (isGate) {
      linkRouteDb.emplace(
          std::make_pair(destination, kTaygaIfName),
          rnl::RouteBuilder{}
              .setDestination(destination)
              .setProtocolId(98)
              .setRouteIfIndex(taygaIfIndex)
              .setRouteIfName(kTaygaIfName)
              .buildLinkRoute());
    } else if (currentGate_) {
      const auto defaultV4Prefix =
          std::make_pair<folly::IPAddress, uint8_t>(folly::IPAddressV4{}, 0);

      // ip route add default dev tayga mtu 1500 advmss 1460
      linkRouteDb.emplace(
          std::make_pair(defaultV4Prefix, kTaygaIfName),
          rnl::RouteBuilder{}
              .setDestination(defaultV4Prefix)
              .setProtocolId(98)
              .setMtu(1500)
              .setAdvMss(1460)
              .setRouteIfIndex(taygaIfIndex)
              .setRouteIfName(kTaygaIfName)
              .buildLinkRoute());

      unicastRouteDb.emplace(
          destination,
          rnl::RouteBuilder{}
              .setDestination(destination)
              .setProtocolId(98)
              .addNextHop(rnl::NextHopBuilder{}
                              .setGateway(folly::IPAddressV6{
                                  folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
                                  meshPaths.at(currentGate_->first).nextHop})
                              .setIfIndex(meshIfIndex)
                              .build())
              .build());
    }
  }
  isGateBeforeRouteSync_ = isGate;

  netlinkSocket_.syncUnicastRoutes(98, std::move(unicastRouteDb)).get();
  netlinkSocket_.syncLinkRoutes(98, std::move(linkRouteDb)).get();
}
