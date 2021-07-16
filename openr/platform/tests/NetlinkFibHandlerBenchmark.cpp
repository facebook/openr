/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <map>
#include <memory>
#include <string>
#include <thread>

#include <folly/Benchmark.h>
#include <folly/Exception.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/MacAddress.h>
#include <folly/Random.h>
#include <folly/gen/Base.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/system/Shell.h>
#include <folly/test/TestUtils.h>

#include <openr/platform/NetlinkFibHandler.h>
#include <openr/tests/mocks/MockNetlinkProtocolSocket.h>
#include <openr/tests/mocks/PrefixGenerator.h>

using namespace openr::fbnl;

namespace {
// Virtual interfaces
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");
// Prefix length of a subnet
static const uint8_t kBitMaskLen = 128;
// Number of nexthops
const uint8_t kNumOfNexthops = 128;

const int16_t kFibId{static_cast<int16_t>(openr::thrift::FibClient::OPENR)};

} // namespace

namespace openr {

// This class creates virtual interface (veths)
// which the Benchmark test can use to add routes (via interface)
class NetlinkFibWrapper {
 public:
  NetlinkFibWrapper() {
    // Create NetlinkProtocolSocket
    nlSock = std::make_unique<MockNetlinkProtocolSocket>(&evb);
    nlSock->addLink(utils::createLink(0, kVethNameX)).get();
    nlSock->addLink(utils::createLink(1, kVethNameY)).get();

    // Start FibService thread
    fibHandler = std::make_unique<NetlinkFibHandler>(nlSock.get());
  }

  ~NetlinkFibWrapper() {
    fibHandler.reset();
    nlSock.reset();
  }

  folly::EventBase evb;
  std::unique_ptr<MockNetlinkProtocolSocket> nlSock;
  std::unique_ptr<NetlinkFibHandler> fibHandler;
  PrefixGenerator prefixGenerator;
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
        ->semifuture_addUnicastRoutes(kFibId, std::move(routes))
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
