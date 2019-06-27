/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <map>
#include <memory>
#include <string>
#include <thread>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Exception.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/MacAddress.h>
#include <folly/Subprocess.h>
#include <folly/gen/Base.h>
#include <folly/system/Shell.h>
#include <folly/test/TestUtils.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/nl/NetlinkMessage.h>
#include <openr/nl/NetlinkRoute.h>
#include <openr/nl/NetlinkTypes.h>

extern "C" {
#include <net/if.h>
#include <netlink/cache.h>
#include <netlink/netlink.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>
#include <netlink/route/link/veth.h>
#include <netlink/route/route.h>
#include <netlink/socket.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
}

using namespace openr;
using namespace openr::Netlink;
using namespace folly::literals::shell_literals;

namespace {
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");
const uint8_t kRouteProtoId = 99;
} // namespace

folly::CIDRNetwork ipPrefix1 = folly::IPAddress::createNetwork("5501::/64");
folly::CIDRNetwork ipPrefix2 = folly::IPAddress::createNetwork("5502::/64");
folly::CIDRNetwork ipPrefix3 = folly::IPAddress::createNetwork("5503::/64");
folly::CIDRNetwork ipPrefix4 = folly::IPAddress::createNetwork("5504::/64");
folly::CIDRNetwork ipPrefix5 = folly::IPAddress::createNetwork("5505::/64");

folly::IPAddress ipAddrX1V6{"fe80::101"};
folly::IPAddress ipAddrY1V6{"fe80::201"};
folly::IPAddress ipAddrY2V6{"fe80::202"};
folly::IPAddress ipAddrY3V6{"fe80::203"};
folly::IPAddress ipAddrY4V6{"fe80::204"};

folly::IPAddress ipAddrX1V4{"172.10.10.10"};
folly::IPAddress ipAddrY1V4{"172.10.11.10"};

std::vector<int32_t> outLabel1{500};
std::vector<int32_t> outLabel2{500, 501};
std::vector<int32_t> outLabel3{502, 503, 504};
std::vector<int32_t> outLabel4{505, 506, 507, 508};
std::vector<int32_t> outLabel5{509};
std::vector<int32_t> outLabel6{609, 610, 611, 612};
uint32_t swapLabel{500};

uint32_t inLabel1{110};
uint32_t inLabel2{120};
uint32_t inLabel3{130};
uint32_t inLabel4{140};
uint32_t inLabel5{141};

class NlMessageFixture : public ::testing::Test {
 public:
  NlMessageFixture() = default;
  ~NlMessageFixture() override = default;

  void
  SetUp() override {
    if (getuid()) {
      SKIP() << "Must run this test as root";
      return;
    }

    // cleanup old interfaces in any
    auto cmd = "ip link del {} 2>/dev/null"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    // Ignore result
    proc.wait();

    socket_ = nl_socket_alloc();
    ASSERT_TRUE(socket_);
    nl_connect(socket_, NETLINK_ROUTE);

    rtnl_link_alloc_cache(socket_, AF_UNSPEC, &linkCache_);
    ASSERT_TRUE(linkCache_);

    link_ = rtnl_link_veth_alloc();
    ASSERT_TRUE(link_);
    auto peerLink = rtnl_link_veth_get_peer(link_);
    ASSERT_TRUE(peerLink);
    rtnl_link_set_name(link_, kVethNameX.c_str());
    rtnl_link_set_name(peerLink, kVethNameY.c_str());
    nl_object_put(OBJ_CAST(peerLink));

    auto err = rtnl_link_add(socket_, link_, NLM_F_CREATE);
    ASSERT_EQ(0, err);

    nl_cache_refill(socket_, linkCache_);

    ifIndexX = rtnl_link_name2i(linkCache_, kVethNameX.c_str());
    ifIndexY = rtnl_link_name2i(linkCache_, kVethNameY.c_str());

    addAddress(kVethNameX, ipAddrX1V6.str());
    addAddress(kVethNameY, ipAddrY1V6.str());
    addAddress(kVethNameY, ipAddrY2V6.str());
    addAddress(kVethNameY, ipAddrY3V6.str());
    addAddress(kVethNameY, ipAddrY4V6.str());

    addAddress(kVethNameX, ipAddrX1V4.str());
    addAddress(kVethNameY, ipAddrY1V4.str());

    // set interface status to up
    bringUpIntf(kVethNameX);
    bringUpIntf(kVethNameY);

    // netlink protocol socket
    nlSock = std::make_unique<NetlinkProtocolSocket>(&evl);

    // start event thread
    eventThread = std::thread([&]() {
      nlSock->init();
      evl.run();
      evl.waitUntilStopped();
    });

    evl.waitUntilRunning();
  }

  void
  TearDown() override {
    if (getuid()) {
      // Nothing to cleanup if not-root
      return;
    }

    if (evl.isRunning()) {
      evl.stop();
      eventThread.join();
    }

    rtnl_link_delete(socket_, link_);
    nl_cache_free(linkCache_);
    nl_socket_free(socket_);
    rtnl_link_veth_release(link_);
  }

 private:
  static void
  bringUpIntf(const std::string& ifName) {
    // Prepare socket
    auto sockFd = socket(PF_INET, SOCK_DGRAM, 0);
    CHECK_LT(0, sockFd);

    // Prepare request
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    folly::strlcpy(ifr.ifr_name, ifName.c_str(), IFNAMSIZ);

    // Get existing flags
    int error = ioctl(sockFd, SIOCGIFFLAGS, static_cast<void*>(&ifr));
    CHECK_EQ(0, error);

    // Mutate flags and set them back
    ifr.ifr_flags |= IFF_UP;
    error = ioctl(sockFd, SIOCSIFFLAGS, static_cast<void*>(&ifr));
    CHECK_EQ(0, error);
  }

  void
  addAddress(const std::string& ifName, const std::string& address) {
    int ifIndex = rtnl_link_name2i(linkCache_, ifName.c_str());
    ASSERT_NE(0, ifIndex);

    auto addrMask = std::make_pair(folly::IPAddress(address), 16);
    struct nl_addr* nlAddr = nl_addr_build(
        addrMask.first.family(),
        (void*)addrMask.first.bytes(),
        addrMask.first.byteCount());
    ASSERT_TRUE(nlAddr);
    nl_addr_set_prefixlen(nlAddr, addrMask.second);

    struct rtnl_addr* addr = rtnl_addr_alloc();
    ASSERT_TRUE(addr);
    rtnl_addr_set_local(addr, nlAddr);
    rtnl_addr_set_ifindex(addr, ifIndex);
    int err = rtnl_addr_add(socket_, addr, 0);
    ASSERT_EQ(0, err);
    nl_addr_put(nlAddr);
    rtnl_addr_put(addr);
  }

  fbzmq::ZmqEventLoop evl;
  std::thread eventThread;

  struct nl_cache* linkCache_{nullptr};
  struct rtnl_link* link_{nullptr};
  struct nl_sock* socket_{nullptr};

 protected:
  openr::fbnl::NextHop
  buildNextHop(
      folly::Optional<std::vector<int32_t>> pushLabels,
      folly::Optional<uint32_t> swapLabel,
      folly::Optional<thrift::MplsActionCode> action,
      folly::Optional<folly::IPAddress> gateway,
      int ifIndex) {
    openr::fbnl::NextHopBuilder nhBuilder;

    if (pushLabels.hasValue()) {
      nhBuilder.setPushLabels(pushLabels.value());
    }
    if (swapLabel.hasValue()) {
      nhBuilder.setSwapLabel(swapLabel.value());
    }
    if (action.hasValue()) {
      nhBuilder.setLabelAction(action.value());
    }
    if (gateway.hasValue()) {
      nhBuilder.setGateway(gateway.value());
    }
    nhBuilder.setIfIndex(ifIndex);
    return nhBuilder.build();
  }

  openr::fbnl::Route
  buildRoute(
      int protocolId,
      const folly::Optional<folly::CIDRNetwork>& dest,
      folly::Optional<uint32_t> mplsLabel,
      const folly::Optional<std::vector<openr::fbnl::NextHop>>& nexthops) {
    fbnl::RouteBuilder rtBuilder;

    rtBuilder.setProtocolId(protocolId);
    if (dest.hasValue()) {
      rtBuilder.setDestination(dest.value());
    }
    if (mplsLabel.hasValue()) {
      rtBuilder.setMplsLabel(mplsLabel.value());
    }
    if (nexthops.hasValue()) {
      for (const auto& nh : nexthops.value()) {
        rtBuilder.addNextHop(nh);
      }
    }
    return rtBuilder.build();
  }

  std::vector<openr::fbnl::Route>
  buildV6RouteDb(int count) {
    std::vector<openr::fbnl::Route> routes;
    std::vector<openr::fbnl::NextHop> paths;
    paths.push_back(buildNextHop(
        outLabel4,
        folly::none,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V4,
        ifIndexY));
    paths.push_back(buildNextHop(
        outLabel5,
        folly::none,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V4,
        ifIndexY));
    paths.push_back(buildNextHop(
        outLabel6,
        folly::none,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V4,
        ifIndexY));

    struct v4Addr addr4 {};
    for (uint32_t i = 0; i < count; i++) {
      addr4.u32_addr = 0x000000A0 + i;
      folly::IPAddress ipAddress =
          folly::IPAddress::fromBinary(folly::ByteRange(
              static_cast<const unsigned char*>(&addr4.u8_addr[0]), 4));
      folly::CIDRNetwork prefix = std::make_pair(ipAddress, 30);
      routes.emplace_back(
          buildRoute(kRouteProtoId, prefix, folly::none, paths));
    }
    return routes;
  }

  std::vector<openr::fbnl::Route>
  buildV4RouteDb(int count) {
    std::vector<openr::fbnl::Route> routes;
    std::vector<openr::fbnl::NextHop> paths;
    // create mix of next hops, including without label
    paths.push_back(buildNextHop(
        outLabel1,
        folly::none,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V6,
        ifIndexZ));
    paths.push_back(buildNextHop(
        outLabel2,
        folly::none,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V6,
        ifIndexZ));
    paths.push_back(buildNextHop(
        folly::none, folly::none, folly::none, ipAddrY1V6, ifIndexZ));
    paths.push_back(buildNextHop(
        outLabel4,
        folly::none,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V6,
        ifIndexZ));
    struct v6Addr addr6 {
      0
    };
    for (uint32_t i = 0; i < count; i++) {
      addr6.u32_addr[0] = htonl(0x50210000 + i);
      folly::IPAddress ipAddress =
          folly::IPAddress::fromBinary(folly::ByteRange(
              static_cast<const unsigned char*>(&addr6.u8_addr[0]), 16));
      folly::CIDRNetwork prefix = std::make_pair(ipAddress, 64);
      routes.emplace_back(
          buildRoute(kRouteProtoId, prefix, folly::none, paths));
    }
    return routes;
  }

  // ifindex of vethTestX and vethTextY
  uint32_t ifIndexX{0};
  uint32_t ifIndexY{0};
  uint32_t ifIndexZ{2};
  uint32_t ifIndexLo{1};

  struct v6Addr {
    union {
      uint8_t u8_addr[16];
      uint32_t u32_addr[4];
    };
  };

  struct v4Addr {
    union {
      uint8_t u8_addr[4];
      uint32_t u32_addr;
    };
  };
  // netlink message socket
  std::unique_ptr<NetlinkProtocolSocket> nlSock{nullptr};
};

TEST_F(NlMessageFixture, EncodeLabel) {
  NetlinkRouteMessage rt{};
  std::vector<std::pair<uint32_t, uint32_t>> labels;
  labels.push_back(std::make_pair(0x0, 0 | 0x100));
  labels.push_back(std::make_pair(0x1, 0x1000 | 0x100));
  labels.push_back(std::make_pair(0xF, 0xF000 | 0x100));
  labels.push_back(std::make_pair(0xFF, 0xFF000 | 0x100));
  labels.push_back(std::make_pair(0xFFF, 0xFFF000 | 0x100));
  labels.push_back(std::make_pair(0xFFFF, 0xFFFF000 | 0x100));
  labels.push_back(std::make_pair(0x8000, 0x8000000 | 0x100));
  labels.push_back(std::make_pair(0x80000, 0x80000000 | 0x100));
  labels.push_back(std::make_pair(0x100000, 0x100));
  labels.push_back(std::make_pair(0x1F0000, 0x100));

  for (const auto& x : labels) {
    EXPECT_EQ(x.second, ntohl(rt.encodeLabel(x.first, true)));
  }
}

TEST_F(NlMessageFixture, IpRouteLabelNexthop) {
  // Add IPv6 route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      outLabel1,
      folly::none,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V6,
      ifIndexZ));
  auto route = buildRoute(kRouteProtoId, ipPrefix1, folly::none, paths);

  ackCount = nlSock->getAckCount();
  // create label next hop
  status = nlSock->addRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);

  ackCount = nlSock->getAckCount();
  status = nlSock->deleteRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);
}

TEST_F(NlMessageFixture, LabelRoutePHPNexthop) {
  // Add label route with single path label with PHP nexthop

  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::PHP,
      ipAddrY1V6,
      ifIndexZ));
  auto route = buildRoute(kRouteProtoId, folly::none, inLabel4, paths);

  ackCount = nlSock->getAckCount();
  status = nlSock->addLabelRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);

  ackCount = nlSock->getAckCount();
  status = nlSock->addLabelRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);

  paths.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::PHP,
      ipAddrY2V6,
      ifIndexZ));

  auto route2 = buildRoute(kRouteProtoId, folly::none, inLabel4, paths);

  ackCount = nlSock->getAckCount();
  status = nlSock->addLabelRoute(route2);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);

  ackCount = nlSock->getAckCount();
  status = nlSock->deleteLabelRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);
}

TEST_F(NlMessageFixture, IpRouteMultipleNextHops) {
  // Add IPv6 route with 48 path ECMP

  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  std::vector<openr::fbnl::NextHop> paths;
  for (uint32_t i = 0; i < 48; i++) {
    outLabel6[0] = outLabel6[0] + i;
    paths.push_back(buildNextHop(
        outLabel6,
        folly::none,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V6,
        ifIndexZ));
  }

  auto route = buildRoute(kRouteProtoId, ipPrefix5, folly::none, paths);
  ackCount = nlSock->getAckCount();
  // create label next hop
  status = nlSock->addRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);

  ackCount = nlSock->getAckCount();
  status = nlSock->deleteRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);
}

TEST_F(NlMessageFixture, MaxPayloadExceeded) {
  // check for max payload handling. Add nexthops that exceeds payload size
  // Should error out

  ResultCode status{ResultCode::FAIL};
  std::vector<openr::fbnl::NextHop> paths;
  struct v6Addr addr6 {
    0
  };
  for (uint32_t i = 0; i < 200; i++) {
    addr6.u32_addr[0] = htonl(0xfe800000 + i);
    folly::IPAddress ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
        static_cast<const unsigned char*>(&addr6.u8_addr[0]), 16));
    paths.push_back(buildNextHop(
        outLabel5,
        folly::none,
        thrift::MplsActionCode::PHP,
        ipAddress,
        ifIndexZ));
  }

  auto route = buildRoute(kRouteProtoId, folly::none, inLabel4, paths);
  status = nlSock->addLabelRoute(route);
  EXPECT_EQ(status, ResultCode::NO_MESSAGE_BUFFER);
}

TEST_F(NlMessageFixture, PopLabel) {
  // pop label to loopback i/f

  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::POP_AND_LOOKUP,
      folly::none,
      ifIndexLo));
  auto route = buildRoute(kRouteProtoId, folly::none, inLabel3, paths);
  ackCount = nlSock->getAckCount();
  // create label next hop
  status = nlSock->addLabelRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);

  ackCount = nlSock->getAckCount();
  status = nlSock->deleteLabelRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);
}

TEST_F(NlMessageFixture, LabelRouteLabelNexthop) {
  // Add label route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none,
      swapLabel,
      thrift::MplsActionCode::SWAP,
      ipAddrY1V6,
      ifIndexZ));
  auto route = buildRoute(kRouteProtoId, folly::none, inLabel3, paths);
  ackCount = nlSock->getAckCount();
  // create label next hop
  status = nlSock->addLabelRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);

  ackCount = nlSock->getAckCount();
  status = nlSock->deleteLabelRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);
}

TEST_F(NlMessageFixture, NlErrorMessage) {
  // Add label route with single path label next with one label
  // and an invalid outgoing I/F. Function should return Fail
  // and the nlmsg error should increase

  std::vector<openr::fbnl::NextHop> paths;
  uint32_t invalidIfindex = 1000;
  paths.push_back(buildNextHop(
      folly::none,
      swapLabel,
      thrift::MplsActionCode::SWAP,
      ipAddrY1V6,
      invalidIfindex));
  auto route = buildRoute(kRouteProtoId, folly::none, inLabel3, paths);
  EXPECT_EQ(0, nlSock->getErrorCount());
  // create label next hop
  ResultCode status = nlSock->addLabelRoute(route);
  EXPECT_EQ(status, ResultCode::SYSERR);
  EXPECT_NE(0, nlSock->getErrorCount());
}

TEST_F(NlMessageFixture, InvalidRoute) {
  // Add two routes, one valid and the other invalid. Only one should be
  // sent to program

  std::vector<openr::fbnl::NextHop> paths1;
  std::vector<openr::fbnl::Route> routes;
  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  paths1.push_back(buildNextHop(
      folly::none,
      swapLabel,
      thrift::MplsActionCode::SWAP,
      ipAddrY1V6,
      ifIndexZ));
  routes.emplace_back(
      buildRoute(kRouteProtoId, ipPrefix1, folly::none, paths1));

  std::vector<openr::fbnl::NextHop> paths2;
  // Invalid route, send PUSH without labels
  paths2.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V6,
      ifIndexZ));
  routes.emplace_back(
      buildRoute(kRouteProtoId, ipPrefix2, folly::none, paths2));

  ackCount = nlSock->getAckCount();
  EXPECT_EQ(0, nlSock->getErrorCount());
  // create label next hop
  nlSock->addRoutes(routes);
  EXPECT_EQ(0, nlSock->getErrorCount());
  // programmed 2 routes but should have received only 1 ack
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);

  // delete needs only the destination prefix or label, doesn't
  // matter if the nexthop is valid or not. In this case delete will
  // be called for both routes but only one route is installed. Kernel
  // will return an error
  ackCount = nlSock->getAckCount();
  status = nlSock->deleteRoute(routes[0]);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  status = nlSock->deleteRoute(routes[1]);
  EXPECT_EQ(1, nlSock->getErrorCount());
  // deleting 2 routes but should have received only 1 ack
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);
}

TEST_F(NlMessageFixture, MultipleIpRoutesLabelNexthop) {
  // Add IPv6 route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  uint32_t count{100000};
  const auto routes = buildV6RouteDb(count);

  ackCount = nlSock->getAckCount();
  LOG(INFO) << "Adding " << count << " routes";
  status = nlSock->addRoutes(routes);
  LOG(INFO) << "Done adding " << count << " routes";
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  // should have received acks with status = 0
  EXPECT_GE(nlSock->getAckCount(), ackCount + count);

  // delete routes
  ackCount = nlSock->getAckCount();
  status = nlSock->deleteRoutes(routes);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  // should have received acks status = 0
  EXPECT_GE(nlSock->getAckCount(), ackCount + count);
}

TEST_F(NlMessageFixture, LabelRouteV4Nexthop) {
  // Add label route with single path label with PHP nexthop

  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::PHP,
      ipAddrY1V4,
      ifIndexY));
  auto route = buildRoute(kRouteProtoId, folly::none, inLabel5, paths);

  ackCount = nlSock->getAckCount();
  status = nlSock->addLabelRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);

  ackCount = nlSock->getAckCount();
  status = nlSock->deleteLabelRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);
}

TEST_F(NlMessageFixture, IpV4RouteLabelNexthop) {
  // Add IPv4 route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  folly::CIDRNetwork ipPrefix1V4 =
      folly::IPAddress::createNetwork("10.10.0.0/24");
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      outLabel4,
      folly::none,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V4,
      ifIndexY));
  paths.push_back(buildNextHop(
      folly::none, folly::none, folly::none, ipAddrY1V4, ifIndexY));
  auto route = buildRoute(kRouteProtoId, ipPrefix1V4, folly::none, paths);

  ackCount = nlSock->getAckCount();
  // create ipv4 route with label nexthop
  status = nlSock->addRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  // should have received one ack with status = 0
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);

  ackCount = nlSock->getAckCount();
  status = nlSock->deleteRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);
}

TEST_F(NlMessageFixture, MaxLabelStackTest) {
  // Add IPv4 route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  folly::CIDRNetwork ipPrefix1V4 =
      folly::IPAddress::createNetwork("11.10.0.0/24");
  std::vector<openr::fbnl::NextHop> paths;
  std::vector<int32_t> labels(16);
  std::iota(std::begin(labels), std::end(labels), 701);
  paths.push_back(buildNextHop(
      labels, folly::none, thrift::MplsActionCode::PUSH, ipAddrY1V4, ifIndexY));
  paths.push_back(buildNextHop(
      folly::none, folly::none, folly::none, ipAddrY1V4, ifIndexY));
  auto route = buildRoute(kRouteProtoId, ipPrefix1V4, folly::none, paths);

  ackCount = nlSock->getAckCount();
  // create ipv4 route with label nexthop
  status = nlSock->addRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  // should have received one ack with status = 0
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);

  ackCount = nlSock->getAckCount();
  status = nlSock->deleteRoute(route);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + 1);
}

TEST_F(NlMessageFixture, MultipleIpV4RouteLabelNexthop) {
  // Add Multiple IPv4 routes with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  const uint32_t count{100000};
  const auto routes = buildV4RouteDb(count);

  ackCount = nlSock->getAckCount();
  LOG(INFO) << "Adding in bulk " << count << " routes";
  status = nlSock->addRoutes(routes);
  LOG(INFO) << "Done adding " << count << " routes";
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  // should have received acks with status = 0
  EXPECT_GE(nlSock->getAckCount(), ackCount + count);

  // delete routes
  LOG(INFO) << "Deleting in bulk " << count << " routes";
  ackCount = nlSock->getAckCount();
  status = nlSock->deleteRoutes(routes);
  LOG(INFO) << "Done deleting";
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  // should have received acks status = 0
  EXPECT_GE(nlSock->getAckCount(), ackCount + count);

  // add routes one by one instead of a vector of routes
  LOG(INFO) << "Adding one by one " << count << " routes";
  ackCount = nlSock->getAckCount();
  for (const auto& route : routes) {
    status = nlSock->addRoute(route);
    EXPECT_EQ(status, ResultCode::SUCCESS);
  }
  LOG(INFO) << "Done adding " << count << " routes";
  EXPECT_EQ(0, nlSock->getErrorCount());
  // should have received acks with status = 0
  EXPECT_GE(nlSock->getAckCount(), ackCount + count);

  // delete routes one by one instead of a vector of routes
  LOG(INFO) << "Deleting " << count << " routes one at a time";
  ackCount = nlSock->getAckCount();
  for (const auto& route : routes) {
    status = nlSock->deleteRoute(route);
    EXPECT_EQ(status, ResultCode::SUCCESS);
  }
  LOG(INFO) << "Done deleting";
  EXPECT_EQ(0, nlSock->getErrorCount());
  // should have received acks status = 0
  EXPECT_GE(nlSock->getAckCount(), ackCount + count);
}

TEST_F(NlMessageFixture, MultipleLabelRoutes) {
  // Add IPv6 route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  ResultCode status{ResultCode::FAIL};
  uint32_t count{20000};
  std::vector<openr::fbnl::NextHop> paths;
  // create label next hop
  paths.push_back(buildNextHop(
      outLabel1,
      folly::none,
      thrift::MplsActionCode::SWAP,
      ipAddrY1V6,
      ifIndexZ));
  std::vector<openr::fbnl::Route> labelRoutes;
  for (uint32_t i = 0; i < count; i++) {
    labelRoutes.push_back(
        buildRoute(kRouteProtoId, folly::none, 600 + i, paths));
  }

  ackCount = nlSock->getAckCount();
  LOG(INFO) << "Adding " << count << " label routes";
  status = nlSock->addRoutes(labelRoutes);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  LOG(INFO) << "Done adding " << count << " label routes";
  EXPECT_GE(nlSock->getAckCount(), ackCount + count);
  EXPECT_EQ(0, nlSock->getErrorCount());

  ackCount = nlSock->getAckCount();
  status = nlSock->deleteRoutes(labelRoutes);
  EXPECT_EQ(status, ResultCode::SUCCESS);
  EXPECT_EQ(0, nlSock->getErrorCount());
  EXPECT_GE(nlSock->getAckCount(), ackCount + count);
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
