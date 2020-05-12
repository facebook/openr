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

#include <fb303/ServiceData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Exception.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/MacAddress.h>
#include <folly/Subprocess.h>
#include <folly/gen/Base.h>
#include <folly/io/async/EventBase.h>
#include <folly/system/Shell.h>
#include <folly/test/TestUtils.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/nl/NetlinkProtocolSocket.h>

extern "C" {
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
}

DEFINE_bool(
    enable_ipv6_rr_semantics, false, "Enable ipv6 route replace semantics");

using namespace openr;
using namespace folly::literals::shell_literals;

using openr::fbnl::NetlinkProtocolSocket;
using openr::fbnl::NetlinkRouteMessage;

namespace {
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");
const uint8_t kRouteProtoId = 99;
const uint32_t kAqRouteProtoIdPriority = 10;
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
folly::IPAddress ipAddrY2V4{"172.10.11.11"};
const folly::MacAddress kLinkAddr1("01:02:03:04:05:06");
const folly::MacAddress kLinkAddr2("01:02:03:04:05:07");

std::vector<int32_t> outLabel1{500};
std::vector<int32_t> outLabel2{500, 501};
std::vector<int32_t> outLabel3{502, 503, 504};
std::vector<int32_t> outLabel4{505, 506, 507, 508};
std::vector<int32_t> outLabel5{509};
std::vector<int32_t> outLabel6{609, 610, 611, 612};
uint32_t swapLabel{500};
uint32_t swapLabel1{501};

uint32_t inLabel1{110};
uint32_t inLabel2{120};
uint32_t inLabel3{130};
uint32_t inLabel4{140};
uint32_t inLabel5{141};

int64_t
getErrorCount() {
  return facebook::fb303::fbData->getCounters()["netlink.requests.error.sum"];
}

int64_t
getAckCount() {
  return facebook::fb303::fbData->getCounters()["netlink.requests.success.sum"];
}

void
printCounters() {
  LOG(INFO) << "Printing counters ";
  for (auto const& [key, value] : facebook::fb303::fbData->getCounters()) {
    LOG(INFO) << "  " << key << " : " << value;
  }
}

class NlMessageFixture : public ::testing::Test {
 public:
  NlMessageFixture() = default;
  ~NlMessageFixture() override = default;

  void
  SetUp() override {
    // Clear fb303 counters
    facebook::fb303::fbData->resetAllData();

    if (getuid()) {
      SKIP() << "Must run this test as root";
      return;
    }

    // cleanup old interfaces in any
    auto cmd = "ip link del {} 2>/dev/null"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    // Ignore result
    proc.wait();

    // add veth interface pair
    cmd = "ip link add {} type veth peer name {}"_shellify(
        kVethNameX.c_str(), kVethNameY.c_str());
    folly::Subprocess proc1(std::move(cmd));
    EXPECT_EQ(0, proc1.wait().exitStatus());

    addAddress(kVethNameX, ipAddrX1V6.str());
    addAddress(kVethNameY, ipAddrY1V6.str());
    addAddress(kVethNameY, ipAddrY2V6.str());
    addAddress(kVethNameY, ipAddrY3V6.str());
    addAddress(kVethNameY, ipAddrY4V6.str());

    addAddress(kVethNameX, ipAddrX1V4.str());
    addAddress(kVethNameY, ipAddrY1V4.str());
    addAddress(kVethNameY, ipAddrY2V4.str());

    // set interface status to up
    bringUpIntf(kVethNameX);
    bringUpIntf(kVethNameY);

    // netlink protocol socket
    nlSock = std::make_unique<NetlinkProtocolSocket>(
        &evb, FLAGS_enable_ipv6_rr_semantics);

    // start event thread
    eventThread = std::thread([&]() { evb.loopForever(); });
    evb.waitUntilRunning();

    // find ifIndexX and ifIndexY
    auto links = nlSock->getAllLinks().get();
    for (const auto& link : links) {
      if (link.getLinkName() == kVethNameX) {
        ifIndexX = link.getIfIndex();
      }
      if (link.getLinkName() == kVethNameY) {
        ifIndexY = link.getIfIndex();
      }
      if (link.getLinkName() == "lo") {
        ifIndexLo = link.getIfIndex();
      }
    }
    ASSERT_NE(ifIndexX, 0);
    ASSERT_NE(ifIndexY, 0);
    ASSERT_NE(ifIndexLo, 0);
  }

  void
  TearDown() override {
    if (getuid()) {
      // Nothing to cleanup if not-root
      return;
    }

    // cleanup virtual interfaces
    auto cmd = "ip link del {} 2>/dev/null"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    // Ignore result
    proc.wait();

    evb.terminateLoopSoon();
    eventThread.join();

    // print netlink counters
    printCounters();
  }

  void
  addNeighborEntry(
      const std::string& ifName,
      const folly::IPAddress& nextHopIp,
      const folly::MacAddress& linkAddr) {
    auto cmd = "ip -6 neigh add {} lladdr {} nud reachable dev {}"_shellify(
        nextHopIp.str().c_str(), linkAddr.toString().c_str(), ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  void
  deleteNeighborEntry(
      const std::string& ifName,
      const folly::IPAddress& nextHopIp,
      const folly::MacAddress& linkAddr) {
    // Now delete the neighbor entry from the system
    auto cmd = "ip -6 neigh del {} lladdr {} nud reachable dev {}"_shellify(
        nextHopIp.str().c_str(), linkAddr.toString().c_str(), ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  void
  addV4NeighborEntry(
      const std::string& ifName,
      const folly::IPAddress& nextHopIp,
      const folly::MacAddress& linkAddr) {
    auto cmd = "ip neigh add {} lladdr {} nud reachable dev {}"_shellify(
        nextHopIp.str().c_str(), linkAddr.toString().c_str(), ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  void
  deleteV4NeighborEntry(
      const std::string& ifName,
      const folly::IPAddress& nextHopIp,
      const folly::MacAddress& linkAddr) {
    // Now delete the neighbor entry from the system
    auto cmd = "ip neigh del {} lladdr {} nud reachable dev {}"_shellify(
        nextHopIp.str().c_str(), linkAddr.toString().c_str(), ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  // Check if route is present in routes obtained from kernel
  bool
  checkRouteInKernelRoutes(
      const std::vector<fbnl::Route>& kernelRoutes, const fbnl::Route& route) {
    LOG_FN_EXECUTION_TIME;
    for (auto& kernelRoute : kernelRoutes) {
      if (route == kernelRoute) {
        return true;
      }
    }
    return false;
  }

  // find count of routes present in the kernel routes
  int
  findRoutesInKernelRoutes(
      const std::vector<fbnl::Route>& kernelRoutes,
      const std::vector<fbnl::Route>& routes) {
    LOG_FN_EXECUTION_TIME;
    std::unordered_map<std::tuple<uint8_t, uint8_t, uint32_t>, fbnl::Route>
        mplsRoutes;
    std::unordered_map<
        std::tuple<uint8_t, uint8_t, folly::CIDRNetwork>,
        fbnl::Route>
        unicastRoutes;

    // Build map for efficient search
    for (auto const& route : kernelRoutes) {
      if (route.getFamily() == AF_MPLS) {
        mplsRoutes.emplace(
            std::make_tuple(
                route.getRouteTable(),
                route.getProtocolId(),
                route.getMplsLabel().value()),
            route);
      } else {
        unicastRoutes.emplace(
            std::make_tuple(
                route.getRouteTable(),
                route.getProtocolId(),
                route.getDestination()),
            route);
      }
    }

    int routeCount{0};
    for (auto const& route : routes) {
      std::optional<fbnl::Route> maybeRoute;
      if (route.getFamily() == AF_MPLS) {
        auto it = mplsRoutes.find(std::make_tuple(
            route.getRouteTable(),
            route.getProtocolId(),
            route.getMplsLabel().value()));
        if (it != mplsRoutes.end()) {
          maybeRoute = it->second;
        }
      } else {
        auto it = unicastRoutes.find(std::make_tuple(
            route.getRouteTable(),
            route.getProtocolId(),
            route.getDestination()));
        if (it != unicastRoutes.end()) {
          maybeRoute = it->second;
        }
      }
      if (maybeRoute.has_value() and route == maybeRoute.value()) {
        routeCount++;
      }
    }
    return routeCount;
  }

  // Check if if address is present in addresses obtained from kernel
  bool
  checkAddressInKernelAddresses(
      const std::vector<fbnl::IfAddress>& kernelAddresses,
      const fbnl::IfAddress& addr) {
    for (auto& kernelAddr : kernelAddresses) {
      if (addr == kernelAddr) {
        return true;
      }
    }
    return false;
  }

  // find count of addresses present in the kernel addresses
  int
  findAddressesInKernelAddresses(
      const std::vector<fbnl::IfAddress>& kernelAddresses,
      const std::vector<fbnl::IfAddress>& addresses) {
    int addrCount{0};
    for (auto& addr : addresses) {
      addrCount += checkAddressInKernelAddresses(kernelAddresses, addr) ? 1 : 0;
    }
    return addrCount;
  }

 private:
  static void
  bringUpIntf(const std::string& ifName) {
    auto cmd = "ip link set dev {} up"_shellify(ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  void
  addAddress(const std::string& ifName, const std::string& address) {
    auto cmd =
        "ip addr add {} dev {}"_shellify(address.c_str(), ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  folly::EventBase evb;
  std::thread eventThread;

 protected:
  openr::fbnl::NextHop
  buildNextHop(
      folly::Optional<std::vector<int32_t>> pushLabels,
      folly::Optional<uint32_t> swapLabel,
      folly::Optional<thrift::MplsActionCode> action,
      folly::Optional<folly::IPAddress> gateway,
      int ifIndex) {
    openr::fbnl::NextHopBuilder nhBuilder;

    if (pushLabels.has_value()) {
      nhBuilder.setPushLabels(pushLabels.value());
    }
    if (swapLabel.has_value()) {
      nhBuilder.setSwapLabel(swapLabel.value());
    }
    if (action.has_value()) {
      nhBuilder.setLabelAction(action.value());
    }
    if (gateway.has_value()) {
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
    if (dest.has_value()) {
      rtBuilder.setDestination(dest.value());
    }
    if (mplsLabel.has_value()) {
      rtBuilder.setMplsLabel(mplsLabel.value());
    }
    if (nexthops.has_value()) {
      for (const auto& nh : nexthops.value()) {
        rtBuilder.addNextHop(nh);
      }
    }
    // Default values
    if (dest.has_value()) {
      // Priority only for IPv4 and IPv6 routes
      rtBuilder.setPriority(kAqRouteProtoIdPriority);
    }
    rtBuilder.setFlags(0);
    rtBuilder.setValid(true);
    return rtBuilder.build();
  }

  std::vector<openr::fbnl::Route>
  buildV4RouteDb(uint32_t count) {
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
  buildV6RouteDb(uint32_t count) {
    std::vector<openr::fbnl::Route> routes;
    std::vector<openr::fbnl::NextHop> paths;
    // create mix of next hops, including without label
    paths.push_back(buildNextHop(
        outLabel1,
        folly::none,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V6,
        ifIndexX));
    paths.push_back(buildNextHop(
        outLabel2,
        folly::none,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V6,
        ifIndexX));
    paths.push_back(buildNextHop(
        folly::none, folly::none, folly::none, ipAddrY1V6, ifIndexX));
    paths.push_back(buildNextHop(
        outLabel4,
        folly::none,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V6,
        ifIndexX));
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
  uint32_t ifIndexLo{0};

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

TEST(NetlinkRouteMessage, EncodeLabel) {
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
    EXPECT_EQ(x.second, ntohl(NetlinkRouteMessage::encodeLabel(x.first, true)));
  }
}

/**
 * This test intends to test the delayed looping of event-base. Request is
 * made before event loop is started. This will help ensuring that socket
 * initialization and message sent happens inside event loop in sequence even
 * though message is requested to be sent before.
 */
TEST(NetlinkProtocolSocket, DelayedEventBase) {
  folly::EventBase evb;

  // Create netlink protocol socket
  NetlinkProtocolSocket nlSock(&evb);

  // Get the request
  // NOTE: Eventbase is not started yet
  auto links = nlSock.getAllLinks();

  // Start event thread
  std::thread evbThread([&]() { evb.loopForever(); });

  // Wait for links (SemiFuture) to get fulfilled
  EXPECT_NO_THROW(std::move(links).get());

  evb.terminateLoopSoon();
  evbThread.join();
}

/**
 * Test safe destruction of socket and handling of pending requests
 */
TEST(NetlinkProtocolSocket, SafeDestruction) {
  auto evb = std::make_unique<folly::EventBase>();

  // Create netlink protocol socket
  auto nlSock = std::make_unique<NetlinkProtocolSocket>(evb.get());

  // Add a request
  // NOTE: Eventbase is not running
  auto linksSf = nlSock->getAllLinks();

  // Destruct netlink socket
  EXPECT_NO_THROW(nlSock.reset());

  // Request should be set to timedout
  ASSERT_TRUE(linksSf.isReady());
  EXPECT_NO_THROW(std::move(linksSf).get());

  // Reset event base and it shouldn't throw
  EXPECT_NO_THROW(evb.reset());
}

TEST_F(NlMessageFixture, IpRouteSingleNextHop) {
  // Add IPv6 route with one next hop and no labels
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none, folly::none, folly::none, ipAddrY1V6, ifIndexX));
  auto route = buildRoute(kRouteProtoId, ipPrefix1, folly::none, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, IpRouteMultipleNextHops) {
  // Add IPv6 route with 4 next hops and no labels
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;

  paths.push_back(buildNextHop(
      folly::none, folly::none, folly::none, ipAddrY1V6, ifIndexX));
  paths.push_back(buildNextHop(
      folly::none, folly::none, folly::none, ipAddrY2V6, ifIndexX));
  paths.push_back(buildNextHop(
      folly::none, folly::none, folly::none, ipAddrY3V6, ifIndexX));
  paths.push_back(buildNextHop(
      folly::none, folly::none, folly::none, ipAddrY4V6, ifIndexX));

  auto route = buildRoute(kRouteProtoId, ipPrefix1, folly::none, paths);

  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify getAllRoutes
  auto kernelRoutes = nlSock->getIPv6Routes(kRouteProtoId).get();
  EXPECT_EQ(1, kernelRoutes.size());
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getIPv6Routes(kRouteProtoId).get();
  EXPECT_EQ(0, kernelRoutes.size());
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, IPv4RouteSingleNextHop) {
  // Add IPv4 route with one next hop and no labels
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  folly::CIDRNetwork ipPrefix1V4 =
      folly::IPAddress::createNetwork("10.10.0.0/24");
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none, folly::none, folly::none, ipAddrY1V4, ifIndexY));
  auto route = buildRoute(kRouteProtoId, ipPrefix1V4, folly::none, paths);

  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify getAllRoutes
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, IPv4RouteMultipleNextHops) {
  // Add IPv4 route with 2 next hops and no labels
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  folly::CIDRNetwork ipPrefix1V4 =
      folly::IPAddress::createNetwork("10.10.0.0/24");
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none, folly::none, folly::none, ipAddrY1V4, ifIndexY));
  paths.push_back(buildNextHop(
      folly::none, folly::none, folly::none, ipAddrY2V4, ifIndexY));
  auto route = buildRoute(kRouteProtoId, ipPrefix1V4, folly::none, paths);

  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify getAllRoutes
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, IpRouteLabelNexthop) {
  // Add IPv6 route with single next hop with one push label nexthop
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      outLabel1,
      folly::none,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V6,
      ifIndexX));
  auto route = buildRoute(kRouteProtoId, ipPrefix1, folly::none, paths);

  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify getAllRoutes
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, IpRouteMultipleLabelNextHops) {
  // Add IPv6 route with 48 path ECMP

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  for (uint32_t i = 0; i < 48; i++) {
    outLabel6[0] = outLabel6[0] + 10 + i;
    paths.push_back(buildNextHop(
        outLabel6,
        folly::none,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V6,
        ifIndexX));
  }

  auto route = buildRoute(kRouteProtoId, ipPrefix5, folly::none, paths);
  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify getAllRoutes for multiple label PUSH nexthops
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, MaxPayloadExceeded) {
  // check for max payload handling. Add nexthops that exceeds payload size
  // Should error out

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
        ifIndexY));
  }

  auto route = buildRoute(kRouteProtoId, folly::none, inLabel4, paths);
  EXPECT_EQ(ENOBUFS, nlSock->addRoute(route).get());
}

TEST_F(NlMessageFixture, PopLabel) {
  // pop label to loopback i/f

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::POP_AND_LOOKUP,
      folly::none,
      ifIndexLo));
  auto route = buildRoute(kRouteProtoId, folly::none, inLabel3, paths);
  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for single POP nexthop
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, PopMultipleNextHops) {
  // pop labels to different interfaces

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::POP_AND_LOOKUP,
      folly::none,
      ifIndexLo));
  paths.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::POP_AND_LOOKUP,
      folly::none,
      ifIndexY));
  auto route = buildRoute(kRouteProtoId, folly::none, inLabel3, paths);
  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for single POP nexthop
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, LabelRouteLabelNexthop) {
  // Add label route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none,
      swapLabel,
      thrift::MplsActionCode::SWAP,
      ipAddrY1V6,
      ifIndexX));
  auto route = buildRoute(kRouteProtoId, folly::none, inLabel3, paths);
  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for single SWAP label nexthop
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, LabelRouteLabelNexthops) {
  // Add label route with multiple SWAP label nexthops
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none,
      swapLabel,
      thrift::MplsActionCode::SWAP,
      ipAddrY1V6,
      ifIndexX));

  paths.push_back(buildNextHop(
      folly::none,
      swapLabel1,
      thrift::MplsActionCode::SWAP,
      ipAddrY2V6,
      ifIndexX));

  auto route = buildRoute(kRouteProtoId, folly::none, inLabel3, paths);
  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for muliple SWAP label nexthop
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
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
  EXPECT_EQ(0, getErrorCount());
  // create label next hop
  EXPECT_EQ(-ENODEV, nlSock->addRoute(route).get());
  EXPECT_NE(0, getErrorCount());
}

TEST_F(NlMessageFixture, InvalidRoute) {
  // Add two routes, one valid and the other invalid. Only one should be
  // sent to program

  std::vector<openr::fbnl::NextHop> paths1;
  std::vector<openr::fbnl::Route> routes;
  uint32_t ackCount{0};
  // Valid route with non-zero push labels.
  // NOTE: IP routes can only have `PUSH` instructions on their next-hops.
  // Linux 5.2+ have implemented a strict check on invalid mpls action
  paths1.push_back(buildNextHop(
      std::vector<int32_t>{outLabel1},
      folly::none,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V6,
      ifIndexX));
  routes.emplace_back(
      buildRoute(kRouteProtoId, ipPrefix1, folly::none, paths1));

  std::vector<openr::fbnl::NextHop> paths2;
  // Invalid route, send PUSH without labels
  paths2.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V6,
      ifIndexX));
  routes.emplace_back(
      buildRoute(kRouteProtoId, ipPrefix2, folly::none, paths2));

  ackCount = getAckCount();
  EXPECT_EQ(0, getErrorCount());
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(routes.at(0)).get());
  EXPECT_EQ(EINVAL, nlSock->addRoute(routes.at(1)).get());
  EXPECT_EQ(0, getErrorCount());
  // programmed 2 routes but should have received only 1 ack
  EXPECT_GE(getAckCount(), ackCount + 1);

  // delete needs only the destination prefix or label, doesn't
  // matter if the nexthop is valid or not. In this case delete will
  // be called for both routes but only one route is installed. Kernel
  // will return an error
  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(routes.at(0)).get());
  EXPECT_EQ(-ESRCH, nlSock->deleteRoute(routes.at(1)).get());
  EXPECT_EQ(0, getErrorCount()); // ESRCH is ignored in error count
  // deleting 2 routes but should have received only 1 ack
  EXPECT_GE(getAckCount(), ackCount + 1);
}

TEST_F(NlMessageFixture, MultipleIpRoutesLabelNexthop) {
  // Add IPv6 route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  uint32_t count{100000};
  const auto routes = buildV6RouteDb(count);

  ackCount = getAckCount();
  LOG(INFO) << "Adding " << count << " routes";
  {
    std::vector<folly::SemiFuture<int>> futures;
    for (auto& route : routes) {
      futures.emplace_back(nlSock->addRoute(route));
    }
    int status =
        NetlinkProtocolSocket::collectReturnStatus(std::move(futures)).get();
    EXPECT_EQ(0, status);
  }
  LOG(INFO) << "Done adding " << count << " routes";
  EXPECT_EQ(0, getErrorCount());
  // should have received acks with status = 0
  EXPECT_GE(getAckCount(), ackCount + count);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getIPv6Routes at scale
  auto kernelRoutes = nlSock->getIPv6Routes(kRouteProtoId).get();
  LOG(INFO) << "Checking if all routes are added to kernel";
  EXPECT_EQ(kernelRoutes.size(), routes.size());
  EXPECT_EQ(findRoutesInKernelRoutes(kernelRoutes, routes), count);

  // delete routes
  ackCount = getAckCount();
  {
    std::vector<folly::SemiFuture<int>> futures;
    for (auto& route : routes) {
      futures.emplace_back(nlSock->deleteRoute(route));
    }
    int status =
        NetlinkProtocolSocket::collectReturnStatus(std::move(futures)).get();
    EXPECT_EQ(0, status);
  }
  EXPECT_EQ(0, getErrorCount());
  // should have received acks status = 0
  EXPECT_GE(getAckCount(), ackCount + count);

  // verify route deletions
  kernelRoutes = nlSock->getIPv6Routes(kRouteProtoId).get();
  EXPECT_EQ(0, kernelRoutes.size());
}

TEST_F(NlMessageFixture, LabelRouteV4Nexthop) {
  // Add label route with single path label with PHP nexthop

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::PHP,
      ipAddrY1V4,
      ifIndexY));
  auto route = buildRoute(kRouteProtoId, folly::none, inLabel5, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify getAllRoutes
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, LabelRoutePHPNexthop) {
  // Add label route with single path label with PHP nexthop

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::PHP,
      ipAddrY1V6,
      ifIndexX));
  auto route1 = buildRoute(kRouteProtoId, folly::none, inLabel4, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route1).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for single hop MPLS PHP
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route1));

  paths.push_back(buildNextHop(
      folly::none,
      folly::none,
      thrift::MplsActionCode::PHP,
      ipAddrY2V6,
      ifIndexX));

  auto route2 = buildRoute(kRouteProtoId, folly::none, inLabel4, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route2).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for multiple next hop MPLS PHP
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route2));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route2).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route2));
}

TEST_F(NlMessageFixture, IpV4RouteLabelNexthop) {
  // Add IPv4 route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
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
      folly::none, folly::none, folly::none, ipAddrY2V4, ifIndexY));
  auto route = buildRoute(kRouteProtoId, ipPrefix1V4, folly::none, paths);

  ackCount = getAckCount();
  // create ipv4 route with label nexthop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  // should have received one ack with status = 0
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for IPv4 nexthops
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, MaxLabelStackTest) {
  // Add IPv4 route with 16 labels in the nexthop (which is max)
  // outoing IF is vethTestY

  uint32_t ackCount{0};
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

  ackCount = getAckCount();
  // create ipv4 route with label nexthop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  // should have received one ack with status = 0
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for max 16 labels
  auto kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, MultipleIpV4RouteLabelNexthop) {
  // Add Multiple IPv4 routes with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  const uint32_t count{100000};
  const auto routes = buildV4RouteDb(count);

  ackCount = getAckCount();
  LOG(INFO) << "Adding in bulk " << count << " routes";
  {
    std::vector<folly::SemiFuture<int>> futures;
    for (auto& route : routes) {
      futures.emplace_back(nlSock->addRoute(route));
    }
    int status =
        NetlinkProtocolSocket::collectReturnStatus(std::move(futures)).get();
    EXPECT_EQ(0, status);
  }
  LOG(INFO) << "Done adding " << count << " routes";
  EXPECT_EQ(0, getErrorCount());
  // should have received acks with status = 0
  EXPECT_GE(getAckCount(), ackCount + count);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getIPv4Routes at scale
  auto kernelRoutes = nlSock->getIPv4Routes(kRouteProtoId).get();
  EXPECT_EQ(kernelRoutes.size(), routes.size());
  LOG(INFO) << "Checking if all routes are added to kernel";
  EXPECT_EQ(findRoutesInKernelRoutes(kernelRoutes, routes), count);

  // delete routes
  LOG(INFO) << "Deleting in bulk " << count << " routes";
  ackCount = getAckCount();
  {
    std::vector<folly::SemiFuture<int>> futures;
    for (auto& route : routes) {
      futures.emplace_back(nlSock->deleteRoute(route));
    }
    int status =
        NetlinkProtocolSocket::collectReturnStatus(std::move(futures)).get();
    EXPECT_EQ(0, status);
  }
  LOG(INFO) << "Done deleting";
  EXPECT_EQ(0, getErrorCount());
  // should have received acks status = 0
  EXPECT_GE(getAckCount(), ackCount + count);

  // verify route deletions
  kernelRoutes = nlSock->getIPv4Routes(kRouteProtoId).get();
  EXPECT_EQ(0, kernelRoutes.size());
  EXPECT_EQ(findRoutesInKernelRoutes(kernelRoutes, routes), 0);

  // add routes one by one instead of a vector of routes
  LOG(INFO) << "Adding one by one " << count << " routes";
  ackCount = getAckCount();
  for (const auto& route : routes) {
    EXPECT_EQ(0, nlSock->addRoute(route).get());
  }
  LOG(INFO) << "Done adding " << count << " routes";
  EXPECT_EQ(0, getErrorCount());
  // should have received acks with status = 0
  EXPECT_GE(getAckCount(), ackCount + count);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getIPv4Routes at scale
  kernelRoutes = nlSock->getIPv4Routes(kRouteProtoId).get();
  EXPECT_EQ(kernelRoutes.size(), routes.size());
  LOG(INFO) << "Checking if all routes are added to kernel";
  EXPECT_EQ(findRoutesInKernelRoutes(kernelRoutes, routes), count);

  // delete routes one by one instead of a vector of routes
  LOG(INFO) << "Deleting " << count << " routes one at a time";
  ackCount = getAckCount();
  for (const auto& route : routes) {
    EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  }
  LOG(INFO) << "Done deleting";
  EXPECT_EQ(0, getErrorCount());
  // should have received acks status = 0
  EXPECT_GE(getAckCount(), ackCount + count);
  // verify route deletions
  kernelRoutes = nlSock->getIPv4Routes(kRouteProtoId).get();
  EXPECT_EQ(0, kernelRoutes.size());
}

TEST_F(NlMessageFixture, MultipleLabelRoutes) {
  // Add IPv6 route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  uint32_t count{20000};
  std::vector<openr::fbnl::NextHop> paths;
  // create label next hop
  paths.push_back(buildNextHop(
      folly::none,
      swapLabel,
      thrift::MplsActionCode::SWAP,
      ipAddrY1V6,
      ifIndexX));
  std::vector<openr::fbnl::Route> labelRoutes;
  for (uint32_t i = 0; i < count; i++) {
    labelRoutes.push_back(
        buildRoute(kRouteProtoId, folly::none, 600 + i, paths));
  }

  ackCount = getAckCount();
  LOG(INFO) << "Adding " << count << " label routes";
  {
    std::vector<folly::SemiFuture<int>> futures;
    for (auto& route : labelRoutes) {
      futures.emplace_back(nlSock->addRoute(route));
    }
    int status =
        NetlinkProtocolSocket::collectReturnStatus(std::move(futures)).get();
    EXPECT_EQ(0, status);
  }
  LOG(INFO) << "Done adding " << count << " label routes";
  EXPECT_GE(getAckCount(), ackCount + count);
  EXPECT_EQ(0, getErrorCount());

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getMplsRoutes at scale
  auto kernelRoutes = nlSock->getMplsRoutes(kRouteProtoId).get();
  LOG(INFO) << "Checking if all routes are added to kernel";
  EXPECT_EQ(kernelRoutes.size(), labelRoutes.size());
  EXPECT_EQ(findRoutesInKernelRoutes(kernelRoutes, labelRoutes), count);

  ackCount = getAckCount();
  {
    std::vector<folly::SemiFuture<int>> futures;
    for (auto& route : labelRoutes) {
      futures.emplace_back(nlSock->deleteRoute(route));
    }
    int status =
        NetlinkProtocolSocket::collectReturnStatus(std::move(futures)).get();
    EXPECT_EQ(0, status);
  }
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + count);

  // verify route deletions
  kernelRoutes = nlSock->getMplsRoutes(kRouteProtoId).get();
  EXPECT_EQ(0, kernelRoutes.size());
}

// Add and remove 250 IPv4 and IPv6 addresses (total 500)
TEST_F(NlMessageFixture, AddrScaleTest) {
  const int addrCount{250};

  auto links = nlSock->getAllLinks().get();
  // Find kVethNameX
  int ifIndexX{-1};
  for (const auto& link : links) {
    if (link.getLinkName() == kVethNameX) {
      ifIndexX = link.getIfIndex();
    }
  }
  EXPECT_NE(ifIndexX, -1);

  std::vector<fbnl::IfAddress> ifAddresses;
  for (int i = 0; i < addrCount; i++) {
    openr::fbnl::IfAddressBuilder builder;
    folly::CIDRNetwork prefix1{
        folly::IPAddress("face:d00d::" + std::to_string(i)), 128};
    auto ifAddr = builder.setPrefix(prefix1)
                      .setIfIndex(ifIndexX)
                      .setScope(RT_SCOPE_UNIVERSE)
                      .setValid(true)
                      .build();
    EXPECT_EQ(0, nlSock->addIfAddress(ifAddr).get());
    ifAddresses.emplace_back(ifAddr);

    openr::fbnl::IfAddressBuilder ipv4builder;
    folly::CIDRNetwork prefix2{
        folly::IPAddress("10.0." + std::to_string(i) + ".0"), 32};
    auto ifAddrV4 = ipv4builder.setPrefix(prefix2)
                        .setIfIndex(ifIndexX)
                        .setScope(RT_SCOPE_UNIVERSE)
                        .setValid(true)
                        .build();
    EXPECT_EQ(0, nlSock->addIfAddress(ifAddrV4).get());
    ifAddresses.emplace_back(ifAddrV4);
  }

  // Verify if addresses have been added
  auto kernelAddresses = nlSock->getAllIfAddresses().get();
  EXPECT_EQ(
      2 * addrCount,
      findAddressesInKernelAddresses(kernelAddresses, ifAddresses));

  for (int i = 0; i < addrCount; i++) {
    openr::fbnl::IfAddressBuilder builder;
    folly::CIDRNetwork prefix1{
        folly::IPAddress("face:d00d::" + std::to_string(i)), 128};
    builder.setPrefix(prefix1).setIfIndex(ifIndexX).setScope(RT_SCOPE_UNIVERSE);
    EXPECT_EQ(0, nlSock->deleteIfAddress(builder.build()).get());

    openr::fbnl::IfAddressBuilder ipv4builder;
    folly::CIDRNetwork prefix2{
        folly::IPAddress("10.0." + std::to_string(i) + ".0"), 32};
    ipv4builder.setPrefix(prefix2).setIfIndex(ifIndexX).setScope(
        RT_SCOPE_UNIVERSE);
    EXPECT_EQ(0, nlSock->deleteIfAddress(ipv4builder.build()).get());
  }

  // Verify if addresses have been deleted
  kernelAddresses = nlSock->getAllIfAddresses().get();
  EXPECT_EQ(0, findAddressesInKernelAddresses(kernelAddresses, ifAddresses));
}

TEST_F(NlMessageFixture, GetAllNeighbors) {
  // Add 100 neighbors and check if getAllReachableNeighbors
  // in NetlinkProtocolSocket returns the neighbors
  int countNeighbors{100};
  LOG(INFO) << "Adding " << countNeighbors << " test neighbors";
  // Bring up neighbors
  for (int i = 0; i < countNeighbors; i++) {
    addNeighborEntry(
        kVethNameX,
        folly::IPAddress{"face:b00c::" + std::to_string(i)},
        kLinkAddr1);
  }

  // Get links and neighbors
  LOG(INFO) << "Getting links and neighbors";
  auto links = nlSock->getAllLinks().get();
  auto neighbors = nlSock->getAllNeighbors().get();

  // Find kVethNameX
  int ifIndexX{-1};
  for (const auto& link : links) {
    if (link.getLinkName() == kVethNameX) {
      ifIndexX = link.getIfIndex();
    }
  }
  EXPECT_NE(ifIndexX, -1);

  int testNeighbors = 0;
  for (const auto& neighbor : neighbors) {
    if (neighbor.getIfIndex() == ifIndexX &&
        neighbor.getDestination().str().find("face:b00c::") !=
            std::string::npos &&
        neighbor.isReachable()) {
      // Found neighbor on vethTestX with face:b00c::i address
      testNeighbors += 1;
      // Check if neighbor has the correct MAC address
      EXPECT_EQ(kLinkAddr1, neighbor.getLinkAddress());
    }
  }
  // Check if Netlink returned all the test neighbors
  EXPECT_EQ(testNeighbors, countNeighbors);
  EXPECT_EQ(0, getErrorCount());

  // Delete neighbors
  LOG(INFO) << "Deleting " << countNeighbors << " test neighbors";
  for (int i = 0; i < countNeighbors; i++) {
    deleteNeighborEntry(
        kVethNameX,
        folly::IPAddress{"face:b00c::" + std::to_string(i)},
        kLinkAddr1);
  }

  // Check if getAllNeighbors do not return any reachable neighbors on
  // kVethNameX
  neighbors = nlSock->getAllNeighbors().get();
  testNeighbors = 0;
  for (const auto& neighbor : neighbors) {
    if (neighbor.getIfIndex() == ifIndexX &&
        neighbor.getDestination().str().find("face:b00c::") !=
            std::string::npos &&
        neighbor.isReachable()) {
      // Found neighbor on vethTestX with face:b00c::i address
      testNeighbors += 1;
    }
  }
  // All test neighbors are unreachable,
  // GetAllReachableNeighbors shouldn't return them
  EXPECT_EQ(testNeighbors, 0);
}

TEST_F(NlMessageFixture, GetAllNeighborsV4) {
  // Add 100 V4 neighbors and check if getAllNeighbors
  // in NetlinkProtocolSocket returns the neighbors
  int countNeighbors{100};
  LOG(INFO) << "Adding " << countNeighbors << " test V4 neighbors";
  // Bring up neighbors
  for (int i = 0; i < countNeighbors; i++) {
    addV4NeighborEntry(
        kVethNameX,
        folly::IPAddress{"172.8.0." + std::to_string(i)},
        kLinkAddr1);
  }

  // Get links and neighbors
  LOG(INFO) << "Getting links and neighbors";
  auto links = nlSock->getAllLinks().get();
  auto neighbors = nlSock->getAllNeighbors().get();

  // Find kVethNameX
  int ifIndexX{-1};
  for (const auto& link : links) {
    if (link.getLinkName() == kVethNameX) {
      ifIndexX = link.getIfIndex();
    }
  }
  EXPECT_NE(ifIndexX, -1);

  int testNeighbors = 0;
  for (const auto& neighbor : neighbors) {
    if (neighbor.getIfIndex() == ifIndexX &&
        neighbor.getDestination().str().find("172.8.0.") != std::string::npos &&
        neighbor.isReachable()) {
      // Found neighbor on vethTestX with face:b00c::i address
      testNeighbors += 1;
      // Check if neighbor has the correct MAC address
      EXPECT_EQ(kLinkAddr1, neighbor.getLinkAddress());
    }
  }
  // Check if Netlink returned all the test neighbors
  EXPECT_EQ(testNeighbors, countNeighbors);
  EXPECT_EQ(0, getErrorCount());

  // Delete neighbors
  LOG(INFO) << "Deleting " << countNeighbors << " test neighbors";
  for (int i = 0; i < countNeighbors; i++) {
    deleteV4NeighborEntry(
        kVethNameX,
        folly::IPAddress{"172.8.0." + std::to_string(i)},
        kLinkAddr1);
  }

  // Check if getAllNeighbors do not return any reachable neighbors on
  // kVethNameX
  neighbors = nlSock->getAllNeighbors().get();
  testNeighbors = 0;
  for (const auto& neighbor : neighbors) {
    if (neighbor.getIfIndex() == ifIndexX &&
        neighbor.getDestination().str().find("172.8.0.") != std::string::npos &&
        neighbor.isReachable()) {
      // Found neighbor on vethTestX with face:b00c::i address
      testNeighbors += 1;
    }
  }
  // All test neighbors are unreachable,
  // GetAllReachableNeighbors should not return them
  EXPECT_EQ(testNeighbors, 0);
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
