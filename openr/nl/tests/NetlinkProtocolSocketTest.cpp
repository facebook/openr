/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
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
const std::chrono::milliseconds kProcTimeout{500};
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

// TODO: Fix the subnet of vethX and vethY for v4 & v6 both. Make it easier
// to get local interface address or remote interface address
folly::IPAddress ipAddrX1V4{"172.10.10.10"};
folly::IPAddress ipAddrX1V4Peer{"172.10.10.11"};
folly::IPAddress ipAddrY1V4{"172.10.11.10"};
folly::IPAddress ipAddrY1V4Peer{"172.10.11.11"};
folly::IPAddress ipAddrY2V4{"172.10.11.20"};
folly::IPAddress ipAddrY2V4Peer{"172.10.11.21"};
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
    addIntfPair(kVethNameX, kVethNameY);

    // add addresses for interfaces
    addAddress(kVethNameX, ipAddrX1V6.str(), 64);
    addAddress(kVethNameY, ipAddrY1V6.str(), 64);
    addAddress(kVethNameY, ipAddrY2V6.str(), 64);
    addAddress(kVethNameY, ipAddrY3V6.str(), 64);
    addAddress(kVethNameY, ipAddrY4V6.str(), 64);

    addAddress(kVethNameX, ipAddrX1V4.str(), 31);
    addAddress(kVethNameY, ipAddrY1V4.str(), 31);
    addAddress(kVethNameY, ipAddrY2V4.str(), 31);

    // set interface status to up
    bringUpIntf(kVethNameX);
    bringUpIntf(kVethNameY);

    // netlink protocol socket
    nlSock = std::make_unique<NetlinkProtocolSocket>(
        &evb, netlinkEventsQ, FLAGS_enable_ipv6_rr_semantics);

    // start event thread
    eventThread = std::thread([&]() { evb.loopForever(); });
    evb.waitUntilRunning();

    // find ifIndexX and ifIndexY
    auto links = nlSock->getAllLinks().get().value();
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
    deleteIntfPair(kVethNameX.c_str());

    // messaging::queue closing
    netlinkEventsQ.close();

    evb.terminateLoopSoon();
    eventThread.join();

    // print netlink counters
    printCounters();
  }

  void
  addIntfPair(const std::string& ifNameA, const std::string& ifNameB) {
    auto cmd = "ip link add {} type veth peer name {}"_shellify(
        ifNameA.c_str(), ifNameB.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  void
  deleteIntfPair(const std::string& ifNameA) {
    auto cmd = "ip link delete {}"_shellify(ifNameA.c_str());
    folly::Subprocess proc(std::move(cmd));
    // Ignore result
    proc.wait();
  }

  void
  addV6NeighborEntry(
      const std::string& ifName,
      const folly::IPAddress& nextHopIp,
      const folly::MacAddress& linkAddr) {
    VLOG(1) << fmt::format(
        "Adding IPV6 neighbor entry for ip address: {}, mac address: {}",
        nextHopIp.str(),
        linkAddr.toString());
    auto cmd = "ip -6 neigh add {} lladdr {} nud reachable dev {}"_shellify(
        nextHopIp.str().c_str(), linkAddr.toString().c_str(), ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  void
  deleteV6NeighborEntry(
      const std::string& ifName,
      const folly::IPAddress& nextHopIp,
      const folly::MacAddress& linkAddr) {
    VLOG(1) << fmt::format(
        "Deleting IPV6 neighbor entry for ip address: {}, mac address: {}",
        nextHopIp.str(),
        linkAddr.toString());
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
    VLOG(1) << fmt::format(
        "Adding ARP entry for ip address: {}, mac address: {}",
        nextHopIp.str(),
        linkAddr.toString());
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
    VLOG(1) << fmt::format(
        "Deleting ARP entry for ip address: {}, mac address: {}",
        nextHopIp.str(),
        linkAddr.toString());
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
  folly::EventBase evb;
  std::thread eventThread;

 protected:
  static void
  bringUpIntf(const std::string& ifName) {
    auto cmd = "ip link set dev {} up"_shellify(ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  static void
  bringDownIntf(const std::string& ifName) {
    auto cmd = "ip link set dev {} down"_shellify(ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  void
  addAddress(
      const std::string& ifName, const std::string& address, size_t mask) {
    auto cmd = fmt::format("ip addr add {}/{} dev {}", address, mask, ifName);
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  void
  deleteAddress(
      const std::string& ifName, const std::string& address, size_t mask) {
    auto cmd = fmt::format("ip addr del {}/{} dev {}", address, mask, ifName);
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  openr::fbnl::NextHop
  buildNextHop(
      std::optional<std::vector<int32_t>> pushLabels,
      std::optional<uint32_t> swapLabel,
      std::optional<thrift::MplsActionCode> action,
      std::optional<folly::IPAddress> gateway,
      std::optional<int> ifIndex,
      std::optional<uint8_t> weight = std::nullopt) {
    openr::fbnl::NextHopBuilder nhBuilder;

    if (weight.has_value()) {
      nhBuilder.setWeight(weight.value());
    }
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
    if (ifIndex.has_value()) {
      nhBuilder.setIfIndex(ifIndex.value());
    }
    return nhBuilder.build();
  }

  openr::fbnl::Route
  buildNullRoute(int protocolId, const folly::CIDRNetwork& dest) {
    fbnl::RouteBuilder rtBuilder;
    auto route = rtBuilder.setDestination(dest)
                     .setProtocolId(protocolId)
                     .setType(RTN_BLACKHOLE);
    return rtBuilder.build();
  }

  openr::fbnl::Route
  buildRoute(
      int protocolId,
      const std::optional<folly::CIDRNetwork>& dest,
      std::optional<uint32_t> mplsLabel,
      const std::optional<std::vector<openr::fbnl::NextHop>>& nexthops) {
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
        std::nullopt,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V4,
        ifIndexY,
        1));
    paths.push_back(buildNextHop(
        outLabel5,
        std::nullopt,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V4,
        ifIndexY,
        1));
    paths.push_back(buildNextHop(
        outLabel6,
        std::nullopt,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V4,
        ifIndexY,
        1));

    struct v4Addr addr4 {};
    for (uint32_t i = 0; i < count; i++) {
      addr4.u32_addr = 0x000000A0 + i;
      folly::IPAddress ipAddress =
          folly::IPAddress::fromBinary(folly::ByteRange(
              static_cast<const unsigned char*>(&addr4.u8_addr[0]), 4));
      folly::CIDRNetwork prefix = std::make_pair(ipAddress, 30);
      routes.emplace_back(
          buildRoute(kRouteProtoId, prefix, std::nullopt, paths));
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
        std::nullopt,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V6,
        ifIndexX,
        1));
    paths.push_back(buildNextHop(
        outLabel2,
        std::nullopt,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V6,
        ifIndexX,
        1));
    paths.push_back(buildNextHop(
        std::nullopt, std::nullopt, std::nullopt, ipAddrY1V6, ifIndexX, 1));
    paths.push_back(buildNextHop(
        outLabel4,
        std::nullopt,
        thrift::MplsActionCode::PUSH,
        ipAddrY1V6,
        ifIndexX,
        1));
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
          buildRoute(kRouteProtoId, prefix, std::nullopt, paths));
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
  messaging::ReplicateQueue<openr::fbnl::NetlinkEvent> netlinkEventsQ;
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
  messaging::ReplicateQueue<openr::fbnl::NetlinkEvent> netlinkEventsQ;

  // Create netlink protocol socket
  NetlinkProtocolSocket nlSock(&evb, netlinkEventsQ);

  // Get the request
  // NOTE: Eventbase is not started yet
  auto links = nlSock.getAllLinks();

  // Start event thread
  std::thread evbThread([&]() { evb.loopForever(); });

  // Wait for links (SemiFuture) to get fulfilled
  EXPECT_NO_THROW(std::move(links).get().value());

  evb.terminateLoopSoon();
  evbThread.join();
}

/**
 * Test error conditions for get<> APIs of each netlink message type.
 * - Enqueue request, but don't pump the event-base
 * - Destroy the netlink-protocol socket class
 * - Observe error condition for -ESHUTDOWN
 */
TEST(NetlinkProtocolSocket, GetApiError) {
  // Create netlink protocol socket
  folly::EventBase evb;
  messaging::ReplicateQueue<openr::fbnl::NetlinkEvent> netlinkEventsQ;
  auto nlSock = std::make_unique<NetlinkProtocolSocket>(&evb, netlinkEventsQ);

  // Make requests
  auto addrs = nlSock->getAllIfAddresses();
  auto links = nlSock->getAllLinks();
  auto nbrs = nlSock->getAllNeighbors();
  auto routes = nlSock->getAllRoutes();

  // Destroy socket
  nlSock.reset();

  // Make sure the requests are fulfilled and have error code set
  ASSERT_TRUE(addrs.isReady());
  EXPECT_EQ(-ESHUTDOWN, std::move(addrs).get().error());

  ASSERT_TRUE(links.isReady());
  EXPECT_EQ(-ESHUTDOWN, std::move(links).get().error());

  ASSERT_TRUE(nbrs.isReady());
  EXPECT_EQ(-ESHUTDOWN, std::move(nbrs).get().error());

  ASSERT_TRUE(routes.isReady());
  EXPECT_EQ(-ESHUTDOWN, std::move(routes).get().error());
}

/**
 * Test safe destruction of socket and handling of pending requests
 */
TEST(NetlinkProtocolSocket, SafeDestruction) {
  auto evb = std::make_unique<folly::EventBase>();
  messaging::ReplicateQueue<openr::fbnl::NetlinkEvent> netlinkEventsQ;

  // Create netlink protocol socket
  auto nlSock =
      std::make_unique<NetlinkProtocolSocket>(evb.get(), netlinkEventsQ);

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

/*
 * Spawn RQueue of `NetlinkEvent` to verify:
 *  1) LINK_EVENT(DOWN) is populated through replicate queue;
 *  2) RQueue can receive updates from different interfaces;
 *  3) verify attributes: ifName/isUp/weight/ifIndex;
 */
TEST_F(NlMessageFixture, LinkEventPublication) {
  // Spawn RQueue to receive platformUpdate request
  auto netlinkEventsReader = netlinkEventsQ.getReader();

  auto waitForLinkEvent = [&](const std::string& ifName, const bool& isUp) {
    auto startTime = std::chrono::steady_clock::now();
    while (true) {
      // check if it is beyond kProcTimeout
      auto endTime = std::chrono::steady_clock::now();
      if (endTime - startTime > kProcTimeout) {
        ASSERT_TRUE(0) << fmt::format(
            "Timeout receiving expected link event for intf: {}. Time limit: {}",
            ifName,
            kProcTimeout.count());
      }
      auto req = netlinkEventsReader.get(); // perform read
      ASSERT_TRUE(req.hasValue());
      // get_if returns `nullptr` if targeted variant is NOT populated
      if (auto* link = std::get_if<openr::fbnl::Link>(&req.value())) {
        if (link->getLinkName() == ifName and link->isUp() == isUp) {
          return;
        }
      }
      // yield CPU
      std::this_thread::yield();
    }
  };

  {
    VLOG(1) << "Bring link DOWN for interfaces: "
            << fmt::format("{}, {}", kVethNameX, kVethNameY);
    // bring DOWN link to trigger link DOWN event
    bringDownIntf(kVethNameX);
    bringDownIntf(kVethNameY);

    waitForLinkEvent(kVethNameX, false);
    waitForLinkEvent(kVethNameY, false);
  }

  {
    VLOG(1) << "Bring link UP for interfaces: "
            << fmt::format("{}, {}", kVethNameX, kVethNameY);
    // bring UP link to trigger link UP event
    bringUpIntf(kVethNameX);
    bringUpIntf(kVethNameY);

    waitForLinkEvent(kVethNameX, true);
    waitForLinkEvent(kVethNameX, true);
  }
}

/*
 * Spawn RQueue of `NetlinkEvent` to verify:
 *  1) ADDR_EVENT(ADD) is populated through replicate queue;
 *  2) RQueue can receive updates from different interfaces;
 *  3) verify attributes by cross-reference between:
 *      [ifName => openr::fbnl::IfAddress]
 *      and
 *      [ifIndex => ifName]
 */
TEST_F(NlMessageFixture, AddressEventPublication) {
  // Spawn RQueue to receive platformUpdate request for addr event
  auto netlinkEventsReader = netlinkEventsQ.getReader();
  std::unordered_map<int64_t, std::string> ifIndexToName;

  // Create CIDRNetwork addresses
  const folly::CIDRNetwork ipAddrX{folly::IPAddress("face:b00c::1"), 128};
  const folly::CIDRNetwork ipAddrY{folly::IPAddress("face:b00c::2"), 128};

  // Establish ifIndex -> ifName mapping
  auto links = nlSock->getAllLinks().get().value();
  for (const auto& link : links) {
    ifIndexToName.emplace(link.getIfIndex(), link.getLinkName());
  }

  auto waitForAddrEvent = [&](const std::string& ifName,
                              const folly::CIDRNetwork& ipAddr,
                              const bool& isValid) {
    auto startTime = std::chrono::steady_clock::now();
    while (true) {
      // check if it is beyond kProcTimeout
      auto endTime = std::chrono::steady_clock::now();
      if (endTime - startTime > kProcTimeout) {
        ASSERT_TRUE(0) << fmt::format(
            "Timeout receiving expected address event for intf: {}, address: {}. Time limit: {}",
            ifName,
            folly::IPAddress::networkToString(ipAddr),
            kProcTimeout.count());
      }
      auto req = netlinkEventsReader.get(); // perform read
      ASSERT_TRUE(req.hasValue());
      // get_if returns `nullptr` if targeted variant is NOT populated
      if (auto* addr = std::get_if<openr::fbnl::IfAddress>(&req.value())) {
        ASSERT_TRUE(ifIndexToName.count(addr->getIfIndex()));
        if (ifIndexToName.at(addr->getIfIndex()) == ifName and
            addr->getPrefix().has_value() and
            addr->getPrefix().value() == ipAddr and
            addr->isValid() == isValid) {
          return;
        }
      }
      // yield CPU
      std::this_thread::yield();
    }
  };

  //
  // Test add/delete address via Linux system call
  //
  {
    VLOG(1) << "Adding address for interfaces: "
            << fmt::format("{}, {}", kVethNameX, kVethNameY)
            << " via system call";
    addAddress(kVethNameX, ipAddrX.first.str(), ipAddrX.second);
    addAddress(kVethNameY, ipAddrY.first.str(), ipAddrY.second);

    waitForAddrEvent(kVethNameX, ipAddrX, true);
    waitForAddrEvent(kVethNameY, ipAddrY, true);
  }

  {
    VLOG(1) << "Removing address for interfaces: "
            << fmt::format("{}, {}", kVethNameX, kVethNameY)
            << " via system call";
    deleteAddress(kVethNameX, ipAddrX.first.str(), ipAddrX.second);
    deleteAddress(kVethNameY, ipAddrY.first.str(), ipAddrY.second);

    waitForAddrEvent(kVethNameX, ipAddrX, false);
    waitForAddrEvent(kVethNameY, ipAddrY, false);
  }

  //
  // Test add/delete address via NetlinkProtocolSocket API
  //
  std::vector<fbnl::IfAddress> ifAddresses;
  {
    VLOG(1) << "Adding address for interfaces: "
            << fmt::format("{}, {}", kVethNameX, kVethNameY)
            << " via NetlinkProtocolSocket API";

    openr::fbnl::IfAddressBuilder builder1;
    auto ifAddr1 = builder1.setPrefix(ipAddrX)
                       .setIfIndex(ifIndexX)
                       .setScope(RT_SCOPE_UNIVERSE)
                       .setValid(true)
                       .build();
    EXPECT_EQ(0, nlSock->addIfAddress(ifAddr1).get());
    ifAddresses.emplace_back(ifAddr1);

    openr::fbnl::IfAddressBuilder builder2;
    auto ifAddr2 = builder2.setPrefix(ipAddrY)
                       .setIfIndex(ifIndexY)
                       .setScope(RT_SCOPE_UNIVERSE)
                       .setValid(true)
                       .build();
    EXPECT_EQ(0, nlSock->addIfAddress(ifAddr2).get());
    ifAddresses.emplace_back(ifAddr2);

    // Verify if addresses have been added
    waitForAddrEvent(kVethNameX, ipAddrX, true);
    waitForAddrEvent(kVethNameY, ipAddrY, true);

    auto kernelAddresses = nlSock->getAllIfAddresses().get().value();
    EXPECT_EQ(2, findAddressesInKernelAddresses(kernelAddresses, ifAddresses));
  }

  {
    VLOG(1) << "Removing address for interfaces: "
            << fmt::format("{}, {}", kVethNameX, kVethNameY)
            << " via NetlinkProtocolSocket API";

    openr::fbnl::IfAddressBuilder builder1;
    auto ifAddr1 = builder1.setPrefix(ipAddrX)
                       .setIfIndex(ifIndexX)
                       .setScope(RT_SCOPE_UNIVERSE)
                       .build();
    EXPECT_EQ(0, nlSock->deleteIfAddress(ifAddr1).get());

    openr::fbnl::IfAddressBuilder builder2;
    auto ifAddr2 = builder2.setPrefix(ipAddrY)
                       .setIfIndex(ifIndexY)
                       .setScope(RT_SCOPE_UNIVERSE)
                       .build();
    EXPECT_EQ(0, nlSock->deleteIfAddress(ifAddr2).get());

    // Verify if addresses have been deleted
    waitForAddrEvent(kVethNameX, ipAddrX, false);
    waitForAddrEvent(kVethNameY, ipAddrY, false);

    auto kernelAddresses = nlSock->getAllIfAddresses().get().value();
    EXPECT_EQ(0, findAddressesInKernelAddresses(kernelAddresses, ifAddresses));
  }
}

/*
 * Spawn RQueue of `NetlinkEvent` to verify:
 *  1) NEIGHBOR_EVENT(ADD) is populated through replicate queue;
 *  2) RQueue can receive updates of different address family;
 *  3) verify attributes: family/isReachable/link-address/ifIndex;
 */
TEST_F(NlMessageFixture, NeighborEventPublication) {
  // spawn RQueue to receive platformUpdate request for neigh event
  auto netlinkEventsReader = netlinkEventsQ.getReader();

  // Create CIDRNetwork addresses
  const folly::IPAddress addressV4{"172.8.0.1"};
  const folly::IPAddress addressV6{"face:b00c::1"};

  auto waitForNeighborEvent = [&](const folly::IPAddress& ipAddr,
                                  const folly::MacAddress& macAddress,
                                  const bool& isReachable) {
    auto startTime = std::chrono::steady_clock::now();
    while (true) {
      // check if it is beyond kProcTimeout
      auto endTime = std::chrono::steady_clock::now();
      if (endTime - startTime > kProcTimeout) {
        ASSERT_TRUE(0) << fmt::format(
            "Timeout receiving expected neighbor event for ip address: {}, mac address: {}, isReachable: {}, Time limit: {}",
            ipAddr.str(),
            macAddress.toString(),
            isReachable,
            kProcTimeout.count());
      }
      auto req = netlinkEventsReader.get(); // perform read
      ASSERT_TRUE(req.hasValue());
      // get_if returns `nullptr` if targeted variant is NOT populated
      if (auto* neigh = std::get_if<openr::fbnl::Neighbor>(&req.value())) {
        if (neigh->getDestination() == ipAddr and
            neigh->isReachable() == isReachable and
            neigh->getFamily() == (ipAddr.isV4() ? AF_INET : AF_INET6)) {
          // ATTN: neighbor delete msg doesn't have link address field populated
          if (isReachable and neigh->getLinkAddress().has_value() and
              neigh->getLinkAddress().value() == macAddress) {
            return;
          } else if (
              (not isReachable) and (not neigh->getLinkAddress().has_value())) {
            return;
          }
        }
      }
      // yield CPU
      std::this_thread::yield();
    }
  };

  {
    addV4NeighborEntry(kVethNameX, addressV4, kLinkAddr1);
    addV6NeighborEntry(kVethNameX, addressV6, kLinkAddr2);

    waitForNeighborEvent(addressV4, kLinkAddr1, true);
    waitForNeighborEvent(addressV6, kLinkAddr2, true);
  }

  {
    deleteV4NeighborEntry(kVethNameX, addressV4, kLinkAddr1);
    deleteV6NeighborEntry(kVethNameX, addressV6, kLinkAddr2);

    waitForNeighborEvent(addressV4, kLinkAddr1, false);
    waitForNeighborEvent(addressV6, kLinkAddr2, false);
  }
}

/*
 * Add and delete duplicate interface addresses to make sure
 * NetlinkProtocolSocket can take care of duplicate requests
 */
TEST_F(NlMessageFixture, AddDelDuplicateIfAddress) {
  auto network = folly::IPAddress::createNetwork("fc00:cafe:4::4/128");
  openr::fbnl::IfAddressBuilder builder;

  auto ifAddr = builder.setPrefix(network)
                    .setIfIndex(ifIndexX)
                    .setScope(RT_SCOPE_UNIVERSE)
                    .setValid(true)
                    .build();
  auto ifAddrDup = ifAddr; // NOTE: explicit copy

  auto before = nlSock->getAllIfAddresses().get().value();

  // Add new interface address
  EXPECT_EQ(0, nlSock->addIfAddress(ifAddr).get());
  // Add duplicated address entry. -EEXIST error.
  EXPECT_EQ(-EEXIST, nlSock->addIfAddress(ifAddrDup).get());

  auto after = nlSock->getAllIfAddresses().get().value();
  EXPECT_EQ(before.size() + 1, after.size());

  // Delete interface address
  EXPECT_EQ(0, nlSock->deleteIfAddress(ifAddr).get());
  // Double delete. -EADDRNOTAVAIL error.
  EXPECT_EQ(-EADDRNOTAVAIL, nlSock->deleteIfAddress(ifAddrDup).get());

  auto kernelAddresses = nlSock->getAllIfAddresses().get().value();
  EXPECT_EQ(before.size(), kernelAddresses.size());
}

TEST_F(NlMessageFixture, InvalidIfAddress) {
  openr::fbnl::IfAddressBuilder builder;

  // Case 1: Invalid interface address with `AF_UNSPEC`
  // Should be either `AF_INET` or `AF_INET6`
  //
  // ATTN: to test INVALID family, do NOT populate `prefix_`
  // field. Otherwise, it will ignore `setFamily()` and honor
  // `prefix_.first.family()`
  auto ifAddr1 = builder.setFamily(AF_UNSPEC).build();
  builder.reset();

  // Case 2: Invalid interface address without `prefix_`
  auto ifAddr2 = builder.setFamily(AF_INET6).build();
  builder.reset();

  // Verify addIfAddress() API failed sanity check
  EXPECT_EQ(EINVAL, nlSock->addIfAddress(ifAddr1).get());
  EXPECT_EQ(EDESTADDRREQ, nlSock->addIfAddress(ifAddr2).get());

  // Verify deleteIfAddress() API failed sanity check
  EXPECT_EQ(EINVAL, nlSock->deleteIfAddress(ifAddr1).get());
  EXPECT_EQ(EDESTADDRREQ, nlSock->deleteIfAddress(ifAddr2).get());
}

/*
 * Check empty route from kernel
 */
TEST_F(NlMessageFixture, EmptyRoute) {
  auto kernelRoutesV4 = nlSock->getIPv4Routes(kRouteProtoId).get().value();
  auto kernelRoutesV6 = nlSock->getIPv6Routes(kRouteProtoId).get().value();

  EXPECT_EQ(0, kernelRoutesV4.size());
  EXPECT_EQ(0, kernelRoutesV6.size());
}

/*
 * Add IPv6 drop route(empty next-hop)
 */
TEST_F(NlMessageFixture, IPv6DropRoute) {
  uint32_t ackCount{0};
  folly::CIDRNetwork network =
      folly::IPAddress::createNetwork("fc00:cafe:4::4/128");
  auto route = buildNullRoute(kRouteProtoId, network);
  auto before = nlSock->getAllRoutes().get().value();

  // Add drop route
  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // Check in Kernel
  // v6 blackhole route has default nexthop point to lo
  // E.g. blackhole 2401:db00:e003:9100:106f::/80 dev lo
  auto after = nlSock->getAllRoutes().get().value();
  EXPECT_EQ(before.size() + 1, after.size());
  bool found = false;
  for (const auto& r : after) {
    if (r.getDestination() == network and r.getProtocolId() == kRouteProtoId and
        r.getNextHops().size() == 1 and r.getType() == RTN_BLACKHOLE) {
      found = true;
    }
  }
  EXPECT_TRUE(found);

  // Delete the routes
  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  after = nlSock->getAllRoutes().get().value();
  EXPECT_EQ(before.size(), after.size());
}

/*
 * Add IPv6 drop route(empty next-hop)
 */
TEST_F(NlMessageFixture, IPv4DropRoute) {
  uint32_t ackCount{0};
  folly::CIDRNetwork network =
      folly::IPAddress::createNetwork("192.168.0.11/32");
  auto route = buildNullRoute(kRouteProtoId, network);
  auto before = nlSock->getAllRoutes().get().value();

  // Add drop route
  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // Check in Kernel
  // v4 blackhole route:
  // E.g. blackhole 10.120.175.0/24 proto gated/bgp
  auto after = nlSock->getAllRoutes().get().value();
  EXPECT_EQ(before.size() + 1, after.size());
  bool found = false;
  for (const auto& r : after) {
    if (r.getDestination() == network and r.getProtocolId() == kRouteProtoId and
        r.getNextHops().size() == 0 and r.getType() == RTN_BLACKHOLE) {
      found = true;
    }
  }
  EXPECT_TRUE(found);

  // Delete the routes
  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  after = nlSock->getAllRoutes().get().value();
  EXPECT_EQ(before.size(), after.size());
}

/*
 * Add an invalid IP route and verify it can't be programmed
 */
TEST_F(NlMessageFixture, InvalidIpRoute) {
  std::vector<openr::fbnl::NextHop> path;
  openr::fbnl::Route route;
  uint32_t ackCount{0};

  path.emplace_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ipAddrY1V6,
      ifIndexX,
      1 /* default weight */));
  route = buildRoute(
      kRouteProtoId, /* proto */
      std::nullopt, /* prefix */
      std::nullopt, /* mpls label */
      path /* nexthops */);

  ackCount = getAckCount();
  // family is by default AF_UNSPEC
  EXPECT_EQ(-EPROTONOSUPPORT, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_EQ(getAckCount(), ackCount);

  // family is by default AF_UNSPEC
  EXPECT_EQ(-EPROTONOSUPPORT, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount()); // ESRCH is ignored in error count
  EXPECT_EQ(getAckCount(), ackCount);
}

/*
 * Add IPv6 route with 1 next-hop and no labels
 */
TEST_F(NlMessageFixture, IPv6RouteSingleNextHop) {
  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;

  paths.emplace_back(buildNextHop(
      std::nullopt, std::nullopt, std::nullopt, ipAddrY1V6, ifIndexX));
  auto route = buildRoute(kRouteProtoId, ipPrefix1, std::nullopt, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify Netlink getAllRoutes
  auto kernelRoutes = nlSock->getIPv6Routes(kRouteProtoId).get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify v6 route is deleted
  kernelRoutes = nlSock->getIPv6Routes(kRouteProtoId).get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

/*
 * Add IPv6 route with 4 next-hops and no labels
 */
TEST_F(NlMessageFixture, IPv6RouteMultipleNextHops) {
  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;

  paths.emplace_back(buildNextHop(
      std::nullopt, std::nullopt, std::nullopt, ipAddrY1V6, ifIndexX, 1));
  paths.emplace_back(buildNextHop(
      std::nullopt, std::nullopt, std::nullopt, ipAddrY2V6, ifIndexX, 1));
  paths.emplace_back(buildNextHop(
      std::nullopt, std::nullopt, std::nullopt, ipAddrY3V6, ifIndexX, 1));
  paths.emplace_back(buildNextHop(
      std::nullopt, std::nullopt, std::nullopt, ipAddrY4V6, ifIndexX, 1));
  auto route = buildRoute(kRouteProtoId, ipPrefix1, std::nullopt, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify getAllRoutes
  auto kernelRoutes = nlSock->getIPv6Routes(kRouteProtoId).get().value();
  EXPECT_EQ(1, kernelRoutes.size());
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getIPv6Routes(kRouteProtoId).get().value();
  EXPECT_EQ(0, kernelRoutes.size());
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

/*
 * Add IPv4 route with 1 next-hop and no labels
 */
TEST_F(NlMessageFixture, IPv4RouteSingleNextHop) {
  uint32_t ackCount{0};
  folly::CIDRNetwork ipPrefix1V4 =
      folly::IPAddress::createNetwork("10.10.0.0/24");
  std::vector<openr::fbnl::NextHop> paths;

  paths.emplace_back(buildNextHop(
      std::nullopt, std::nullopt, std::nullopt, ipAddrY1V4, ifIndexY));
  auto route = buildRoute(kRouteProtoId, ipPrefix1V4, std::nullopt, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify getAllRoutes
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

/*
 * Add IPv4 route with 2 next-hops and no labels
 */
TEST_F(NlMessageFixture, IPv4RouteMultipleNextHops) {
  uint32_t ackCount{0};
  folly::CIDRNetwork ipPrefix1V4 =
      folly::IPAddress::createNetwork("10.10.0.0/24");
  std::vector<openr::fbnl::NextHop> paths;

  paths.emplace_back(buildNextHop(
      std::nullopt, std::nullopt, std::nullopt, ipAddrY1V4, ifIndexY, 1));
  paths.emplace_back(buildNextHop(
      std::nullopt, std::nullopt, std::nullopt, ipAddrY2V4, ifIndexY, 1));
  auto route = buildRoute(kRouteProtoId, ipPrefix1V4, std::nullopt, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify getAllRoutes
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify v4 route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

/*
 * Add IPv6 route with 1 next-hop with 1 push label next-hop
 */
TEST_F(NlMessageFixture, IPv6RouteLabelNexthop) {
  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;

  // create label next hop
  paths.emplace_back(buildNextHop(
      outLabel1, /* push labels */
      std::nullopt, /* swap labels */
      thrift::MplsActionCode::PUSH, /* MPLS action */
      ipAddrY1V6, /* gateway */
      ifIndexX /* ifIndex */));
  auto route = buildRoute(kRouteProtoId, ipPrefix1, std::nullopt, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify getAllRoutes
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

/*
 * Add IPv6 route with 48 push label next-hops
 */
TEST_F(NlMessageFixture, IpRouteMultipleLabelNextHops) {
  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;

  // create label next hop
  for (uint32_t i = 0; i < 48; i++) {
    outLabel6[0] = outLabel6[0] + 10 + i;
    paths.emplace_back(buildNextHop(
        outLabel6, /* push labels */
        std::nullopt, /* swap labels */
        thrift::MplsActionCode::PUSH, /* MPLS action */
        ipAddrY1V6, /* gateway */
        ifIndexX /* ifIndex */));
  }
  auto route = buildRoute(kRouteProtoId, ipPrefix5, std::nullopt, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify getAllRoutes for multiple label PUSH nexthops
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify v6 route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
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
        std::nullopt,
        thrift::MplsActionCode::PHP,
        ipAddress,
        ifIndexY));
  }

  auto route = buildRoute(kRouteProtoId, std::nullopt, inLabel4, paths);
  EXPECT_EQ(ENOBUFS, nlSock->addRoute(route).get());
}

TEST_F(NlMessageFixture, PopLabel) {
  // pop label to loopback i/f

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      thrift::MplsActionCode::POP_AND_LOOKUP,
      std::nullopt,
      ifIndexLo));
  auto route = buildRoute(kRouteProtoId, std::nullopt, inLabel3, paths);
  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for single POP nexthop
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, PopMultipleNextHops) {
  // pop labels to different interfaces

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      thrift::MplsActionCode::POP_AND_LOOKUP,
      std::nullopt,
      ifIndexLo));
  paths.push_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      thrift::MplsActionCode::POP_AND_LOOKUP,
      std::nullopt,
      ifIndexY));
  auto route = buildRoute(kRouteProtoId, std::nullopt, inLabel3, paths);
  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for single POP nexthop
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, LabelRouteLabelNexthop) {
  // Add label route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      std::nullopt,
      swapLabel,
      thrift::MplsActionCode::SWAP,
      ipAddrY1V6,
      ifIndexX));
  auto route = buildRoute(kRouteProtoId, std::nullopt, inLabel3, paths);
  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for single SWAP label nexthop
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, LabelRouteLabelNexthops) {
  // Add label route with multiple SWAP label nexthops
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      std::nullopt,
      swapLabel,
      thrift::MplsActionCode::SWAP,
      ipAddrY1V6,
      ifIndexX));

  paths.push_back(buildNextHop(
      std::nullopt,
      swapLabel1,
      thrift::MplsActionCode::SWAP,
      ipAddrY2V6,
      ifIndexX));

  auto route = buildRoute(kRouteProtoId, std::nullopt, inLabel3, paths);
  ackCount = getAckCount();
  // create label next hop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for muliple SWAP label nexthop
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, NlErrorMessage) {
  // Add label route with single path label next with one label
  // and an invalid outgoing I/F. Function should return Fail
  // and the nlmsg error should increase

  std::vector<openr::fbnl::NextHop> paths;
  uint32_t invalidIfindex = 1000;
  paths.push_back(buildNextHop(
      std::nullopt,
      swapLabel,
      thrift::MplsActionCode::SWAP,
      ipAddrY1V6,
      invalidIfindex));
  auto route = buildRoute(kRouteProtoId, std::nullopt, inLabel3, paths);
  EXPECT_EQ(0, getErrorCount());
  // create label next hop
  EXPECT_EQ(-ENODEV, nlSock->addRoute(route).get());
  EXPECT_NE(0, getErrorCount());
}

TEST_F(NlMessageFixture, InvalidMplsRoute) {
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
      std::nullopt,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V6,
      ifIndexX,
      1));
  routes.emplace_back(
      buildRoute(kRouteProtoId, ipPrefix1, std::nullopt, paths1));

  std::vector<openr::fbnl::NextHop> paths2;
  // Invalid route, send PUSH without labels
  paths2.push_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V6,
      ifIndexX,
      1));
  routes.emplace_back(
      buildRoute(kRouteProtoId, ipPrefix2, std::nullopt, paths2));

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
  auto kernelRoutes = nlSock->getIPv6Routes(kRouteProtoId).get().value();
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
  kernelRoutes = nlSock->getIPv6Routes(kRouteProtoId).get().value();
  EXPECT_EQ(0, kernelRoutes.size());
}

TEST_F(NlMessageFixture, LabelRouteV4Nexthop) {
  // Add label route with single path label with PHP nexthop

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      thrift::MplsActionCode::PHP,
      ipAddrY1V4,
      ifIndexY));
  auto route = buildRoute(kRouteProtoId, std::nullopt, inLabel5, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify getAllRoutes
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, LabelRouteAutoResolveInterfaceIndex) {
  // Add label route with single path label with PHP nexthop

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.emplace_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      thrift::MplsActionCode::PHP,
      ipAddrY1V4Peer,
      std::nullopt)); // NOTE: No interface index
  paths.emplace_back(buildNextHop(
      std::nullopt,
      swapLabel, // SWAP label
      thrift::MplsActionCode::SWAP,
      ipAddrX1V4Peer,
      std::nullopt)); // NOTE: No interface index
  auto route = buildRoute(kRouteProtoId, std::nullopt, inLabel5, paths);
  LOG(INFO) << "Adding route: " << route.str();
  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  //
  // Kernel will report next-hops with resolved interface index
  //
  std::vector<openr::fbnl::NextHop> resolvedPaths;
  resolvedPaths.emplace_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      thrift::MplsActionCode::PHP,
      ipAddrY1V4Peer,
      ifIndexY)); // NOTE: interface index should resolve to ifIndexY
  resolvedPaths.emplace_back(buildNextHop(
      std::nullopt,
      swapLabel, // SWAP label
      thrift::MplsActionCode::SWAP,
      ipAddrX1V4Peer,
      ifIndexX)); // NOTE: interface index should resolve to ifIndexX

  auto resolvedRoute =
      buildRoute(kRouteProtoId, std::nullopt, inLabel5, resolvedPaths);

  // verify that resolvedRoute shows up in kernel
  auto kernelRoutes = nlSock->getMplsRoutes(kRouteProtoId).get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, resolvedRoute));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getMplsRoutes(kRouteProtoId).get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, resolvedRoute));
}

TEST_F(NlMessageFixture, LabelRoutePHPNexthop) {
  // Add label route with single path label with PHP nexthop

  uint32_t ackCount{0};
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      thrift::MplsActionCode::PHP,
      ipAddrY1V6,
      ifIndexX));
  auto route1 = buildRoute(kRouteProtoId, std::nullopt, inLabel4, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route1).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for single hop MPLS PHP
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route1));

  paths.push_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      thrift::MplsActionCode::PHP,
      ipAddrY2V6,
      ifIndexX));

  auto route2 = buildRoute(kRouteProtoId, std::nullopt, inLabel4, paths);

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->addRoute(route2).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for multiple next hop MPLS PHP
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route2));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route2).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
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
      std::nullopt,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V4,
      ifIndexY,
      1));
  paths.push_back(buildNextHop(
      std::nullopt, std::nullopt, std::nullopt, ipAddrY2V4, ifIndexY, 1));
  auto route = buildRoute(kRouteProtoId, ipPrefix1V4, std::nullopt, paths);

  ackCount = getAckCount();
  // create ipv4 route with label nexthop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  // should have received one ack with status = 0
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for IPv4 nexthops
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

TEST_F(NlMessageFixture, IpV4RouteLabelNexthopAutoResolveInterface) {
  // Add IPv4 route with single path label next with one label
  // outoing IF is vethTestY

  uint32_t ackCount{0};
  folly::CIDRNetwork ipPrefix1V4 =
      folly::IPAddress::createNetwork("10.10.0.0/24");
  std::vector<openr::fbnl::NextHop> paths{buildNextHop(
      outLabel4,
      std::nullopt,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V4Peer,
      std::nullopt)}; // NOTE: No next-hop. We expect it to auto resolve.
  auto route = buildRoute(kRouteProtoId, ipPrefix1V4, std::nullopt, paths);

  ackCount = getAckCount();
  // create ipv4 route with label nexthop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  // should have received one ack with status = 0
  EXPECT_GE(getAckCount(), ackCount + 1);

  // Kernel will report next-hops with resolved interface index
  std::vector<openr::fbnl::NextHop> resolvedPaths{buildNextHop(
      outLabel4,
      std::nullopt,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V4Peer,
      ifIndexY)}; // NOTE: Kernel will resolve this next-hop
  auto resolvedRoute =
      buildRoute(kRouteProtoId, ipPrefix1V4, std::nullopt, resolvedPaths);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for IPv4 nexthops
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, resolvedRoute));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, resolvedRoute));
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
      labels,
      std::nullopt,
      thrift::MplsActionCode::PUSH,
      ipAddrY1V4,
      ifIndexY,
      1));
  paths.push_back(buildNextHop(
      std::nullopt, std::nullopt, std::nullopt, ipAddrY1V4, ifIndexY, 1));
  auto route = buildRoute(kRouteProtoId, ipPrefix1V4, std::nullopt, paths);

  ackCount = getAckCount();
  // create ipv4 route with label nexthop
  EXPECT_EQ(0, nlSock->addRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  // should have received one ack with status = 0
  EXPECT_GE(getAckCount(), ackCount + 1);

  LOG(INFO) << "Getting all routes...";
  // verify Netlink getAllRoutes for max 16 labels
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  ackCount = getAckCount();
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());
  EXPECT_EQ(0, getErrorCount());
  EXPECT_GE(getAckCount(), ackCount + 1);

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
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
  auto kernelRoutes = nlSock->getIPv4Routes(kRouteProtoId).get().value();
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
  kernelRoutes = nlSock->getIPv4Routes(kRouteProtoId).get().value();
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
  kernelRoutes = nlSock->getIPv4Routes(kRouteProtoId).get().value();
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
  kernelRoutes = nlSock->getIPv4Routes(kRouteProtoId).get().value();
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
      std::nullopt,
      swapLabel,
      thrift::MplsActionCode::SWAP,
      ipAddrY1V6,
      ifIndexX));
  std::vector<openr::fbnl::Route> labelRoutes;
  for (uint32_t i = 0; i < count; i++) {
    labelRoutes.push_back(
        buildRoute(kRouteProtoId, std::nullopt, 600 + i, paths));
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
  auto kernelRoutes = nlSock->getMplsRoutes(kRouteProtoId).get().value();
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
  kernelRoutes = nlSock->getMplsRoutes(kRouteProtoId).get().value();
  EXPECT_EQ(0, kernelRoutes.size());
}

/*
 * Flap multiple links up and down and stress test link events
 */
TEST_F(NlMessageFixture, LinkFlapScaleTest) {
  // Spawn RQueue to receive netlink event
  auto netlinkEventsReader = netlinkEventsQ.getReader();
  const int32_t linkCount{100};
  const int32_t flapCount{10};

  auto checkLinkEventCount = [&](int32_t cnt) {
    auto startTime = std::chrono::steady_clock::now();
    while (cnt > 0) {
      // check if it is beyond kProcTimeout
      auto endTime = std::chrono::steady_clock::now();
      if (endTime - startTime > kProcTimeout) {
        ASSERT_TRUE(0) << fmt::format(
            "Timeout receiving {} link event. Time limit: {}",
            cnt,
            kProcTimeout.count());
      }
      auto req = netlinkEventsReader.get(); // perform read
      ASSERT_TRUE(req.hasValue());
      // get_if returns `nullptr` if targeted variant is NOT populated
      if (auto* link = std::get_if<openr::fbnl::Link>(&req.value())) {
        --cnt;
      }
    }

    auto endTime = std::chrono::steady_clock::now();
    const auto elapsedTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            endTime - startTime)
            .count();

    VLOG(1) << "Took: " << elapsedTime << "ms to receive all link events";
  };

  // Create link paris to stress testing link-flapping
  for (int i = 0; i < linkCount; i++) {
    const std::string vethNameA{"vethTestA" + std::to_string(i)};
    const std::string vethNameB{"vethTestB" + std::to_string(i)};
    addIntfPair(vethNameA, vethNameB);
  }

  // Simulate flapping of linkCount of links
  for (int flap = 0; flap < flapCount; flap++) {
    // Bring up links
    for (int i = 0; i < linkCount; i++) {
      const std::string vethNameA{"vethTestA" + std::to_string(i)};
      const std::string vethNameB{"vethTestB" + std::to_string(i)};
      bringUpIntf(vethNameA);
      bringUpIntf(vethNameB);
    }
    // Bring down links
    for (int i = 0; i < linkCount; i++) {
      const std::string vethNameA{"vethTestA" + std::to_string(i)};
      const std::string vethNameB{"vethTestB" + std::to_string(i)};
      bringDownIntf(vethNameA);
      bringDownIntf(vethNameB);
    }
  }

  // Verify 2 * linkCount * flapCount (vethNameA + vethNameB) events
  checkLinkEventCount(2 * linkCount * flapCount);

  // Cleanup
  for (int i = 0; i < linkCount; i++) {
    const std::string vethNameA{"vethTestA" + std::to_string(i)};
    deleteIntfPair(vethNameA);
  }
}

/*
 * Add and remove 250 IPv4 and IPv6 addresses (total 500).
 * Verify addresses has been added/deleted from kernel.
 */
TEST_F(NlMessageFixture, AddrScaleTest) {
  const int addrCount{250};
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
  auto kernelAddresses = nlSock->getAllIfAddresses().get().value();
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
  kernelAddresses = nlSock->getAllIfAddresses().get().value();
  EXPECT_EQ(0, findAddressesInKernelAddresses(kernelAddresses, ifAddresses));
}

/*
 * Add 100 IPV6 neighbors and check if getAllNeighbors() API
 * returns all of the neighbors.
 */
TEST_F(NlMessageFixture, GetAllNeighborsV6) {
  const int countNeighbors{100};

  LOG(INFO) << "Adding " << countNeighbors << " test neighbors";
  for (int i = 0; i < countNeighbors; i++) {
    addV6NeighborEntry(
        kVethNameX,
        folly::IPAddress{"face:b00c::" + std::to_string(i)},
        kLinkAddr1);
  }

  LOG(INFO) << "Getting links and neighbors";
  auto links = nlSock->getAllLinks().get().value();
  auto neighbors = nlSock->getAllNeighbors().get().value();

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
  EXPECT_EQ(testNeighbors, countNeighbors);
  EXPECT_EQ(0, getErrorCount());

  LOG(INFO) << "Deleting " << countNeighbors << " test neighbors";
  for (int i = 0; i < countNeighbors; i++) {
    deleteV6NeighborEntry(
        kVethNameX,
        folly::IPAddress{"face:b00c::" + std::to_string(i)},
        kLinkAddr1);
  }

  // Check if getAllNeighbors do not return any reachable neighbors on
  // kVethNameX
  neighbors = nlSock->getAllNeighbors().get().value();
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

/*
 * Add 100 IPV4 neighbors and check if getAllNeighbors() API
 * returns all of the neighbors.
 */
TEST_F(NlMessageFixture, GetAllNeighborsV4) {
  const int countNeighbors{100};

  LOG(INFO) << "Adding " << countNeighbors << " test V4 neighbors";
  for (int i = 0; i < countNeighbors; i++) {
    addV4NeighborEntry(
        kVethNameX,
        folly::IPAddress{"172.8.0." + std::to_string(i)},
        kLinkAddr1);
  }

  LOG(INFO) << "Getting links and neighbors";
  auto links = nlSock->getAllLinks().get().value();
  auto neighbors = nlSock->getAllNeighbors().get().value();

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
  EXPECT_EQ(testNeighbors, countNeighbors);
  EXPECT_EQ(0, getErrorCount());

  LOG(INFO) << "Deleting " << countNeighbors << " test neighbors";
  for (int i = 0; i < countNeighbors; i++) {
    deleteV4NeighborEntry(
        kVethNameX,
        folly::IPAddress{"172.8.0." + std::to_string(i)},
        kLinkAddr1);
  }

  // Check if getAllNeighbors do not return any reachable neighbors on
  // kVethNameX
  neighbors = nlSock->getAllNeighbors().get().value();
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

/**
 * Verifies that MPLS UCMP returns expected error code (invalid argument)
 */
TEST_F(NlMessageFixture, MplsUcmpError) {
  //
  // POP next-hop
  //
  {
    std::vector<openr::fbnl::NextHop> paths{buildNextHop(
        std::nullopt,
        std::nullopt,
        thrift::MplsActionCode::POP_AND_LOOKUP,
        std::nullopt,
        std::nullopt,
        3 /* non-default weight */)};
    auto route = buildRoute(kRouteProtoId, std::nullopt, inLabel1, paths);
    EXPECT_EQ(-EINVAL, nlSock->addRoute(route).get());
  }

  //
  // PHP next-hop
  //
  {
    std::vector<openr::fbnl::NextHop> paths{buildNextHop(
        std::nullopt,
        std::nullopt,
        thrift::MplsActionCode::PHP,
        ipAddrX1V6,
        ifIndexY,
        3 /* non-default weight */)};
    auto route = buildRoute(kRouteProtoId, std::nullopt, inLabel1, paths);
    EXPECT_EQ(-EINVAL, nlSock->addRoute(route).get());
  }

  //
  // SWAP next-hop
  //
  {
    std::vector<openr::fbnl::NextHop> paths{buildNextHop(
        std::nullopt,
        swapLabel1,
        thrift::MplsActionCode::SWAP,
        ipAddrX1V6,
        ifIndexY,
        3 /* non-default weight */)};
    auto route = buildRoute(kRouteProtoId, std::nullopt, inLabel1, paths);
    EXPECT_EQ(-EINVAL, nlSock->addRoute(route).get());
  }
}

class NlMessageFixtureV4OrV6 : public NlMessageFixture,
                               public testing::WithParamInterface<bool> {};

/**
 * Validates the UCMP functionality with single next-hop. The kernel ignore the
 * weight and reports default weight (0 aka 1) as there is no effective UCMP for
 * route with single next-hop
 */
TEST_P(NlMessageFixtureV4OrV6, UcmpSingleNextHop) {
  const bool isV4 = GetParam();

  folly::CIDRNetwork ipPrefix = isV4
      ? folly::IPAddress::createNetwork("10.10.0.0/24")
      : folly::IPAddress::createNetwork("fd00::/64");

  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      outLabel4,
      std::nullopt,
      thrift::MplsActionCode::PUSH,
      isV4 ? ipAddrY1V4Peer : ipAddrX1V6,
      ifIndexY,
      3 /* non-default weight */));
  auto route = buildRoute(kRouteProtoId, ipPrefix, std::nullopt, paths);

  // Add route
  EXPECT_EQ(0, nlSock->addRoute(route).get());

  // Create expected route object
  std::vector<openr::fbnl::NextHop> expectedPaths;
  expectedPaths.push_back(buildNextHop(
      outLabel4,
      std::nullopt,
      thrift::MplsActionCode::PUSH,
      isV4 ? ipAddrY1V4Peer : ipAddrX1V6,
      ifIndexY,
      std::nullopt /* default weight */));
  auto expectedRoute =
      buildRoute(kRouteProtoId, ipPrefix, std::nullopt, expectedPaths);

  // verify Netlink getAllRoutes for IPv4 nexthops
  // NOTE: Weight is ignored
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, expectedRoute));

  // Delete the route
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, expectedRoute));
}

/**
 * Add multiple next-hops to IP route with different weights. Verify that
 * kernel accepts the route and also report the route back with the same
 * weight numbers.
 *
 * NOTE: Kernel doesn't normalize any weights
 */
TEST_P(NlMessageFixtureV4OrV6, UcmpMultipleNextHops) {
  const bool isV4 = GetParam();

  folly::CIDRNetwork ipPrefix = isV4
      ? folly::IPAddress::createNetwork("10.10.0.0/24")
      : folly::IPAddress::createNetwork("fd00::/64");

  // Path contains one MPLS push next-hop and another IP nexthop
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      outLabel4,
      std::nullopt,
      thrift::MplsActionCode::PUSH,
      isV4 ? ipAddrY1V4Peer : ipAddrX1V6,
      ifIndexY,
      3 /* non-default weight */));
  paths.push_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      std::nullopt,
      isV4 ? ipAddrX1V4Peer : ipAddrY1V6,
      ifIndexX,
      9 /* non-default weight */));
  auto route = buildRoute(kRouteProtoId, ipPrefix, std::nullopt, paths);

  // Add route
  EXPECT_EQ(0, nlSock->addRoute(route).get());

  // verify Netlink getAllRoutes for IPv4 nexthops
  // NOTE: Weight is reported as it. It is not normalized
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, route));

  // Delete the route
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, route));
}

/**
 * Same as above but now both nexthops have same weight (0 and 1).
 * NOTE: weight=0 and weight=1 are treated same as weight=1
 */
TEST_P(NlMessageFixtureV4OrV6, UcmpMultipleNextHopsDefaultWeight) {
  const bool isV4 = GetParam();

  folly::CIDRNetwork ipPrefix = isV4
      ? folly::IPAddress::createNetwork("10.10.0.0/24")
      : folly::IPAddress::createNetwork("fd00::/64");

  // Path contains one MPLS push next-hop and another IP nexthop
  std::vector<openr::fbnl::NextHop> paths;
  paths.push_back(buildNextHop(
      outLabel4,
      std::nullopt,
      thrift::MplsActionCode::PUSH,
      isV4 ? ipAddrY1V4Peer : ipAddrX1V6,
      ifIndexY,
      0 /* default-weight */));
  paths.push_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      std::nullopt,
      isV4 ? ipAddrX1V4Peer : ipAddrY1V6,
      ifIndexX,
      1 /* default weight */));
  auto route = buildRoute(kRouteProtoId, ipPrefix, std::nullopt, paths);

  // Add route
  EXPECT_EQ(0, nlSock->addRoute(route).get());

  // Create expected route. It contains both next-hops with weight=1
  std::vector<openr::fbnl::NextHop> expectedPaths;
  expectedPaths.push_back(buildNextHop(
      outLabel4,
      std::nullopt,
      thrift::MplsActionCode::PUSH,
      isV4 ? ipAddrY1V4Peer : ipAddrX1V6,
      ifIndexY,
      1 /* default-weight */));
  expectedPaths.push_back(buildNextHop(
      std::nullopt,
      std::nullopt,
      std::nullopt,
      isV4 ? ipAddrX1V4Peer : ipAddrY1V6,
      ifIndexX,
      1 /* default weight */));
  auto expectedRoute =
      buildRoute(kRouteProtoId, ipPrefix, std::nullopt, expectedPaths);

  // verify Netlink getAllRoutes for IPv4 nexthops
  // NOTE: Weight is reported as it. It is not normalized
  auto kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_TRUE(checkRouteInKernelRoutes(kernelRoutes, expectedRoute));

  // Delete the route
  EXPECT_EQ(0, nlSock->deleteRoute(route).get());

  // verify if route is deleted
  kernelRoutes = nlSock->getAllRoutes().get().value();
  EXPECT_FALSE(checkRouteInKernelRoutes(kernelRoutes, expectedRoute));
}

INSTANTIATE_TEST_CASE_P(
    NetlinkProtocolSocket, NlMessageFixtureV4OrV6, ::testing::Bool());

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
