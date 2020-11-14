/*
 *  Copyright (c) 2013-present, Facebook, Inc.
 *  All rights reserved.
 */

#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>

#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/Random.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/system/ThreadName.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/common/NetworkUtil.h>
#include <openr/if/gen-cpp2/Platform_constants.h>
#include <openr/platform/NetlinkFibHandler.h>
#include <openr/tests/mocks/MockNetlinkProtocolSocket.h>

using namespace openr;

namespace {

const folly::StringPiece kPrefixV4{"192.168.{}.{}/32"};
const folly::StringPiece kPrefixV6{"fc00:cafe:{}::{}/128"};
const folly::StringPiece kNextHopV4{"169.254.0.{}"};
const folly::StringPiece kNextHopV6{"fe80::{}"};
const std::vector<std::string> kInterfaces{"eth0", "eth1", "eth2", "eth4"};

thrift::NextHopThrift
createNextHop(
    size_t index,
    bool isV4,
    std::optional<thrift::MplsAction> mplsAction = std::nullopt) {
  CHECK(kInterfaces.size());
  thrift::NextHopThrift nh;
  const auto& nhFormat = isV4 ? kNextHopV4 : kNextHopV6;
  *nh.address_ref() = toBinaryAddress(folly::sformat(nhFormat, 1 + index));
  nh.address_ref()->ifName_ref() =
      kInterfaces.at(folly::Random::rand32() % kInterfaces.size());
  nh.weight_ref() = 0;
  if (mplsAction.has_value()) {
    nh.mplsAction_ref() = mplsAction.value();
  }
  return nh;
}

std::vector<thrift::NextHopThrift>
createNextHops(
    size_t numNhs,
    bool isV4,
    std::optional<thrift::MplsAction> mplsAction = std::nullopt) {
  CHECK_LT(0, numNhs);
  CHECK_GT(256, numNhs);
  std::vector<thrift::NextHopThrift> nhs;
  for (size_t i = 0; i < numNhs; ++i) {
    nhs.emplace_back(createNextHop(i, isV4, mplsAction));
  }
  return nhs;
}

thrift::UnicastRoute
createUnicastRoute(size_t index, size_t numNhs, bool isV4) {
  CHECK_LT(0, numNhs); // Number of nexthops must be non-zero
  thrift::UnicastRoute route;
  const auto& format = isV4 ? kPrefixV4 : kPrefixV6;
  route.dest_ref() =
      toIpPrefix(folly::sformat(format, index / 255, 1 + index % 255));
  route.nextHops_ref() = createNextHops(numNhs, isV4);
  return route;
}

std::vector<thrift::UnicastRoute>
createUnicastRoutes(size_t numRoutes, bool isV4) {
  CHECK(kInterfaces.size());
  std::vector<thrift::UnicastRoute> routes;
  for (size_t i = 0; i < numRoutes; ++i) {
    size_t numNhs = 1 + folly::Random::rand32() % kInterfaces.size();
    routes.emplace_back(createUnicastRoute(i, numNhs, isV4));
  }
  return routes;
}

thrift::MplsRoute
createMplsRoute(
    size_t index,
    size_t numNhs,
    bool isV4,
    const thrift::MplsAction& mplsAction) {
  thrift::MplsRoute route;
  route.topLabel_ref() = index + 1;
  route.nextHops_ref() = createNextHops(numNhs, isV4, mplsAction);
  return route;
}

std::vector<thrift::MplsRoute>
createMplsRoutes(
    size_t numRoutes, bool isV4, const thrift::MplsAction& mplsAction) {
  CHECK(kInterfaces.size());
  std::vector<thrift::MplsRoute> routes;
  for (size_t i = 0; i < numRoutes; ++i) {
    size_t numNhs = 1 + folly::Random::rand32() % kInterfaces.size();
    routes.emplace_back(createMplsRoute(i, numNhs, isV4, mplsAction));
  }
  return routes;
}

void
sortNextHops(std::vector<thrift::NextHopThrift>& nexthops) {
  std::sort(nexthops.begin(), nexthops.end());
}

template <typename RouteType>
void
sortNextHops(std::vector<RouteType>& routes) {
  for (auto& route : routes) {
    sortNextHops(*route.nextHops_ref());
  }
}

} // namespace

/**
 * Base class for test fixture. Provides FibHandler for testing.
 * Internally creates an instance of MockNetlinkProtocolSocket for maintaining
 * in memory cache of routes added/removed. The whole testing is based on
 * public APIs of `NetlinkFibHandler`
 *
 * For unicast routes, boolean parameter indicates the type of prefix and its
 * nexthops, while in case of mpls routes it indicates the type of its nexthops
 */
class FibHandlerFixture : public testing::TestWithParam<bool> {
 public:
  void
  SetUp() override {
    // Add loopback interface with index=0
    ASSERT_EQ(
        0,
        nlSock_
            .addLink(
                fbnl::utils::createLink(0, "lo", true, true /* isLoopback */))
            .get());

    // Add interfaces to fake netlink with index starting at 1
    for (size_t i = 0; i < kInterfaces.size(); ++i) {
      ASSERT_EQ(
          0,
          nlSock_
              .addLink(fbnl::utils::createLink(
                  i + 1, kInterfaces.at(i), true, false))
              .get());
    }
  }

 private:
  // Intentionally keeping private to not expose in UTs
  folly::EventBase nlEvb_;
  fbnl::MockNetlinkProtocolSocket nlSock_{&nlEvb_};

 public:
  // FibHandler is accessible in UTs for testing
  NetlinkFibHandler handler{
      dynamic_cast<fbnl::NetlinkProtocolSocket*>(&nlSock_)};
};

//
// Tests static API - NetlinkFibHandler::getProtocol
// For mapping see `clientIdToProtocolId` in `Platform.thrift`
//
TEST(NetlinkFibHandler, getProtocol) {
  EXPECT_EQ(99, NetlinkFibHandler::getProtocol(786));
  EXPECT_EQ(253, NetlinkFibHandler::getProtocol(0));

  // Invalid clientId
  EXPECT_EQ(std::nullopt, NetlinkFibHandler::getProtocol(110));
}

//
// Tests static API - NetlinkFibHandler::getClientName
//
TEST(NetlinkFibHandler, getClientName) {
  EXPECT_EQ("OPENR", NetlinkFibHandler::getClientName(786));
  EXPECT_EQ("BGP", NetlinkFibHandler::getClientName(0));

  // Invalid clientId
  EXPECT_EQ("110", NetlinkFibHandler::getClientName(110));
}

//
// Tests static API - NetlinkFibHandler::protocolToPriority
//
TEST(NetlinkFibHandler, protocolToPriority) {
  EXPECT_EQ(
      10, NetlinkFibHandler::protocolToPriority(static_cast<uint8_t>(99)));
  EXPECT_EQ(
      20, NetlinkFibHandler::protocolToPriority(static_cast<uint8_t>(253)));

  // Invalid clientId
  EXPECT_EQ(
      thrift::Platform_constants::kUnknowProtAdminDistance(),
      NetlinkFibHandler::protocolToPriority(17));
}

//
// Get routes for invalid client ID
//
TEST(NetlinkFibHandler, getRoutesWithInvalidClient) {
  const int16_t kInvalidClient = 111;
  folly::EventBase evb;
  fbnl::MockNetlinkProtocolSocket nlSock(&evb);
  NetlinkFibHandler handler(&nlSock);

  EXPECT_THROW(
      handler.semifuture_getRouteTableByClient(kInvalidClient).get(),
      fbnl::NlException);
}

//
// Test correctness of route add, update, and remove
//
// Add a route - verify it gets added
// Add the same route again - verify it remains the same
//
// Update the route with a new nexthop
// Update the route with one less nexthop
//
// Del the route
// Del the route again - no effect
//
TEST_P(FibHandlerFixture, UnicastAddUpdateDel) {
  const int16_t kClientId = 786;
  const bool isV4 = GetParam();

  // Create route with two nexthops
  thrift::UnicastRoute r1 = createUnicastRoute(0, 2, isV4);

  // Expect no routes in the beginning
  auto routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(0, routes->size());

  // Add route and verify that it gets added
  handler
      .semifuture_addUnicastRoute(
          kClientId, std::make_unique<thrift::UnicastRoute>(r1))
      .get();
  routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(r1, routes->at(0));

  // Add same route again and verify no issues
  handler
      .semifuture_addUnicastRoute(
          kClientId, std::make_unique<thrift::UnicastRoute>(r1))
      .get();
  routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(r1, routes->at(0));

  // Add a nexthop - Expand ECMP group
  r1.nextHops_ref()->push_back(createNextHop(2 /* index */, isV4));
  handler
      .semifuture_addUnicastRoute(
          kClientId, std::make_unique<thrift::UnicastRoute>(r1))
      .get();
  routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(r1, routes->at(0));

  // Remove one nexthop - Shrink ECMP group
  r1.nextHops_ref()->pop_back();
  handler
      .semifuture_addUnicastRoute(
          kClientId, std::make_unique<thrift::UnicastRoute>(r1))
      .get();
  routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(r1, routes->at(0));

  // Update route with new nexthops (remove first nexthop, add 2 more nexthops)
  // NOTE: NetlinkTypes may organize nexthops in the arbitrary order
  *r1.nextHops_ref() = createNextHops(5, isV4);
  r1.nextHops_ref()->at(0) = r1.nextHops_ref()->at(4);
  r1.nextHops_ref()->pop_back();
  handler
      .semifuture_addUnicastRoute(
          kClientId, std::make_unique<thrift::UnicastRoute>(r1))
      .get();
  routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  sortNextHops(*r1.nextHops_ref());
  sortNextHops(*routes);
  EXPECT_EQ(r1, routes->at(0));

  // Remove route
  handler
      .semifuture_deleteUnicastRoute(
          kClientId, std::make_unique<thrift::IpPrefix>(*r1.dest_ref()))
      .get();
  routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  EXPECT_EQ(0, routes->size());

  // Remove route again
  handler
      .semifuture_deleteUnicastRoute(
          kClientId, std::make_unique<thrift::IpPrefix>(*r1.dest_ref()))
      .get();
  routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  EXPECT_EQ(0, routes->size());
}

//
// Add route with different weights. Make sure the code translates the weight
// and reads it again when working with fbnl data structures
//
TEST_P(FibHandlerFixture, UnicastAddUcmp) {
  const int16_t kClientId = 786;
  const bool isV4 = GetParam();

  // Create route with two nexthops
  thrift::UnicastRoute r1 = createUnicastRoute(0, 2, isV4);
  r1.nextHops_ref()->at(0).weight_ref() = 3;
  r1.nextHops_ref()->at(1).weight_ref() = 7;

  // Expect no routes in the beginning
  auto routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(0, routes->size());

  // Add route and verify that it gets added
  handler
      .semifuture_addUnicastRoute(
          kClientId, std::make_unique<thrift::UnicastRoute>(r1))
      .get();
  routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(r1, routes->at(0));
}

//
// Add/Get route with label push action
//
TEST_P(FibHandlerFixture, UnicastAddRouteWithLabelPush) {
  const int16_t kClientId = 786;
  const bool isV4 = GetParam();

  // Create route with one nexthop and add an MplsAction to it
  thrift::UnicastRoute r1 = createUnicastRoute(0, 1, isV4);
  r1.nextHops_ref()->at(0).mplsAction_ref() = createMplsAction(
      thrift::MplsActionCode::PUSH, std::nullopt, std::vector<int32_t>{2, 1});

  // Expect no routes in the beginning
  auto routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(0, routes->size());

  // Add route and verify that it gets added
  handler
      .semifuture_addUnicastRoute(
          kClientId, std::make_unique<thrift::UnicastRoute>(r1))
      .get();
  routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  EXPECT_EQ(r1, routes->at(0));
}

//
// Test correctness of SyncFib
//
// syncFib with [r1, r2, r3] routes - ensure all gets added
// syncFib with [r1, r2', r4] routes - ensure r2-update, r3-delete and r4-add
//
TEST_P(FibHandlerFixture, UnicastSync) {
  const int16_t kClientId = 786;
  const bool isV4 = GetParam();

  // Query routes and make sure it is empty
  auto routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(0, routes->size());

  // Create 4 routes and sync them
  auto rts = createUnicastRoutes(4, isV4);
  handler
      .semifuture_syncFib(
          kClientId, std::make_unique<std::vector<thrift::UnicastRoute>>(rts))
      .get();
  routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(4, routes->size());
  // NOTE: MockNetlinkSocket returns routes in sorted order
  sortNextHops(*routes);
  EXPECT_EQ(rts, *routes);

  // Create 6 routes and sync (NOTE: nexthops might differ)
  rts = createUnicastRoutes(6, isV4);
  handler
      .semifuture_syncFib(
          kClientId, std::make_unique<std::vector<thrift::UnicastRoute>>(rts))
      .get();
  routes = handler.semifuture_getRouteTableByClient(kClientId).get();
  ASSERT_EQ(6, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(rts, *routes);
}

//
// Test correctness of multiple client support. Incrementally add and remove
// route for same prefix1 from client1 and client2. Verify that addition or
// removal of routes for one client doesn't affect the other client.
//
TEST_P(FibHandlerFixture, UnicastMultipleClients) {
  const int16_t kClient1 = 786;
  const int16_t kClient2 = 0;
  const bool isV4 = GetParam();

  thrift::UnicastRoute c1r1 = createUnicastRoute(0, 1, isV4); // 1 nexthop
  thrift::UnicastRoute c2r1 = createUnicastRoute(0, 2, isV4); // 2 nexthop

  // Make sure prefix for both routes are same
  ASSERT_EQ(*c1r1.dest_ref(), *c2r1.dest_ref());

  // Expect no routes in the beginning for both clients
  {
    auto c1r = handler.semifuture_getRouteTableByClient(kClient1).get();
    auto c2r = handler.semifuture_getRouteTableByClient(kClient2).get();
    ASSERT_EQ(0, c1r->size());
    ASSERT_EQ(0, c2r->size());
  }

  // Add c1r1
  {
    handler
        .semifuture_addUnicastRoute(
            kClient1, std::make_unique<thrift::UnicastRoute>(c1r1))
        .get();

    auto c1r = handler.semifuture_getRouteTableByClient(kClient1).get();
    auto c2r = handler.semifuture_getRouteTableByClient(kClient2).get();
    ASSERT_EQ(1, c1r->size());
    ASSERT_EQ(0, c2r->size());

    sortNextHops(*c1r);
    EXPECT_EQ(c1r1, c1r->at(0));
  }

  // Add c2r1
  {
    handler
        .semifuture_addUnicastRoute(
            kClient2, std::make_unique<thrift::UnicastRoute>(c2r1))
        .get();

    auto c1r = handler.semifuture_getRouteTableByClient(kClient1).get();
    auto c2r = handler.semifuture_getRouteTableByClient(kClient2).get();
    ASSERT_EQ(1, c1r->size());
    ASSERT_EQ(1, c2r->size());

    sortNextHops(*c1r);
    EXPECT_EQ(c1r1, c1r->at(0));

    sortNextHops(*c2r);
    EXPECT_EQ(c2r1, c2r->at(0));
  }

  // Remove c1r1 (ensure c2r1 remains)
  {
    handler
        .semifuture_deleteUnicastRoute(
            kClient1, std::make_unique<thrift::IpPrefix>(*c1r1.dest_ref()))
        .get();

    auto c1r = handler.semifuture_getRouteTableByClient(kClient1).get();
    auto c2r = handler.semifuture_getRouteTableByClient(kClient2).get();
    ASSERT_EQ(0, c1r->size());
    ASSERT_EQ(1, c2r->size());

    sortNextHops(*c2r);
    EXPECT_EQ(c2r1, c2r->at(0));
  }

  // Remove c2r1 (ensure both are gone)
  {
    handler
        .semifuture_deleteUnicastRoute(
            kClient2, std::make_unique<thrift::IpPrefix>(*c2r1.dest_ref()))
        .get();

    auto c1r = handler.semifuture_getRouteTableByClient(kClient1).get();
    auto c2r = handler.semifuture_getRouteTableByClient(kClient2).get();
    ASSERT_EQ(0, c1r->size());
    ASSERT_EQ(0, c2r->size());
  }
}

//
// Add and Remove for POP label
//
TEST_P(FibHandlerFixture, MplsAddDelPop) {
  const int16_t kClientId = 786;

  thrift::NextHopThrift popNh;
  *popNh.address_ref() = toBinaryAddress(folly::IPAddressV6("::"));
  popNh.mplsAction_ref() =
      createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP);
  thrift::MplsRoute r1;
  r1.topLabel_ref() = 1;
  r1.nextHops_ref()->emplace_back(popNh);

  // Add route with pop action
  {
    std::vector<thrift::MplsRoute> routes{r1};
    handler
        .semifuture_addMplsRoutes(
            kClientId, std::make_unique<std::vector<thrift::MplsRoute>>(routes))
        .get();

    auto rts = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
    ASSERT_EQ(1, rts->size());
    EXPECT_EQ(r1, rts->at(0));
  }

  // Delete route
  {
    std::vector<int32_t> labels{*r1.topLabel_ref()};
    handler
        .semifuture_deleteMplsRoutes(
            kClientId, std::make_unique<std::vector<int32_t>>(labels))
        .get();

    auto rts = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
    EXPECT_EQ(0, rts->size());
  }
}

TEST_P(FibHandlerFixture, MplsAddUpdateDelSwapPhp) {
  const int16_t kClientId = 786;
  const bool isV4 = GetParam();

  const auto swapAction = createMplsAction(thrift::MplsActionCode::SWAP, 10);
  const auto phpAction = createMplsAction(thrift::MplsActionCode::PHP);

  auto r1 = createMplsRoute(0, 2, isV4, swapAction); // 2 nexthops
  const std::vector<int32_t> labels{*r1.topLabel_ref()};

  // Expect no routes in the beginning
  auto routes = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
  ASSERT_EQ(0, routes->size());

  // Add route and verify that it gets added
  handler
      .semifuture_addMplsRoutes(
          kClientId,
          std::make_unique<std::vector<thrift::MplsRoute>>(
              std::vector<thrift::MplsRoute>{r1}))
      .get();
  routes = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(r1, routes->at(0));

  // Add same route again and verify no issues
  handler
      .semifuture_addMplsRoutes(
          kClientId,
          std::make_unique<std::vector<thrift::MplsRoute>>(
              std::vector<thrift::MplsRoute>{r1}))
      .get();
  routes = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(r1, routes->at(0));

  // Add a nexthop - Expand ECMP group (with PHP action)
  r1.nextHops_ref()->push_back(createNextHop(2 /* index */, isV4, phpAction));
  handler
      .semifuture_addMplsRoutes(
          kClientId,
          std::make_unique<std::vector<thrift::MplsRoute>>(
              std::vector<thrift::MplsRoute>{r1}))
      .get();
  routes = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(r1, routes->at(0));

  // Remove one nexthop - Shrink ECMP group
  r1.nextHops_ref()->pop_back();
  handler
      .semifuture_addMplsRoutes(
          kClientId,
          std::make_unique<std::vector<thrift::MplsRoute>>(
              std::vector<thrift::MplsRoute>{r1}))
      .get();
  routes = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(r1, routes->at(0));

  // Update route with new nexthops (remove first nexthop, add 2 more nexthops)
  // NOTE: NetlinkTypes may organize nexthops in the arbitrary order
  *r1.nextHops_ref() = createNextHops(5, isV4, phpAction);
  handler
      .semifuture_addMplsRoutes(
          kClientId,
          std::make_unique<std::vector<thrift::MplsRoute>>(
              std::vector<thrift::MplsRoute>{r1}))
      .get();
  routes = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
  ASSERT_EQ(1, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(r1, routes->at(0));

  // Remove route
  handler
      .semifuture_deleteMplsRoutes(
          kClientId, std::make_unique<std::vector<int32_t>>(labels))
      .get();
  routes = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
  EXPECT_EQ(0, routes->size());

  // Remove route again
  handler
      .semifuture_deleteMplsRoutes(
          kClientId, std::make_unique<std::vector<int32_t>>(labels))
      .get();
  routes = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
  EXPECT_EQ(0, routes->size());
}

TEST_P(FibHandlerFixture, MplsSync) {
  const int16_t kClientId = 786;
  const bool isV4 = GetParam();
  const auto swapAction = createMplsAction(thrift::MplsActionCode::SWAP, 10);

  // Query routes and make sure it is empty
  auto routes = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
  ASSERT_EQ(0, routes->size());

  // Create 4 routes and sync them
  auto rts = createMplsRoutes(4, isV4, swapAction);
  handler
      .semifuture_syncMplsFib(
          kClientId, std::make_unique<std::vector<thrift::MplsRoute>>(rts))
      .get();
  routes = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
  ASSERT_EQ(4, routes->size());
  // NOTE: MockNetlinkSocket returns routes in sorted order
  sortNextHops(*routes);
  EXPECT_EQ(rts, *routes);

  // Create 6 routes and sync (NOTE: nexthops might differ)
  rts = createMplsRoutes(6, isV4, swapAction);
  handler
      .semifuture_syncMplsFib(
          kClientId, std::make_unique<std::vector<thrift::MplsRoute>>(rts))
      .get();
  routes = handler.semifuture_getMplsRouteTableByClient(kClientId).get();
  ASSERT_EQ(6, routes->size());
  sortNextHops(*routes);
  EXPECT_EQ(rts, *routes);
}

TEST_P(FibHandlerFixture, MplsMultipleClient) {
  const int16_t kClient1 = 786;
  const int16_t kClient2 = 0;
  const bool isV4 = GetParam();

  const auto action1 = createMplsAction(thrift::MplsActionCode::SWAP, 10);
  const auto action2 = createMplsAction(thrift::MplsActionCode::PHP);

  thrift::MplsRoute c1r1 = createMplsRoute(0, 1, isV4, action1);
  thrift::MplsRoute c2r1 = createMplsRoute(0, 2, isV4, action2);

  // Make sure prefix for both routes are same
  ASSERT_EQ(*c1r1.topLabel_ref(), *c2r1.topLabel_ref());

  // Expect no routes in the beginning for both clients
  {
    auto c1r = handler.semifuture_getMplsRouteTableByClient(kClient1).get();
    auto c2r = handler.semifuture_getMplsRouteTableByClient(kClient2).get();
    ASSERT_EQ(0, c1r->size());
    ASSERT_EQ(0, c2r->size());
  }

  // Add c1r1
  {
    handler
        .semifuture_addMplsRoutes(
            kClient1,
            std::make_unique<std::vector<thrift::MplsRoute>>(
                std::vector<thrift::MplsRoute>{c1r1}))
        .get();

    auto c1r = handler.semifuture_getMplsRouteTableByClient(kClient1).get();
    auto c2r = handler.semifuture_getMplsRouteTableByClient(kClient2).get();
    ASSERT_EQ(1, c1r->size());
    ASSERT_EQ(0, c2r->size());

    sortNextHops(*c1r);
    EXPECT_EQ(c1r1, c1r->at(0));
  }

  // Add c2r1
  {
    handler
        .semifuture_addMplsRoutes(
            kClient2,
            std::make_unique<std::vector<thrift::MplsRoute>>(
                std::vector<thrift::MplsRoute>{c2r1}))
        .get();

    auto c1r = handler.semifuture_getMplsRouteTableByClient(kClient1).get();
    auto c2r = handler.semifuture_getMplsRouteTableByClient(kClient2).get();
    ASSERT_EQ(1, c1r->size());
    ASSERT_EQ(1, c2r->size());

    sortNextHops(*c1r);
    EXPECT_EQ(c1r1, c1r->at(0));

    sortNextHops(*c2r);
    EXPECT_EQ(c2r1, c2r->at(0));
  }

  // Remove c1r1 (ensure c2r1 remains)
  {
    const std::vector<int32_t> labels{*c1r1.topLabel_ref()};
    handler
        .semifuture_deleteMplsRoutes(
            kClient1, std::make_unique<std::vector<int32_t>>(labels))
        .get();

    auto c1r = handler.semifuture_getMplsRouteTableByClient(kClient1).get();
    auto c2r = handler.semifuture_getMplsRouteTableByClient(kClient2).get();
    ASSERT_EQ(0, c1r->size());
    ASSERT_EQ(1, c2r->size());

    sortNextHops(*c2r);
    EXPECT_EQ(c2r1, c2r->at(0));
  }

  // Remove c2r1 (ensure both are gone)
  {
    const std::vector<int32_t> labels{*c2r1.topLabel_ref()};
    handler
        .semifuture_deleteMplsRoutes(
            kClient2, std::make_unique<std::vector<int32_t>>(labels))
        .get();

    auto c1r = handler.semifuture_getMplsRouteTableByClient(kClient1).get();
    auto c2r = handler.semifuture_getMplsRouteTableByClient(kClient2).get();
    ASSERT_EQ(0, c1r->size());
    ASSERT_EQ(0, c2r->size());
  }
}

//
// instantiate parameterized tests
//
INSTANTIATE_TEST_CASE_P(Netlink, FibHandlerFixture, testing::Bool());

int
main(int argc, char* argv[]) {
  // Init
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);

  return RUN_ALL_TESTS();
}
