/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Gateway11sRootRouteProgrammer.h"

#include <folly/Subprocess.h>

using namespace openr::fbmeshd;

namespace {
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
getMesh0IPV6FromMacAddress(folly::MacAddress macAddress) {
  return getIPV6FromMacAddress("\xfc\x00\x00\x00\x00\x00\x00\x00", macAddress);
}

} // namespace

Gateway11sRootRouteProgrammer::Gateway11sRootRouteProgrammer(
    openr::fbmeshd::Nl80211Handler& nlHandler,
    std::chrono::seconds const interval,
    double const gatewayChangeThresholdFactor)
    : nlHandler_{nlHandler},
      netlinkSocket_{this, nullptr},
      gatewayChangeThresholdFactor_{gatewayChangeThresholdFactor} {
  const NetInterface& netif = nlHandler_.lookupMeshNetif();

  folly::Subprocess{
      std::vector<std::string>{
          "/sbin/ip",
          "-6",
          "neigh",
          "add",
          "proxy",
          getTaygaIPV6FromMacAddress(*netif.maybeMacAddress).str(),
          "dev",
          "mesh0"}}
      .wait();

  timer_ = fbzmq::ZmqTimeout::make(
      this, [this]() mutable noexcept { determineBestRoot(); });
  timer_->scheduleTimeout(interval, true);
}

void
Gateway11sRootRouteProgrammer::determineBestRoot() {
  const NetInterface& netif = nlHandler_.lookupMeshNetif();

  folly::Optional<std::pair<folly::MacAddress, uint32_t>> bestRoot;

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_GET_MPATH,
                            NLM_F_DUMP | NLM_F_ACK};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  bool isCurrentRootStillAlive = false;
  GenericNetlinkSocket{}.sendAndReceive(
      msg,
      [&bestRoot, &isCurrentRootStillAlive, this](
          const GenericNetlinkMessage& msg) {
        const auto tb = msg.getAttributes<NL80211_ATTR_MAX>();

        if (!tb[NL80211_ATTR_MPATH_INFO]) {
          LOG(INFO) << "mpath info missing";
          return NL_SKIP;
        }

        TabularNetlinkAttribute<NL80211_MPATH_INFO_MAX> pinfo{
            tb[NL80211_ATTR_MPATH_INFO]};

        const auto myMacAddress = folly::MacAddress::fromBinary(
            {static_cast<unsigned char*>(nla_data(tb[NL80211_ATTR_MAC])),
             ETH_ALEN});
        const auto myMetric = nla_get_u32(pinfo[NL80211_MPATH_INFO_METRIC]);
        if (nla_get_u32(pinfo[NL80211_MPATH_INFO_EXPTIME]) > 0 &&
            nla_get_u8(pinfo[NL80211_MPATH_INFO_IS_ROOT])) {
          if (currentRoot_ && currentRoot_->first == myMacAddress) {
            isCurrentRootStillAlive = true;
          }
          if (!bestRoot || bestRoot->second > myMetric) {
            bestRoot = std::make_pair(myMacAddress, myMetric);
          }
        }

        return NL_SKIP;
      });

  if (bestRoot) {
    LOG(INFO) << "Best root: " << bestRoot->first
              << " with metric: " << bestRoot->second;
  } else {
    LOG(INFO) << "No root found";
  }
  if (currentRoot_ && isCurrentRootStillAlive) {
    if (bestRoot->second * gatewayChangeThresholdFactor_ <
        currentRoot_->second) {
      currentRoot_ = bestRoot;
    }
  } else {
    currentRoot_ = bestRoot;
  }
  if (currentRoot_) {
    LOG(INFO) << "Current root: " << currentRoot_->first
              << " with metric: " << currentRoot_->second;
  } else {
    LOG(INFO) << "No current root found";
  }

  openr::fbnl::NlUnicastRoutes routeDb;
  std::vector<fbnl::IfAddress> mesh0Addrs;
  ifIndex = netlinkSocket_.getIfIndex("tayga").get();
  auto destination = std::make_pair<folly::IPAddress, uint8_t>(
      folly::IPAddressV6{"fd00:ffff::"}, 96);

  if (isGate_) {
    routeDb.emplace(
        destination,
        fbnl::RouteBuilder{}
            .setDestination(destination)
            .setProtocolId(98)
            .addNextHop(fbnl::NextHopBuilder{}.setIfIndex(ifIndex).build())
            .build());
  } else if (currentRoot_) {
    const auto defaultV4Prefix =
        std::make_pair<folly::IPAddress, uint8_t>(folly::IPAddressV4{}, 0);

    // ip route add default dev tayga mtu 1260 advmss 1220
    // the MTU is set to 1260 because IPv6 default mtu is 1280, and the IPv4->
    // IPv6 conversion increases the packet size by 20 bytes.
    routeDb.emplace(
        defaultV4Prefix,
        fbnl::RouteBuilder{}
            .setDestination(defaultV4Prefix)
            .setProtocolId(98)
            .setMtu(1500)
            .setAdvMss(1460)
            .addNextHop(fbnl::NextHopBuilder{}.setIfIndex(ifIndex).build())
            .build());

    routeDb.emplace(
        destination,
        fbnl::RouteBuilder{}
            .setDestination(destination)
            .setProtocolId(98)
            .addNextHop(
                fbnl::NextHopBuilder{}
                    .setGateway(getMesh0IPV6FromMacAddress(currentRoot_->first))
                    .build())
            .build());
  }

  destination = std::make_pair<folly::IPAddress, uint8_t>(
      folly::IPAddressV6{"fd00::"}, 64);
  routeDb.emplace(
      destination,
      fbnl::RouteBuilder{}
          .setDestination(destination)
          .setProtocolId(98)
          .addNextHop(fbnl::NextHopBuilder{}
                          .setIfIndex(netif.maybeIfIndex.value())
                          .build())
          .build());

  destination = folly::CIDRNetwork{
      getTaygaIPV6FromMacAddress(*netif.maybeMacAddress), 128};
  routeDb.emplace(
      destination,
      fbnl::RouteBuilder{}
          .setDestination(destination)
          .setProtocolId(98)
          .addNextHop(fbnl::NextHopBuilder{}.setIfIndex(ifIndex).build())
          .build());

  destination = folly::CIDRNetwork{folly::IPAddressV4{"172.16.0.0"}, 16};
  routeDb.emplace(
      destination,
      fbnl::RouteBuilder{}
          .setDestination(destination)
          .setProtocolId(98)
          .addNextHop(fbnl::NextHopBuilder{}.setIfIndex(ifIndex).build())
          .build());

  mesh0Addrs.push_back(
      fbnl::IfAddressBuilder{}
          .setPrefix(folly::CIDRNetwork{
              getMesh0IPV6FromMacAddress(*netif.maybeMacAddress), 64})
          .setIfIndex(netif.maybeIfIndex.value())
          .build());

  netlinkSocket_.syncIfAddress(
      netif.maybeIfIndex.value(), mesh0Addrs, AF_INET6, RT_SCOPE_UNIVERSE);
  netlinkSocket_.syncUnicastRoutes(98, std::move(routeDb)).get();
}

void
Gateway11sRootRouteProgrammer::setGatewayStatus(bool isGate) {
  runImmediatelyOrInEventLoop([isGate, this]() { isGate_ = isGate; });
}
