#include <openr/nl/NetlinkTypes.h>
#include <openr/nl/NetlinkException.h>

#include <gtest/gtest.h>
#include <glog/logging.h>

extern "C" {
#include <netlink/cache.h>
#include <netlink/netlink.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>
#include <netlink/route/link/veth.h>
#include <netlink/route/route.h>
#include <netlink/socket.h>
#include <linux/if.h>
#include <sys/ioctl.h>
}

using namespace openr;
using namespace openr::fbnl;

const uint8_t kProtocolId = 99;
const int kIfIndex = 1;
const uint8_t kWeight = 4;

class NetlinkTypesFixture : public testing::Test {
 public:
  NetlinkTypesFixture() = default;
  ~NetlinkTypesFixture() override = default;

  void SetUp() override {
  }

  void TearDown() override {
  }

};

TEST_F(NetlinkTypesFixture, NextHopIfIndexConsTest) {
  // Create NextHop with ifindex
  NextHopBuilder builder;
  auto nh = builder.setIfIndex(kIfIndex).build();
  EXPECT_TRUE(nh.getIfIndex().hasValue());
  EXPECT_EQ(kIfIndex, nh.getIfIndex().value());
  EXPECT_FALSE(nh.getGateway().hasValue());
  EXPECT_FALSE(nh.getWeight().hasValue());
  struct rtnl_nexthop* object = nh.fromNetlinkNextHop();
  EXPECT_TRUE(object != nullptr);
  EXPECT_EQ(kIfIndex, rtnl_route_nh_get_ifindex(object));
  EXPECT_EQ(0, rtnl_route_nh_get_weight(object)); // weight's default value

  struct nl_addr* gw = rtnl_route_nh_get_gateway(object);
  EXPECT_TRUE(gw == nullptr);

  // Get multiple times
  struct rtnl_nexthop* object1 = nh.fromNetlinkNextHop();
  EXPECT_EQ(object, object1);

  NextHopBuilder newBuilder;
  auto nhFromObj = newBuilder.buildFromObject(object);
  EXPECT_EQ(nh, nhFromObj);

  // Free object
  rtnl_route_nh_free(object);
  nhFromObj.release();
}

TEST_F(NetlinkTypesFixture, NextHopGatewayConsTest) {
  // Create NextHop with gateway
  folly::IPAddress gateway("fc00:cafe:3::3");
  NextHopBuilder builder;
  auto nh = builder.setGateway(gateway)
                   .setWeight(kWeight)
                   .build();
  EXPECT_FALSE(nh.getIfIndex().hasValue());
  EXPECT_TRUE(nh.getGateway().hasValue());
  EXPECT_EQ(gateway, nh.getGateway().value());
  EXPECT_TRUE(nh.getWeight().hasValue());
  EXPECT_EQ(kWeight, nh.getWeight().value());

  struct rtnl_nexthop* object = nh.fromNetlinkNextHop();
  EXPECT_TRUE(object != nullptr);
  EXPECT_EQ(0, rtnl_route_nh_get_ifindex(object)); // ifIndex's default value
  EXPECT_EQ(kWeight, rtnl_route_nh_get_weight(object));
  struct nl_addr* nl_gw = nl_addr_build(
    gateway.family(), (void*)gateway.bytes(), gateway.byteCount());
  EXPECT_TRUE(nl_addr_cmp(nl_gw, rtnl_route_nh_get_gateway(object)) == 0);

  NextHopBuilder newBuilder;
  auto nhFromObj = newBuilder.buildFromObject(object);
  EXPECT_EQ(nh, nhFromObj);

  // Free object
  nl_addr_put(nl_gw);
  rtnl_route_nh_free(object);
  nhFromObj.release();
}

TEST_F(NetlinkTypesFixture, NexthopGeneralConsTest) {
  folly::IPAddress gateway("fc00:cafe:3::3");
  NextHopBuilder builder;
  auto nh = builder.setGateway(gateway)
                   .setIfIndex(kIfIndex)
                   .setWeight(kWeight)
                   .build();
  // Create nextHop with ifIndex and gateway
  EXPECT_TRUE(nh.getIfIndex().hasValue());
  EXPECT_EQ(kIfIndex, nh.getIfIndex().value());
  EXPECT_TRUE(nh.getGateway().hasValue());
  EXPECT_EQ(gateway, nh.getGateway().value());
  EXPECT_TRUE(nh.getWeight().hasValue());
  EXPECT_EQ(kWeight, nh.getWeight().value());

  struct rtnl_nexthop* object = nh.fromNetlinkNextHop();
  EXPECT_TRUE(object != nullptr);
  EXPECT_EQ(kIfIndex, rtnl_route_nh_get_ifindex(object));
  EXPECT_EQ(kWeight, rtnl_route_nh_get_weight(object));
  struct nl_addr* nl_gw = nl_addr_build(
    gateway.family(), (void*)gateway.bytes(), gateway.byteCount());
  EXPECT_TRUE(nl_addr_cmp(nl_gw, rtnl_route_nh_get_gateway(object)) == 0);
  EXPECT_TRUE(object == nh.fromNetlinkNextHop());

  NextHopBuilder newBuilder;
  auto nhFromObj = newBuilder.buildFromObject(object);
  EXPECT_EQ(nh, nhFromObj);

  // Free object
  nl_addr_put(nl_gw);
  rtnl_route_nh_free(object);
  nhFromObj.release();
}

TEST_F(NetlinkTypesFixture, RouteBaseTest) {
  folly::CIDRNetwork dst{folly::IPAddress("fc00:cafe:3::3"), 128};
  RouteBuilder builder;
  // Use default values
  auto route = builder.setDestination(dst)
         .setProtocolId(kProtocolId)
         .buildRoute();
  EXPECT_EQ(AF_INET6, route.getFamily());
  EXPECT_EQ(kProtocolId, route.getProtocolId());
  EXPECT_EQ(RT_SCOPE_UNIVERSE, route.getScope());
  EXPECT_EQ(RT_TABLE_MAIN, route.getRouteTable());
  EXPECT_EQ(dst, route.getDestination());
  EXPECT_EQ(RTN_UNICAST, route.getType());
  EXPECT_FALSE(route.getFlags().hasValue());
  EXPECT_TRUE(route.getNextHops().empty());
  EXPECT_FALSE(route.getPriority().hasValue());
  EXPECT_FALSE(route.getTos().hasValue());

  struct rtnl_route* object = route.fromNetlinkRoute();
  EXPECT_TRUE(object != nullptr);

  EXPECT_EQ(AF_INET6, rtnl_route_get_family(object));
  EXPECT_EQ(kProtocolId, rtnl_route_get_protocol(object));
  EXPECT_EQ(RT_SCOPE_UNIVERSE, rtnl_route_get_scope(object));
  EXPECT_EQ(RT_TABLE_MAIN, rtnl_route_get_table(object));
  EXPECT_EQ(dst, route.getDestination());
  EXPECT_EQ(RTN_UNICAST, route.getType());
  struct nl_addr* dstObj = nl_addr_build(
    dst.first.family(), (void*)dst.first.bytes(), dst.first.byteCount());
    nl_addr_set_prefixlen(dstObj, dst.second);
  EXPECT_TRUE(nl_addr_cmp(dstObj, rtnl_route_get_dst(object)) == 0);
  EXPECT_EQ(0, rtnl_route_get_flags(object));
  EXPECT_EQ(0, rtnl_route_get_priority(object));
  EXPECT_EQ(0, rtnl_route_get_tos(object));
  EXPECT_EQ(0, rtnl_route_get_nnexthops(object));

  struct rtnl_route* object1 = route.fromNetlinkRoute();
  EXPECT_EQ(object, object1);
  // Route will release rtnl_route object
  nl_addr_put(dstObj);
}

TEST_F(NetlinkTypesFixture, RouteEqualTest) {
  folly::CIDRNetwork dst{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::IPAddress gateway1("face:cafe:3::3");
  folly::IPAddress gateway2("face:cafe:3::4");
  folly::IPAddress gateway3("face:cafe:3::5");
  uint32_t flags = 0x01;
  uint32_t priority = 3;
  uint8_t tos = 2;
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
                      .addNextHop(nh1)
                      .addNextHop(nh2)
                      .buildRoute();
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
                      .addNextHop(nh2)
                      .addNextHop(nh1)
                      .buildRoute();
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
                      .addNextHop(nh1)
                      .buildRoute();
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
                      .addNextHop(nh1)
                      .addNextHop(nh3)
                      .buildRoute();
  EXPECT_FALSE(route2 == route4);

  // Add same nexthop
  nhBuilder.reset();
  auto nh4 =
    nhBuilder.setIfIndex(kIfIndex).setGateway(gateway1).build();
  nhBuilder.reset();
  auto nh5 =
    nhBuilder.setIfIndex(kIfIndex).setGateway(gateway1).build();
  RouteBuilder builder5;
  auto route5 = builder5.setDestination(dst)
                        .addNextHop(nh4)
                        .addNextHop(nh5)
                        .buildRoute();
  EXPECT_EQ(1, route5.getNextHops().size());
  nh5.release();
}

TEST_F(NetlinkTypesFixture, RouteMoveConsTest) {
  folly::CIDRNetwork dst{folly::IPAddress("fc00:cafe:3::3"), 128};
  folly::IPAddress gateway("face:cafe:3::3");
  uint32_t flags = 0x01;
  uint32_t priority = 3;
  uint8_t tos = 2;
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
                      .addNextHop(nh1)
                      .buildRoute();

  struct rtnl_route* p = route.fromNetlinkRoute();
  Route route1(std::move(route));
  EXPECT_TRUE(nullptr == route.fromNetlinkRoute());
  struct rtnl_route* p1 = route1.fromNetlinkRoute();
  EXPECT_EQ(p, p1);
  EXPECT_EQ(AF_INET6, route1.getFamily());
  EXPECT_EQ(AF_INET6, rtnl_route_get_family(p1));
  EXPECT_EQ(kProtocolId, route1.getProtocolId());
  EXPECT_EQ(kProtocolId, rtnl_route_get_protocol(p1));
  EXPECT_EQ(RT_SCOPE_UNIVERSE, route1.getScope());
  EXPECT_EQ(RT_SCOPE_UNIVERSE, rtnl_route_get_scope(p1));
  EXPECT_EQ(RT_TABLE_MAIN, route1.getRouteTable());
  EXPECT_EQ(RT_TABLE_MAIN, rtnl_route_get_table(p1));
  EXPECT_EQ(dst, route1.getDestination());
  EXPECT_EQ(RTN_UNICAST, route1.getType());
  EXPECT_EQ(RTN_UNICAST, rtnl_route_get_type(p1));
  EXPECT_TRUE(route1.getFlags().hasValue());
  EXPECT_EQ(flags, route1.getFlags().value());
  EXPECT_EQ(flags, rtnl_route_get_flags(p1));
  EXPECT_TRUE(route1.getPriority().hasValue());
  EXPECT_EQ(priority, route1.getPriority().value());
  EXPECT_EQ(priority, rtnl_route_get_priority(p1));
  EXPECT_TRUE(route1.getTos().hasValue());
  EXPECT_EQ(tos, route1.getTos().value());
  EXPECT_EQ(tos, rtnl_route_get_tos(p1));
  EXPECT_EQ(1, route1.getNextHops().size());
  EXPECT_TRUE(route1.getNextHops().begin()->getGateway().hasValue());
  EXPECT_EQ(gateway, route1.getNextHops().begin()->getGateway().value());

  Route route2 = std::move(route1);
  EXPECT_TRUE(nullptr == route1.fromNetlinkRoute());
  struct rtnl_route* p2 = route2.fromNetlinkRoute();
  EXPECT_EQ(p, p2);
  EXPECT_EQ(AF_INET6, route2.getFamily());
  EXPECT_EQ(AF_INET6, rtnl_route_get_family(p2));
  EXPECT_EQ(kProtocolId, route2.getProtocolId());
  EXPECT_EQ(kProtocolId, rtnl_route_get_protocol(p2));
  EXPECT_EQ(RT_SCOPE_UNIVERSE, route2.getScope());
  EXPECT_EQ(RT_SCOPE_UNIVERSE, rtnl_route_get_scope(p2));
  EXPECT_EQ(RT_TABLE_MAIN, route2.getRouteTable());
  EXPECT_EQ(RT_TABLE_MAIN, rtnl_route_get_table(p2));
  EXPECT_EQ(dst, route2.getDestination());
  EXPECT_EQ(RTN_UNICAST, route2.getType());
  EXPECT_EQ(RTN_UNICAST, rtnl_route_get_type(p2));
  EXPECT_TRUE(route2.getFlags().hasValue());
  EXPECT_EQ(flags, route2.getFlags().value());
  EXPECT_EQ(flags, rtnl_route_get_flags(p2));
  EXPECT_TRUE(route2.getPriority().hasValue());
  EXPECT_EQ(priority, route2.getPriority().value());
  EXPECT_EQ(priority, rtnl_route_get_priority(p2));
  EXPECT_TRUE(route2.getTos().hasValue());
  EXPECT_EQ(tos, route2.getTos().value());
  EXPECT_EQ(tos, rtnl_route_get_tos(p2));
  EXPECT_EQ(1, route2.getNextHops().size());
}

TEST_F(NetlinkTypesFixture, RouteOptionalParamTest) {

  folly::CIDRNetwork dst{folly::IPAddress("fc00:cafe:3::3"), 128};
  uint32_t flags = 0x01;
  uint32_t priority = 3;
  uint8_t tos = 2;
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
                        .addNextHop(nh1)
                        .addNextHop(nh2)
                        .addNextHop(nh3)
                        .buildRoute();

  EXPECT_EQ(AF_INET6, route.getFamily());
  EXPECT_EQ(kProtocolId, route.getProtocolId());
  EXPECT_EQ(RT_SCOPE_UNIVERSE, route.getScope());
  EXPECT_EQ(RT_TABLE_MAIN, route.getRouteTable());
  EXPECT_EQ(dst, route.getDestination());
  EXPECT_EQ(RTN_UNICAST, route.getType());
  EXPECT_TRUE(route.getFlags().hasValue());
  EXPECT_EQ(flags, route.getFlags().value());
  EXPECT_TRUE(route.getPriority().hasValue());
  EXPECT_EQ(priority, route.getPriority().value());
  EXPECT_TRUE(route.getTos().hasValue());
  EXPECT_EQ(tos, route.getTos().value());
  EXPECT_EQ(3, route.getNextHops().size());

  struct rtnl_route* object = route.fromNetlinkRoute();
  EXPECT_TRUE(object != nullptr);

  EXPECT_EQ(AF_INET6, rtnl_route_get_family(object));
  EXPECT_EQ(kProtocolId, rtnl_route_get_protocol(object));
  EXPECT_EQ(RT_SCOPE_UNIVERSE, rtnl_route_get_scope(object));
  EXPECT_EQ(RT_TABLE_MAIN, rtnl_route_get_table(object));
  EXPECT_EQ(dst, route.getDestination());
  EXPECT_EQ(RTN_UNICAST, route.getType());
  struct nl_addr* dstObj = nl_addr_build(
    dst.first.family(), (void*)dst.first.bytes(), dst.first.byteCount());
    nl_addr_set_prefixlen(dstObj, dst.second);
  EXPECT_TRUE(nl_addr_cmp(dstObj, rtnl_route_get_dst(object)) == 0);
  EXPECT_EQ(flags, rtnl_route_get_flags(object));
  EXPECT_EQ(priority, rtnl_route_get_priority(object));
  EXPECT_EQ(tos, rtnl_route_get_tos(object));
  EXPECT_EQ(3, rtnl_route_get_nnexthops(object));

  auto nextHopFunc = [](struct rtnl_nexthop * obj, void* gw) noexcept->void {
    struct rtnl_nexthop* nextHop = reinterpret_cast<struct rtnl_nexthop*>(obj);

    int ifIndex = rtnl_route_nh_get_ifindex(nextHop);
    if (ifIndex != 0) {
      EXPECT_EQ(kIfIndex, ifIndex);
    }
    struct nl_addr* gatewayObj = rtnl_route_nh_get_gateway(nextHop);
    if (gatewayObj) {
      folly::IPAddress* dest = reinterpret_cast<folly::IPAddress*>(gw);
      struct nl_addr* destObj = nl_addr_build(
        dest->family(), (void*)dest->bytes(), dest->byteCount());
      EXPECT_TRUE(nl_addr_cmp(destObj, gatewayObj) == 0);
      nl_addr_put(destObj);
    }
  };
  rtnl_route_foreach_nexthop(object, nextHopFunc, &gateway);

  // Only create once
  struct rtnl_route* object1 = route.fromNetlinkRoute();
  EXPECT_EQ(object, object1);
  nl_addr_put(dstObj);
}

TEST_F(NetlinkTypesFixture, IfAddressMoveConsTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  uint32_t flags = 0x01;
  IfAddressBuilder builder;
  auto ifAddr = builder.setPrefix(prefix)
                       .setIfIndex(kIfIndex)
                       .setFlags(flags)
                       .build();
  struct rtnl_addr* p = ifAddr.fromIfAddress();
  IfAddress ifAddr1(std::move(ifAddr));
  EXPECT_TRUE(nullptr == ifAddr.fromIfAddress());
  struct rtnl_addr* p1 = ifAddr1.fromIfAddress();
  EXPECT_EQ(p, p1);
  EXPECT_EQ(AF_INET6, rtnl_addr_get_family(p1));
  EXPECT_EQ(AF_INET6, ifAddr1.getFamily());
  EXPECT_EQ(prefix.second, rtnl_addr_get_prefixlen(p1));
  EXPECT_EQ(prefix.second, ifAddr1.getPrefixLen());
  EXPECT_EQ(RT_SCOPE_NOWHERE, rtnl_addr_get_scope(p1));
  EXPECT_FALSE(ifAddr1.getScope().hasValue());
  EXPECT_EQ(flags, rtnl_addr_get_flags(p1));
  EXPECT_TRUE(ifAddr1.getFlags().hasValue());
  EXPECT_EQ(flags, ifAddr1.getFlags().value());
  EXPECT_EQ(kIfIndex, rtnl_addr_get_ifindex(p1));
  EXPECT_EQ(kIfIndex, ifAddr1.getIfIndex());

  IfAddress ifAddr2 = std::move(ifAddr1);
  struct rtnl_addr* p2 = ifAddr2.fromIfAddress();
  EXPECT_TRUE(nullptr == ifAddr1.fromIfAddress());
  EXPECT_EQ(p, p2);
  EXPECT_EQ(AF_INET6, rtnl_addr_get_family(p2));
  EXPECT_EQ(AF_INET6, ifAddr2.getFamily());
  EXPECT_EQ(prefix.second, rtnl_addr_get_prefixlen(p2));
  EXPECT_EQ(prefix.second, ifAddr2.getPrefixLen());
  EXPECT_EQ(RT_SCOPE_NOWHERE, rtnl_addr_get_scope(p2));
  EXPECT_FALSE(ifAddr2.getScope().hasValue());
  EXPECT_EQ(flags, rtnl_addr_get_flags(p2));
  EXPECT_TRUE(ifAddr2.getFlags().hasValue());
  EXPECT_EQ(flags, ifAddr2.getFlags().value());
  EXPECT_EQ(kIfIndex, rtnl_addr_get_ifindex(p2));
  EXPECT_EQ(kIfIndex, ifAddr2.getIfIndex());
}

TEST_F(NetlinkTypesFixture, IfAddressTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  uint32_t flags = 0x01;
  IfAddressBuilder builder;
  auto ifAddr = builder.setPrefix(prefix)
                       .setIfIndex(kIfIndex)
                       .setFlags(flags)
                       .build();
  struct rtnl_addr* addr = ifAddr.fromIfAddress();
  EXPECT_TRUE(addr != nullptr);
  EXPECT_EQ(AF_INET6, rtnl_addr_get_family(addr));
  EXPECT_EQ(AF_INET6, ifAddr.getFamily());
  EXPECT_EQ(prefix.second, rtnl_addr_get_prefixlen(addr));
  EXPECT_EQ(prefix.second, ifAddr.getPrefixLen());
  EXPECT_EQ(RT_SCOPE_NOWHERE, rtnl_addr_get_scope(addr));
  EXPECT_FALSE(ifAddr.getScope().hasValue());
  EXPECT_EQ(flags, rtnl_addr_get_flags(addr));
  EXPECT_TRUE(ifAddr.getFlags().hasValue());
  EXPECT_EQ(flags, ifAddr.getFlags().value());
  EXPECT_EQ(kIfIndex, rtnl_addr_get_ifindex(addr));
  EXPECT_EQ(kIfIndex, ifAddr.getIfIndex());
  EXPECT_EQ(addr, ifAddr.fromIfAddress());

  folly::CIDRNetwork prefixV4{folly::IPAddress("192.168.0.11"), 32};
  builder.reset();
  auto ifAddr1 = builder.setPrefix(prefixV4)
                        .setFlags(flags)
                        .setScope(rtnl_str2scope("site"))
                        .setIfIndex(kIfIndex)
                        .build();
  addr = nullptr;
  addr = ifAddr1.fromIfAddress();
  EXPECT_TRUE(addr != nullptr);
  EXPECT_EQ(AF_INET, rtnl_addr_get_family(addr));
  EXPECT_EQ(AF_INET, ifAddr1.getFamily());
  EXPECT_EQ(prefixV4.second, rtnl_addr_get_prefixlen(addr));
  EXPECT_EQ(prefixV4.second, ifAddr1.getPrefixLen());
  EXPECT_EQ(rtnl_str2scope("site"), rtnl_addr_get_scope(addr));
  EXPECT_TRUE(ifAddr1.getScope().hasValue());
  EXPECT_EQ(rtnl_str2scope("site"), ifAddr1.getScope().value());
  EXPECT_EQ(flags, rtnl_addr_get_flags(addr));
  EXPECT_TRUE(ifAddr1.getFlags().hasValue());
  EXPECT_EQ(flags, ifAddr1.getFlags().value());
  EXPECT_EQ(kIfIndex, rtnl_addr_get_ifindex(addr));
  EXPECT_EQ(kIfIndex, ifAddr1.getIfIndex());
  EXPECT_EQ(addr, ifAddr1.fromIfAddress());
}

TEST_F(NetlinkTypesFixture, IfAddressMiscTest) {
  folly::CIDRNetwork prefix{folly::IPAddress("fc00:cafe:3::3"), 128};
  uint32_t flags = 0x01;
  IfAddressBuilder builder;
  auto ifAddr = builder.setPrefix(prefix)
                       .setIfIndex(kIfIndex)
                       .setFlags(flags)
                       .setFamily(AF_INET6) // will be shadowed
                       .build();
  struct rtnl_addr* addr = ifAddr.fromIfAddress();
  EXPECT_TRUE(addr != nullptr);
  EXPECT_EQ(AF_INET6, rtnl_addr_get_family(addr));
  EXPECT_EQ(AF_INET6, ifAddr.getFamily());
  EXPECT_EQ(prefix.second, rtnl_addr_get_prefixlen(addr));
  EXPECT_EQ(prefix.second, ifAddr.getPrefixLen());
  EXPECT_EQ(RT_SCOPE_NOWHERE, rtnl_addr_get_scope(addr));
  EXPECT_FALSE(ifAddr.getScope().hasValue());
  EXPECT_EQ(flags, rtnl_addr_get_flags(addr));
  EXPECT_TRUE(ifAddr.getFlags().hasValue());
  EXPECT_EQ(flags, ifAddr.getFlags().value());
  EXPECT_EQ(kIfIndex, rtnl_addr_get_ifindex(addr));
  EXPECT_EQ(kIfIndex, ifAddr.getIfIndex());
  EXPECT_EQ(addr, ifAddr.fromIfAddress());

  addr = nullptr;
  builder.reset();
  auto ifAddr1 = builder.setFamily(AF_INET).setIfIndex(kIfIndex).build();
  struct rtnl_addr* addr1 = ifAddr1.fromIfAddress();
  EXPECT_TRUE(addr1 != nullptr);
  EXPECT_EQ(AF_INET, rtnl_addr_get_family(addr1));
  EXPECT_EQ(AF_INET, ifAddr1.getFamily());
  EXPECT_EQ(kIfIndex, rtnl_addr_get_ifindex(addr1));
  EXPECT_EQ(kIfIndex, ifAddr1.getIfIndex());
}

int main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}

TEST_F(NetlinkTypesFixture, NeighborTypeTest) {
  folly::IPAddress dst("fc00:cafe:3::3");
  folly::MacAddress mac("00:00:00:00:00:00");
  NeighborBuilder builder;
  auto neigh = builder.setIfIndex(kIfIndex)
                      .setState(NUD_REACHABLE)
                      .setDestination(dst)
                      .setLinkAddress(mac)
                      .build();
  struct rtnl_neigh* obj = neigh.fromNeighbor();
  EXPECT_TRUE(obj != nullptr);
  struct nl_addr* dstObj = rtnl_neigh_get_dst(obj);
  EXPECT_TRUE(dstObj != nullptr);
  auto dstAddr = folly::IPAddress::fromBinary(folly::ByteRange(
      static_cast<const unsigned char*>(nl_addr_get_binary_addr(dstObj)),
      nl_addr_get_len(dstObj)));
  EXPECT_EQ(dst, dstAddr);
  EXPECT_EQ(dst, neigh.getDestination());

  struct nl_addr* macObj = rtnl_neigh_get_lladdr(obj);
  EXPECT_TRUE(macObj != nullptr);
  auto macAddr = folly::MacAddress::fromBinary(folly::ByteRange(
      static_cast<const unsigned char*>(nl_addr_get_binary_addr(macObj)),
      nl_addr_get_len(macObj)));
  EXPECT_EQ(mac, macAddr);
  EXPECT_EQ(mac, neigh.getLinkAddress());
  EXPECT_EQ(kIfIndex, rtnl_neigh_get_ifindex(obj));
  EXPECT_EQ(kIfIndex, neigh.getIfIndex());
  EXPECT_EQ(AF_INET6, neigh.getFamily());

  auto neigh1 = builder.buildFromObject(obj);
  EXPECT_TRUE(obj != neigh1.fromNeighbor());
  obj = neigh1.fromNeighbor();
  EXPECT_TRUE(obj != nullptr);
  dstObj = rtnl_neigh_get_dst(obj);
  EXPECT_TRUE(dstObj != nullptr);
  dstAddr = folly::IPAddress::fromBinary(folly::ByteRange(
      static_cast<const unsigned char*>(nl_addr_get_binary_addr(dstObj)),
      nl_addr_get_len(dstObj)));
  EXPECT_EQ(dst, dstAddr);
  EXPECT_EQ(dst, neigh1.getDestination());

  macObj = rtnl_neigh_get_lladdr(obj);
  EXPECT_TRUE(macObj != nullptr);
  macAddr = folly::MacAddress::fromBinary(folly::ByteRange(
      static_cast<const unsigned char*>(nl_addr_get_binary_addr(macObj)),
      nl_addr_get_len(macObj)));
  EXPECT_EQ(mac, macAddr);
  EXPECT_EQ(mac, neigh1.getLinkAddress());
  EXPECT_EQ(kIfIndex, rtnl_neigh_get_ifindex(obj));
  EXPECT_EQ(kIfIndex, neigh1.getIfIndex());
  EXPECT_EQ(AF_INET6, neigh1.getFamily());
}

TEST_F(NetlinkTypesFixture, LinkTypeTest) {
  std::string linkName("iface");
  unsigned int flags = 0x0 | IFF_RUNNING;

  LinkBuilder builder;
  auto link  = builder.setIfIndex(kIfIndex)
                      .setFlags(flags)
                      .setLinkName(linkName)
                      .build();
  struct rtnl_link* obj = link.fromLink();
  EXPECT_TRUE(obj != nullptr);
  EXPECT_EQ(kIfIndex, rtnl_link_get_ifindex(obj));
  EXPECT_EQ(kIfIndex, link.getIfIndex());
  EXPECT_EQ(linkName, std::string(rtnl_link_get_name(obj)));
  EXPECT_EQ(linkName, link.getLinkName());
  EXPECT_EQ(flags, rtnl_link_get_flags(obj));
  EXPECT_EQ(flags, link.getFlags());
  EXPECT_TRUE(link.isUp());

  auto link1 = builder.buildFromObject(obj);
  EXPECT_TRUE(obj != link1.fromLink());
  obj = link1.fromLink();
  EXPECT_TRUE(obj != nullptr);
  EXPECT_EQ(kIfIndex, rtnl_link_get_ifindex(obj));
  EXPECT_EQ(kIfIndex, link1.getIfIndex());
  EXPECT_EQ(linkName, std::string(rtnl_link_get_name(obj)));
  EXPECT_EQ(linkName, link1.getLinkName());
  EXPECT_EQ(flags, rtnl_link_get_flags(obj));
  EXPECT_EQ(flags, link1.getFlags());
  EXPECT_TRUE(link1.isUp());
}
