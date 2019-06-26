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

#include <benchmark/benchmark.h>
#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Exception.h>
#include <folly/Format.h>
#include <folly/MacAddress.h>
#include <folly/Random.h>
#include <folly/test/TestUtils.h>
#include <openr/fib/tests/PrefixGenerator.h>
#include <openr/nl/NetlinkSocket.h>
#include <openr/platform/NetlinkFibHandler.h>
#include <openr/platform/tests/NetlinkFibHandlerBenchmark.h>

extern "C" {
#include <net/if.h>
#include <netlink/route/link/veth.h>
#include <netlink/route/route.h>
#include <sys/ioctl.h>
}

using namespace openr::fbnl;

namespace {
// Virtual interfaces
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");
// Prefix length of a subnet
static const uint8_t kBitMaskLen = 128;
// Number of nexthops
const uint8_t kNumOfNexthops = 128;

} // namespace

namespace openr {

const int16_t kFibId{static_cast<int16_t>(thrift::FibClient::OPENR)};

// This class creates virtual interface (veths)
// which the Benchmark test can use to add routes (via interface)
class NetlinkFibWrapper {
 public:
  struct RouteCallbackContext {
    struct nl_cache* routeCache{nullptr};
    std::vector<Route> results;
  };

  struct AddressCallbackContext {
    struct nl_cache* linkeCache{nullptr};
    std::vector<IfAddress> results;
  };

  NetlinkFibWrapper() {
    // Allocate socket
    socket_ = nl_socket_alloc();
    nl_connect(socket_, NETLINK_ROUTE);
    rtnl_link_alloc_cache(socket_, AF_UNSPEC, &linkCache_);
    rtnl_addr_alloc_cache(socket_, &addrCache_);
    rtnl_route_alloc_cache(socket_, AF_UNSPEC, 0, &routeCache_);

    // Virtual interface and virtual link
    link_ = rtnl_link_veth_alloc();
    auto peerLink = rtnl_link_veth_get_peer(link_);
    rtnl_link_set_name(link_, kVethNameX.c_str());
    rtnl_link_set_name(peerLink, kVethNameY.c_str());
    nl_object_put(OBJ_CAST(peerLink));

    rtnl_link_add(socket_, link_, NLM_F_CREATE);

    nl_cache_refill(socket_, linkCache_);
    addAddress(kVethNameX, "169.254.0.101");
    addAddress(kVethNameY, "169.254.0.102");

    // set interface status to up
    bringUpIntf(kVethNameX);
    bringUpIntf(kVethNameY);

    // Create NetlinkProtocolSocket
    std::unique_ptr<openr::Netlink::NetlinkProtocolSocket> nlProtocolSocket;
    nlProtocolSocket =
        std::make_unique<openr::Netlink::NetlinkProtocolSocket>(&evl2);
    nlProtocolSocketThread = std::thread([&]() {
      nlProtocolSocket->init();
      evl2.run();
      evl2.waitUntilStopped();
    });
    evl2.waitUntilRunning();

    // Create netlink route socket
    nlSocket = std::make_shared<NetlinkSocket>(
        &evl, nullptr, true, std::move(nlProtocolSocket));

    // Run the zmq event loop in its own thread
    // We will either timeout if expected events are not received
    // or stop after we receive expected events
    eventThread = std::thread([&]() {
      evl.run();
      evl.waitUntilStopped();
    });
    evl.waitUntilRunning();

    // Start FibService thread
    fibHandler = std::make_shared<NetlinkFibHandler>(&evl, nlSocket);
  }

  ~NetlinkFibWrapper() {
    if (evl.isRunning()) {
      evl.stop();
      eventThread.join();
    }
    if (evl2.isRunning()) {
      evl2.stop();
      nlProtocolSocketThread.join();
    }

    nlSocket.reset();

    rtnl_link_delete(socket_, link_);
    nl_cache_free(linkCache_);
    nl_cache_free(addrCache_);
    nl_cache_free(routeCache_);
    nl_socket_free(socket_);
    rtnl_link_veth_release(link_);
  }

  fbzmq::Context context;
  std::shared_ptr<NetlinkSocket> nlSocket;
  fbzmq::ZmqEventLoop evl;
  fbzmq::ZmqEventLoop evl2;
  std::thread eventThread;
  std::thread nlProtocolSocketThread;
  std::shared_ptr<NetlinkFibHandler> fibHandler;
  PrefixGenerator prefixGenerator;

  struct rtnl_link* link_{nullptr};
  struct nl_sock* socket_{nullptr};
  struct nl_cache* linkCache_{nullptr};
  struct nl_cache* addrCache_{nullptr};
  struct nl_cache* routeCache_{nullptr};

 private:
  void
  addAddress(const std::string& ifName, const std::string& address) {
    int ifIndex = rtnl_link_name2i(linkCache_, ifName.c_str());

    auto addrMask = std::make_pair(folly::IPAddress(address), 16);
    struct nl_addr* nlAddr = nl_addr_build(
        addrMask.first.family(),
        (void*)addrMask.first.bytes(),
        addrMask.first.byteCount());
    nl_addr_set_prefixlen(nlAddr, addrMask.second);

    struct rtnl_addr* addr = rtnl_addr_alloc();
    rtnl_addr_set_local(addr, nlAddr);
    rtnl_addr_set_ifindex(addr, ifIndex);
    rtnl_addr_add(socket_, addr, 0);
    nl_addr_put(nlAddr);
    rtnl_addr_put(addr);
  }

  static void
  bringUpIntf(const std::string& ifName) {
    // Prepare socket
    auto sockFd = socket(PF_INET, SOCK_DGRAM, 0);

    // Prepare request
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    folly::strlcpy(ifr.ifr_name, ifName.c_str(), IFNAMSIZ);

    // Get existing flags
    ioctl(sockFd, SIOCGIFFLAGS, static_cast<void*>(&ifr));

    // Mutate flags and set them back
    ifr.ifr_flags |= IFF_UP;
    ioctl(sockFd, SIOCSIFFLAGS, static_cast<void*>(&ifr));
  }
};

void
BM_NetlinkFibHandler(benchmark::State& state) {
  /* Benchmark test to measure the time performance */
  auto netlinkFibWrapper = std::make_unique<NetlinkFibWrapper>();
  const uint32_t numOfPrefixes = state.range(0);

  // Randomly generate IPV6 prefixes
  auto prefixes = netlinkFibWrapper->prefixGenerator.ipv6PrefixGenerator(
      numOfPrefixes, kBitMaskLen);

  for (auto _ : state) {
    auto routes = std::make_unique<std::vector<thrift::UnicastRoute>>();
    routes->reserve(prefixes.size());

    // Update routes by randomly regenerating nextHops for kDeltaSize prefixes.
    for (auto index = 0; index < numOfPrefixes; index++) {
      routes->emplace_back(createUnicastRoute(
          prefixes[index],
          netlinkFibWrapper->prefixGenerator.getRandomNextHopsUnicast(
              kNumOfNexthops, kVethNameY)));
    }

    // Add new routes through netlink
    netlinkFibWrapper->fibHandler
        ->future_addUnicastRoutes(kFibId, std::move(routes))
        .wait();
  }
}

} // namespace openr
