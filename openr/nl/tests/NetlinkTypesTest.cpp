/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/nl/NetlinkTypes.h>

#include <glog/logging.h>
#include <gtest/gtest.h>

extern "C" {
#include <linux/rtnetlink.h>
#include <net/if.h>
}

using namespace openr;
using namespace openr::fbnl;

const uint8_t kProtocolId = 99;
const int kIfIndex = 1;
const uint8_t kWeight = 4;

TEST(NetlinkTypes, NextHopIfIndexTest) {
  // Create NextHop with ifindex
  NextHopBuilder builder;
  auto nh = builder.setIfIndex(kIfIndex).build();
  EXPECT_TRUE(nh.getIfIndex().has_value());
  EXPECT_EQ(kIfIndex, nh.getIfIndex().value());
  EXPECT_FALSE(nh.getGateway().has_value());
  EXPECT_EQ(0, nh.getWeight());
}

TEST(NetlinkTypes, NextHopGatewayTest) {
  // Create NextHop with gateway
  folly::IPAddress gateway("fc00:cafe:3::3");
  NextHopBuilder builder;
  auto nh = builder.setGateway(gateway).setWeight(kWeight).build();
  EXPECT_FALSE(nh.getIfIndex().has_value());
  EXPECT_TRUE(nh.getGateway().has_value());
  EXPECT_EQ(gateway, nh.getGateway().value());
  EXPECT_EQ(kWeight, nh.getWeight());
}

TEST(NetlinkTypes, NexthopGeneralTest) {
  folly::IPAddress gateway("fc00:cafe:3::3");
  NextHopBuilder builder;
  auto nh = builder.setGateway(gateway)
                .setIfIndex(kIfIndex)
                .setWeight(kWeight)
                .build();
  LOG(INFO) << nh.str();

  // Create nextHop with ifIndex and gateway
  EXPECT_TRUE(nh.getIfIndex().has_value());
  EXPECT_EQ(kIfIndex, nh.getIfIndex().value());
  EXPECT_TRUE(nh.getGateway().has_value());
  EXPECT_EQ(gateway, nh.getGateway().value());
  EXPECT_EQ(kWeight, nh.getWeight());
}

TEST(NetlinkTypes, RouteBaseTest) {
  folly::CIDRNetwork dst{folly::IPAddress("fc00:cafe:3::3"), 128};
  RouteBuilder builder;
  // Use default values
  auto route = builder.setDestination(dst).setProtocolId(kProtocolId).build();
  EXPECT_EQ(AF_INET6, route.getFamily());
  EXPECT_EQ(kProtocolId, route.getProtocolId());
  EXPECT_EQ(RT_SCOPE_UNIVERSE, route.getScope());
  EXPECT_EQ(RT_TABLE_MAIN, route.getRouteTable());
  EXPECT_EQ(dst, route.getDestination());
  EXPECT_EQ(RTN_UNICAST, route.getType());
  EXPECT_FALSE(route.getFlags().has_value());
  EXPECT_TRUE(route.getNextHops().empty());
  EXPECT_FALSE(route.getPriority().has_value());
  EXPECT_FALSE(route.getTos().has_value());
  EXPECT_FALSE(route.getMtu().has_value());
  EXPECT_FALSE(route.getAdvMss().has_value());
}

TEST(NetlinkTypes, RouteEqualTest) {
  folly::CIDRNetwork dst{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::IPAddress gateway1("face:cafe:3::3");
  folly::IPAddress gateway2("face:cafe:3::4");
  folly::IPAddress gateway3("face:cafe:3::5");
  uint32_t flags = 0x01;
  uint32_t priority = 3;
  uint8_t tos = 2;
  uint32_t mtu = 4;
  uint32_t advMss = 5;
  NextHopBuilder nhBuilder;
  auto nh1 = nhBuilder.setIfIndex(kIfIndex).setGateway(gateway1).build();
  nhBuilder.reset();
  auto nh2 = nhBuilder.setIfIndex(kIfIndex).setGateway(gateway2).build();
  RouteBuilder builder1;
  auto route1 = builder1.setDestination(dst)
                    .setType(RTN_UNICAST)
                    .setProtocolId(kProtocolId)
                    .setScope(RT_SCOPE_UNIVERSE)
                    .setRouteTable(RT_TABLE_MAIN)
                    .setFlags(flags)
                    .setPriority(priority)
                    .setTos(tos)
                    .setMtu(mtu)
                    .setAdvMss(advMss)
                    .addNextHop(nh1)
                    .addNextHop(nh2)
                    .build();
  LOG(INFO) << route1.str();

  RouteBuilder builder2;
  nhBuilder.reset();
  nh1 = nhBuilder.setIfIndex(kIfIndex).setGateway(gateway1).build();
  nhBuilder.reset();
  nh2 = nhBuilder.setIfIndex(kIfIndex).setGateway(gateway2).build();
  auto route2 = builder2.setDestination(dst)
                    .setType(RTN_UNICAST)
                    .setProtocolId(kProtocolId)
                    .setScope(RT_SCOPE_UNIVERSE)
                    .setRouteTable(RT_TABLE_MAIN)
                    .setFlags(flags)
                    .setPriority(priority)
                    .setTos(tos)
                    .setMtu(mtu)
                    .setAdvMss(advMss)
                    .addNextHop(nh2)
                    .addNextHop(nh1)
                    .build();
  LOG(INFO) << route2.str();

  EXPECT_TRUE(route1 == route2);
  RouteBuilder builder3;
  nhBuilder.reset();
  nh1 = nhBuilder.setIfIndex(kIfIndex).setGateway(gateway1).build();
  auto route3 = builder3.setDestination(dst)
                    .setType(RTN_UNICAST)
                    .setProtocolId(kProtocolId)
                    .setScope(RT_SCOPE_UNIVERSE)
                    .setRouteTable(RT_TABLE_MAIN)
                    .setFlags(flags)
                    .setPriority(priority)
                    .setTos(tos)
                    .setMtu(mtu)
                    .setAdvMss(advMss)
                    .addNextHop(nh1)
                    .build();
  EXPECT_FALSE(route2 == route3);
  RouteBuilder builder4;
  nhBuilder.reset();
  nh1 = nhBuilder.setIfIndex(kIfIndex).setGateway(gateway1).build();
  auto nh3 = nhBuilder.setIfIndex(kIfIndex).setGateway(gateway3).build();
  auto route4 = builder4.setDestination(dst)
                    .setType(RTN_UNICAST)
                    .setProtocolId(kProtocolId)
                    .setScope(RT_SCOPE_UNIVERSE)
                    .setRouteTable(RT_TABLE_MAIN)
                    .setFlags(flags)
                    .setPriority(priority)
                    .setTos(tos)
                    .setMtu(mtu)
                    .setAdvMss(advMss)
                    .addNextHop(nh1)
                    .addNextHop(nh3)
                    .build();
  EXPECT_FALSE(route2 == route4);

  // Add same nexthop
  nhBuilder.reset();
  auto nh4 = nhBuilder.setIfIndex(kIfIndex).setGateway(gateway1).build();
  nhBuilder.reset();
  auto nh5 = nhBuilder.setIfIndex(kIfIndex).setGateway(gateway1).build();
  RouteBuilder builder5;
  auto route5 =
      builder5.setDestination(dst).addNextHop(nh4).addNextHop(nh5).build();
  EXPECT_EQ(1, route5.getNextHops().size());
}

TEST(NetlinkTypes, NextHopBuilderReset) {
  NextHopBuilder nhBuilder;
  nhBuilder.setIfIndex(1)
      .setWeight(10)
      .setGateway(folly::IPAddress("face:cafe:3::3"))
      .setLabelAction(thrift::MplsActionCode::PHP)
      .setSwapLabel(20)
      .setPushLabels({30, 40});

  EXPECT_TRUE(nhBuilder.getIfIndex().has_value());
  EXPECT_TRUE(nhBuilder.getGateway().has_value());
  EXPECT_TRUE(nhBuilder.getLabelAction().has_value());
  EXPECT_TRUE(nhBuilder.getSwapLabel().has_value());
  EXPECT_TRUE(nhBuilder.getPushLabels().has_value());

  nhBuilder.reset();

  EXPECT_FALSE(nhBuilder.getIfIndex().has_value());
  EXPECT_FALSE(nhBuilder.getGateway().has_value());
  EXPECT_FALSE(nhBuilder.getLabelAction().has_value());
  EXPECT_FALSE(nhBuilder.getSwapLabel().has_value());
  EXPECT_FALSE(nhBuilder.getPushLabels().has_value());
}

TEST(NetlinkTypes, RouteMoveTest) {
  folly::CIDRNetwork dst{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::IPAddress gateway("face:cafe:3::3");
  uint32_t flags = 0x01;
  uint32_t priority = 3;
  uint8_t tos = 2;
  uint32_t mtu = 4;
  uint32_t advMss = 5;
  NextHopBuilder nhBuilder;
  auto nh1 = nhBuilder.setIfIndex(kIfIndex).setGateway(gateway).build();
  RouteBuilder builder;
  auto route = builder.setDestination(dst)
                   .setType(RTN_UNICAST)
                   .setProtocolId(kProtocolId)
                   .setScope(RT_SCOPE_UNIVERSE)
                   .setRouteTable(RT_TABLE_MAIN)
                   .setFlags(flags)
                   .setPriority(priority)
                   .setTos(tos)
                   .setMtu(mtu)
                   .setAdvMss(advMss)
                   .addNextHop(nh1)
                   .build();

  Route route1(std::move(route));
  EXPECT_EQ(AF_INET6, route1.getFamily());
  EXPECT_EQ(kProtocolId, route1.getProtocolId());
  EXPECT_EQ(RT_SCOPE_UNIVERSE, route1.getScope());
  EXPECT_EQ(RT_TABLE_MAIN, route1.getRouteTable());
  EXPECT_EQ(dst, route1.getDestination());
  EXPECT_EQ(RTN_UNICAST, route1.getType());
  EXPECT_TRUE(route1.getFlags().has_value());
  EXPECT_EQ(flags, route1.getFlags().value());
  EXPECT_TRUE(route1.getPriority().has_value());
  EXPECT_EQ(priority, route1.getPriority().value());
  EXPECT_TRUE(route1.getTos().has_value());
  EXPECT_EQ(tos, route1.getTos().value());
  EXPECT_TRUE(route1.getMtu().has_value());
  EXPECT_EQ(mtu, route1.getMtu().value());
  EXPECT_TRUE(route1.getAdvMss().has_value());
  EXPECT_EQ(advMss, route1.getAdvMss().value());
  EXPECT_EQ(1, route1.getNextHops().size());
  EXPECT_TRUE(route1.getNextHops().begin()->getGateway().has_value());
  EXPECT_EQ(gateway, route1.getNextHops().begin()->getGateway().value());

  Route route2 = std::move(route1);
  EXPECT_EQ(AF_INET6, route2.getFamily());
  EXPECT_EQ(kProtocolId, route2.getProtocolId());
  EXPECT_EQ(RT_SCOPE_UNIVERSE, route2.getScope());
  EXPECT_EQ(RT_TABLE_MAIN, route2.getRouteTable());
  EXPECT_EQ(dst, route2.getDestination());
  EXPECT_EQ(RTN_UNICAST, route2.getType());
  EXPECT_TRUE(route2.getFlags().has_value());
  EXPECT_EQ(flags, route2.getFlags().value());
  EXPECT_TRUE(route2.getPriority().has_value());
  EXPECT_EQ(priority, route2.getPriority().value());
  EXPECT_TRUE(route2.getTos().has_value());
  EXPECT_EQ(tos, route2.getTos().value());
  EXPECT_TRUE(route2.getMtu().has_value());
  EXPECT_EQ(mtu, route2.getMtu().value());
  EXPECT_TRUE(route2.getAdvMss().has_value());
  EXPECT_EQ(advMss, route2.getAdvMss().value());
  EXPECT_EQ(1, route2.getNextHops().size());
}

TEST(NetlinkTypes, RouteCopyTest) {
  folly::CIDRNetwork dst{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::IPAddress gateway("face:cafe:3::3");
  uint32_t flags = 0x01;
  uint32_t priority = 3;
  uint8_t tos = 2;
  uint32_t mtu = 4;
  uint32_t advMss = 5;
  NextHopBuilder nhBuilder;
  auto nh1 = nhBuilder.setIfIndex(kIfIndex).setGateway(gateway).build();
  RouteBuilder builder;
  auto route = builder.setDestination(dst)
                   .setType(RTN_UNICAST)
                   .setProtocolId(kProtocolId)
                   .setScope(RT_SCOPE_UNIVERSE)
                   .setRouteTable(RT_TABLE_MAIN)
                   .setFlags(flags)
                   .setPriority(priority)
                   .setTos(tos)
                   .setMtu(mtu)
                   .setAdvMss(advMss)
                   .addNextHop(nh1)
                   .build();

  // Copy constructor
  fbnl::Route route2(route);
  EXPECT_EQ(route, route2);

  // Copy assignment operator
  route2 = route;
  EXPECT_EQ(route, route2);
}

TEST(NetlinkTypes, RouteOptionalParamTest) {
  folly::CIDRNetwork dst{folly::IPAddress("fc00:cafe:3::3"), 128};
  uint32_t flags = 0x01;
  uint32_t priority = 3;
  uint8_t tos = 2;
  uint32_t mtu = 4;
  uint32_t advMss = 5;
  folly::IPAddress gateway("face:cafe:3::3");
  NextHopBuilder builder;
  auto nh1 = builder.setIfIndex(kIfIndex).build();
  builder.reset();
  auto nh2 = builder.setGateway(gateway).build();
  builder.reset();
  auto nh3 = builder.setIfIndex(kIfIndex).setGateway(gateway).build();
  RouteBuilder rtbuilder;
  auto route = rtbuilder.setDestination(dst)
                   .setType(RTN_UNICAST)
                   .setProtocolId(kProtocolId)
                   .setScope(RT_SCOPE_UNIVERSE)
                   .setRouteTable(RT_TABLE_MAIN)
                   .setFlags(flags)
                   .setPriority(priority)
                   .setTos(tos)
                   .setMtu(mtu)
                   .setAdvMss(advMss)
                   .addNextHop(nh1)
                   .addNextHop(nh2)
                   .addNextHop(nh3)
                   .build();

  EXPECT_EQ(AF_INET6, route.getFamily());
  EXPECT_EQ(kProtocolId, route.getProtocolId());
  EXPECT_EQ(RT_SCOPE_UNIVERSE, route.getScope());
  EXPECT_EQ(RT_TABLE_MAIN, route.getRouteTable());
  EXPECT_EQ(dst, route.getDestination());
  EXPECT_EQ(RTN_UNICAST, route.getType());
  EXPECT_TRUE(route.getFlags().has_value());
  EXPECT_EQ(flags, route.getFlags().value());
  EXPECT_TRUE(route.getPriority().has_value());
  EXPECT_EQ(priority, route.getPriority().value());
  EXPECT_TRUE(route.getTos().has_value());
  EXPECT_EQ(tos, route.getTos().value());
  EXPECT_TRUE(route.getMtu().has_value());
  EXPECT_EQ(mtu, route.getMtu().value());
  EXPECT_TRUE(route.getAdvMss().has_value());
  EXPECT_EQ(advMss, route.getAdvMss().value());
  EXPECT_EQ(3, route.getNextHops().size());
  EXPECT_EQ(dst, route.getDestination());
  EXPECT_EQ(RTN_UNICAST, route.getType());
}

TEST(NetlinkTypes, IfAddressMoveTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  uint32_t flags = 0x01;
  IfAddressBuilder builder;
  auto ifAddr =
      builder.setPrefix(prefix).setIfIndex(kIfIndex).setFlags(flags).build();
  IfAddress ifAddr1(std::move(ifAddr));

  EXPECT_EQ(AF_INET6, ifAddr1.getFamily());
  EXPECT_EQ(prefix.second, ifAddr1.getPrefixLen());
  EXPECT_FALSE(ifAddr1.getScope().has_value());
  EXPECT_TRUE(ifAddr1.getFlags().has_value());
  EXPECT_EQ(flags, ifAddr1.getFlags().value());
  EXPECT_EQ(kIfIndex, ifAddr1.getIfIndex());

  IfAddress ifAddr2 = std::move(ifAddr1);
  EXPECT_EQ(AF_INET6, ifAddr2.getFamily());
  EXPECT_EQ(prefix.second, ifAddr2.getPrefixLen());
  EXPECT_FALSE(ifAddr2.getScope().has_value());
  EXPECT_TRUE(ifAddr2.getFlags().has_value());
  EXPECT_EQ(flags, ifAddr2.getFlags().value());
  EXPECT_EQ(kIfIndex, ifAddr2.getIfIndex());
}

TEST(NetlinkTypes, IfAddressCopyTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  uint32_t flags = 0x01;
  IfAddressBuilder builder;
  auto ifAddr =
      builder.setPrefix(prefix).setIfIndex(kIfIndex).setFlags(flags).build();

  // Copy constructor
  fbnl::IfAddress ifAddr2(ifAddr);
  EXPECT_EQ(ifAddr, ifAddr2);

  // Copy assignment operator
  ifAddr2 = ifAddr;
  EXPECT_EQ(ifAddr, ifAddr2);
}

TEST(NetlinkTypes, IfAddressTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  uint32_t flags = 0x01;
  IfAddressBuilder builder;
  auto ifAddr =
      builder.setPrefix(prefix).setIfIndex(kIfIndex).setFlags(flags).build();
  LOG(INFO) << ifAddr.str();

  EXPECT_EQ(AF_INET6, ifAddr.getFamily());
  EXPECT_EQ(prefix.second, ifAddr.getPrefixLen());
  EXPECT_FALSE(ifAddr.getScope().has_value());
  EXPECT_TRUE(ifAddr.getFlags().has_value());
  EXPECT_EQ(flags, ifAddr.getFlags().value());
  EXPECT_EQ(kIfIndex, ifAddr.getIfIndex());

  folly::CIDRNetwork prefixV4{folly::IPAddress("192.168.0.11"), 32};
  builder.reset();
  auto ifAddr1 = builder.setPrefix(prefixV4)
                     .setFlags(flags)
                     .setScope(RT_SCOPE_SITE)
                     .setIfIndex(kIfIndex)
                     .build();

  EXPECT_EQ(AF_INET, ifAddr1.getFamily());
  EXPECT_EQ(prefixV4.second, ifAddr1.getPrefixLen());
  EXPECT_TRUE(ifAddr1.getScope().has_value());
  EXPECT_EQ(RT_SCOPE_SITE, ifAddr1.getScope().value());
  EXPECT_TRUE(ifAddr1.getFlags().has_value());
  EXPECT_EQ(flags, ifAddr1.getFlags().value());
  EXPECT_EQ(kIfIndex, ifAddr1.getIfIndex());
}

TEST(NetlinkTypes, IfAddressMiscTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  uint32_t flags = 0x01;
  IfAddressBuilder builder;
  auto ifAddr = builder.setPrefix(prefix)
                    .setIfIndex(kIfIndex)
                    .setFlags(flags)
                    .setFamily(AF_INET6) // will be shadowed
                    .build();
  EXPECT_EQ(AF_INET6, ifAddr.getFamily());
  EXPECT_EQ(prefix.second, ifAddr.getPrefixLen());
  EXPECT_FALSE(ifAddr.getScope().has_value());
  EXPECT_TRUE(ifAddr.getFlags().has_value());
  EXPECT_EQ(flags, ifAddr.getFlags().value());
  EXPECT_EQ(kIfIndex, ifAddr.getIfIndex());

  builder.reset();
  auto ifAddr1 = builder.setFamily(AF_INET).setIfIndex(kIfIndex).build();
  EXPECT_EQ(AF_INET, ifAddr1.getFamily());
  EXPECT_EQ(kIfIndex, ifAddr1.getIfIndex());
}

TEST(NetlinkTypes, NeighborTypeTest) {
  folly::IPAddress dst("fc00:cafe:3::3");
  folly::MacAddress mac("00:00:00:00:00:00");
  NeighborBuilder builder;
  auto neigh = builder.setIfIndex(kIfIndex)
                   .setState(NUD_REACHABLE)
                   .setDestination(dst)
                   .setLinkAddress(mac)
                   .build();
  LOG(INFO) << neigh.str();

  EXPECT_EQ(dst, neigh.getDestination());
  EXPECT_EQ(mac, neigh.getLinkAddress());
  EXPECT_EQ(kIfIndex, neigh.getIfIndex());
  EXPECT_EQ(AF_INET6, neigh.getFamily());
}

TEST(NetlinkTypes, NeighborMoveTest) {
  folly::IPAddress dst("fc00:cafe:3::3");
  folly::MacAddress mac("00:00:00:00:00:00");
  NeighborBuilder builder;
  auto neigh = builder.setIfIndex(kIfIndex)
                   .setState(NUD_REACHABLE)
                   .setDestination(dst)
                   .setLinkAddress(mac)
                   .build();

  // Move constructor
  fbnl::Neighbor neigh2(std::move(neigh));

  // Verify expectations
  EXPECT_EQ(kIfIndex, neigh2.getIfIndex());
  EXPECT_EQ(dst, neigh2.getDestination());
  EXPECT_EQ(mac, neigh2.getLinkAddress());
  EXPECT_TRUE(neigh2.isReachable());
}

TEST(NetlinkTypes, NeighborCopyTest) {
  folly::IPAddress dst("fc00:cafe:3::3");
  folly::MacAddress mac("00:00:00:00:00:00");
  NeighborBuilder builder;
  auto neigh = builder.setIfIndex(kIfIndex)
                   .setState(NUD_REACHABLE)
                   .setDestination(dst)
                   .setLinkAddress(mac)
                   .build();

  // Copy constructor
  fbnl::Neighbor neigh2(neigh);
  EXPECT_EQ(neigh, neigh2);

  // Copy assignment operator
  neigh2 = neigh;
  EXPECT_EQ(neigh, neigh2);
}

TEST(NetlinkTypes, LinkTypeTest) {
  std::string linkName("iface");
  unsigned int flags = 0x0 | IFF_RUNNING;

  LinkBuilder builder;
  auto link = builder.setIfIndex(kIfIndex)
                  .setFlags(flags)
                  .setLinkName(linkName)
                  .build();
  LOG(INFO) << link.str();

  EXPECT_EQ(kIfIndex, link.getIfIndex());
  EXPECT_EQ(linkName, link.getLinkName());
  EXPECT_EQ(flags, link.getFlags());
  EXPECT_TRUE(link.isUp());
}

TEST(NetlinkTypes, LinkMoveTest) {
  std::string linkName("iface");
  unsigned int flags = 0x0 | IFF_RUNNING;

  LinkBuilder builder;
  auto link = builder.setIfIndex(kIfIndex)
                  .setFlags(flags)
                  .setLinkName(linkName)
                  .build();

  // Move constructor
  fbnl::Link link2(std::move(link));

  // Verify expectations from new link object
  EXPECT_EQ(linkName, link2.getLinkName());
  EXPECT_EQ(kIfIndex, link2.getIfIndex());
  EXPECT_EQ(flags, link.getFlags());
  EXPECT_TRUE(link.isUp());
}

TEST(NetlinkTypes, LinkCopyTest) {
  std::string linkName("iface");
  unsigned int flags = 0x0 | IFF_RUNNING;

  LinkBuilder builder;
  auto link = builder.setIfIndex(kIfIndex)
                  .setFlags(flags)
                  .setLinkName(linkName)
                  .build();

  // Copy constructor
  fbnl::Link link2(link);
  EXPECT_EQ(link, link2);

  // Copy assignment operator
  link2 = link;
  EXPECT_EQ(link, link2);
}

TEST(NetlinkTypes, GreInfoBaseTest) {
  const auto ip1 = folly::IPAddress("1.2.3.4");
  const auto ip2 = folly::IPAddress("5.6.7.8");
  const auto ttl = 64;
  GreInfo greInfo(ip1, ip2, ttl);
  EXPECT_EQ(ip1, greInfo.getLocalAddr());
  EXPECT_EQ(ip2, greInfo.getRemoteAddr());
  EXPECT_EQ(ttl, greInfo.getTtl());
}

TEST(NetlinkTypes, GreInfoMoveTest) {
  const auto ip1 = folly::IPAddress("1.2.3.4");
  const auto ip2 = folly::IPAddress("5.6.7.8");
  const auto ttl = 64;
  GreInfo greInfo(ip1, ip2, ttl);

  // Move constructor
  fbnl::GreInfo greInfo2(std::move(greInfo));

  EXPECT_EQ(ip1, greInfo2.getLocalAddr());
  EXPECT_EQ(ip2, greInfo2.getRemoteAddr());
  EXPECT_EQ(ttl, greInfo2.getTtl());
}

TEST(NetlinkTypes, GreInfoCopyTest) {
  const auto ip1 = folly::IPAddress("1.2.3.4");
  const auto ip2 = folly::IPAddress("5.6.7.8");
  const auto ttl = 64;
  GreInfo greInfo(ip1, ip2, ttl);

  // Copy constructor
  fbnl::GreInfo greInfo2(greInfo);
  EXPECT_EQ(greInfo, greInfo2);

  // Copy assignment operator
  greInfo2 = greInfo;
  EXPECT_EQ(greInfo, greInfo2);
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
