#include <openr/nl/NetlinkTypes.h>

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
  NetlinkNextHopBuilder builder;
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
  // Free object
  rtnl_route_nh_free(object);
}

TEST_F(NetlinkTypesFixture, NextHopGatewayConsTest) {
  // Create NextHop with gateway
  folly::IPAddress gateway("fc00:cafe:3::3");
  NetlinkNextHopBuilder builder;
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
  // Free object
  nl_addr_put(nl_gw);
  rtnl_route_nh_free(object);
}

TEST_F(NetlinkTypesFixture, NexthopGeneralConsTest) {
  folly::IPAddress gateway("fc00:cafe:3::3");
  NetlinkNextHopBuilder builder;
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
  // Free object
  nl_addr_put(nl_gw);
  rtnl_route_nh_free(object);
}

TEST_F(NetlinkTypesFixture, RouteBaseTest) {
  folly::CIDRNetwork dst{folly::IPAddress("fc00:cafe:3::3"), 128};
  RouteBuilder builder;
  // Use default values
  auto route = builder.setDestination(dst)
         .setProtocolId(kProtocolId)
         .build();
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

TEST_F(NetlinkTypesFixture, RouteOptionalParamTest) {

  folly::CIDRNetwork dst{folly::IPAddress("fc00:cafe:3::3"), 128};
  uint32_t flags = 0x01;
  uint32_t priority = 3;
  uint8_t tos = 2;
  folly::IPAddress gateway("face:cafe:3::3");
  NetlinkNextHopBuilder builder;
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
                        .build();

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

int main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
