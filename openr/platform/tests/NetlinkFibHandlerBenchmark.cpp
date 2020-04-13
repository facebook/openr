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
#include <folly/Benchmark.h>
#include <folly/Exception.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/MacAddress.h>
#include <folly/Random.h>
#include <folly/Subprocess.h>
#include <folly/gen/Base.h>
#include <folly/init/Init.h>
#include <folly/system/Shell.h>
#include <folly/test/TestUtils.h>
#include <openr/fib/tests/PrefixGenerator.h>
#include <openr/nl/NetlinkSocket.h>
#include <openr/platform/NetlinkFibHandler.h>

extern "C" {
#include <net/if.h>
}

using namespace openr::fbnl;
using namespace folly::literals::shell_literals;

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
  NetlinkFibWrapper() {
    // cleanup old interfaces in any
    auto cmd = "ip link del {} 2>/dev/null"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    // Ignore result
    proc.wait();

    // add veth interface pair
    cmd = "ip link add {} type veth peer name {}"_shellify(
        kVethNameX.c_str(), kVethNameY.c_str());
    folly::Subprocess proc1(std::move(cmd));
    proc1.wait();

    addAddress(kVethNameX, "169.254.0.101");
    addAddress(kVethNameY, "169.254.0.102");

    // set interface status to up
    bringUpIntf(kVethNameX);
    bringUpIntf(kVethNameY);

    // Create NetlinkProtocolSocket
    std::unique_ptr<openr::fbnl::NetlinkProtocolSocket> nlProtocolSocket;
    nlProtocolSocket =
        std::make_unique<openr::fbnl::NetlinkProtocolSocket>(&evl2);
    nlProtocolSocketThread = std::thread([&]() {
      evl2.run();
      evl2.waitUntilStopped();
    });
    evl2.waitUntilRunning();

    // Create netlink route socket
    nlSocket = std::make_shared<NetlinkSocket>(
        &evl, nullptr, std::move(nlProtocolSocket));

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
    // cleanup virtual interfaces
    auto cmd = "ip link del {} 2>/dev/null"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    // Ignore result
    proc.wait();

    if (evl.isRunning()) {
      evl.stop();
      eventThread.join();
    }
    if (evl2.isRunning()) {
      evl2.stop();
      nlProtocolSocketThread.join();
    }

    nlSocket.reset();
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
    auto cmd =
        "ip addr add {} dev {}"_shellify(address.c_str(), ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    proc.wait();
  }

  static void
  bringUpIntf(const std::string& ifName) {
    auto cmd = "ip link set dev {} up"_shellify(ifName.c_str());
    folly::Subprocess proc(std::move(cmd));
    proc.wait();
  }
};

/**
 * Benchmark test to measure the time performance of NetlinkFibHandler
 * 1. Create a NetlinkFibHandler
 * 2. Generate random IpV6s and routes
 * 3. Add routes through netlink
 * 4. Wait until the completion of routes update
 */
static void
BM_NetlinkFibHandler(uint32_t iters, size_t numOfPrefixes) {
  auto suspender = folly::BenchmarkSuspender();
  auto netlinkFibWrapper = std::make_unique<NetlinkFibWrapper>();

  // Randomly generate IPV6 prefixes
  auto prefixes = netlinkFibWrapper->prefixGenerator.ipv6PrefixGenerator(
      numOfPrefixes, kBitMaskLen);

  suspender.dismiss(); // Start measuring benchmark time

  for (uint32_t i = 0; i < iters; i++) {
    auto routes = std::make_unique<std::vector<thrift::UnicastRoute>>();
    routes->reserve(prefixes.size());

    // Update routes by randomly regenerating nextHops for kDeltaSize prefixes.
    for (uint32_t index = 0; index < numOfPrefixes; index++) {
      routes->emplace_back(createUnicastRoute(
          prefixes[index],
          netlinkFibWrapper->prefixGenerator.getRandomNextHopsUnicast(
              kNumOfNexthops, kVethNameY)));
    }

    suspender.rehire(); // Stop measuring time again
    // Add new routes through netlink
    netlinkFibWrapper->fibHandler
        ->future_addUnicastRoutes(kFibId, std::move(routes))
        .wait();
  }
}

// The parameter is the number of prefixes
BENCHMARK_PARAM(BM_NetlinkFibHandler, 10);
BENCHMARK_PARAM(BM_NetlinkFibHandler, 100);
BENCHMARK_PARAM(BM_NetlinkFibHandler, 1000);
BENCHMARK_PARAM(BM_NetlinkFibHandler, 10000);

} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
