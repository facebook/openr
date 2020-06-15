/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/common/Util.h>
#include <openr/decision/RibPolicy.h>

using namespace openr;

namespace {

thrift::RibPolicyStatement
createPolicyStatement(
    std::vector<thrift::IpPrefix> const& prefixes,
    int32_t defaultWeight,
    std::map<std::string, int32_t> areaToWeight) {
  thrift::RibPolicyStatement p;
  p.name = "TestPolicyStatement";
  p.matcher.prefixes_ref() = prefixes;
  p.action.set_weight_ref() = thrift::RibRouteActionWeight{};
  p.action.set_weight_ref()->default_weight = defaultWeight;
  p.action.set_weight_ref()->area_to_weight = areaToWeight;
  return p;
}

thrift::RibPolicy
createPolicy(
    std::vector<thrift::RibPolicyStatement> statements, int32_t ttl_secs) {
  thrift::RibPolicy policy;
  policy.statements = std::move(statements);
  policy.ttl_secs = ttl_secs;
  return policy;
}

} // namespace

TEST(RibPolicyStatement, Error) {
  // Create RibPolicyStatement with no action
  {
    thrift::RibPolicyStatement stmt;
    stmt.matcher.prefixes_ref() = std::vector<thrift::IpPrefix>{};
    EXPECT_THROW((RibPolicyStatement(stmt)), thrift::OpenrError);
  }

  // Create RibPolicyStatement with no matcher
  {
    thrift::RibPolicyStatement stmt;
    stmt.action.set_weight_ref() = thrift::RibRouteActionWeight{};
    EXPECT_THROW((RibPolicyStatement(stmt)), thrift::OpenrError);
  }
}

TEST(RibPolicy, Error) {
  // Create RibPolicy with no statements
  {
    thrift::RibPolicy policy; // NOTE: empty statements
    EXPECT_THROW((RibPolicy(policy)), thrift::OpenrError);
  }
}

TEST(RibPolicyStatement, ApplyAction) {
  const auto thriftPolicyStatement = createPolicyStatement(
      {toIpPrefix("fc00::/64")}, 1, {{"area1", 0}, {"area2", 2}});
  auto policyStatement = RibPolicyStatement(thriftPolicyStatement);

  const auto nhDefault =
      createNextHop(toBinaryAddress("fe80::1"), "iface-default");
  const auto nh1 = createNextHop(
      toBinaryAddress("fe80::1"), "iface1", 0, std::nullopt, false, "area1");
  const auto nh2 = createNextHop(
      toBinaryAddress("fe80::1"), "iface2", 0, std::nullopt, false, "area2");

  // Test with non matching prefix. Shouldn't be transformed.
  {
    const auto constRoute =
        createUnicastRoute(toIpPrefix("fd00::/64"), {nhDefault, nh1, nh2});
    auto route = constRoute; // Mutable copy

    EXPECT_FALSE(policyStatement.applyAction(route));
    EXPECT_EQ(constRoute, route); // Route shouldn't be modified
  }

  // Test with matching prefix. Expect transformed nexthops
  {
    const auto constRoute =
        createUnicastRoute(toIpPrefix("fc00::/64"), {nhDefault, nh1, nh2});
    auto route = constRoute; // Mutable copy

    EXPECT_TRUE(policyStatement.applyAction(route));

    // We should only see two next-hops. `area1` next-hop will be removed
    // because it's weight is set to `0`
    ASSERT_EQ(2, route.nextHops.size());

    auto nhDefaultModified = nhDefault;
    nhDefaultModified.weight = 1;
    EXPECT_EQ(nhDefaultModified, route.nextHops.at(0));

    auto nh2Modified = nh2;
    nh2Modified.weight = 2;
    EXPECT_EQ(nh2Modified, route.nextHops.at(1));
  }
}

TEST(RibPolicy, ApiTest) {
  const auto policyStatement =
      createPolicyStatement({toIpPrefix("10.0.0.0/8")}, 1, {{"test-area", 2}});
  const auto thriftPolicy = createPolicy({policyStatement}, 3);
  auto policy = RibPolicy(thriftPolicy);

  // Verify `toThrift()` API
  {
    auto thriftPolicyCopy = policy.toThrift();

    // Verify ttl. It must be less or equal
    EXPECT_LE(thriftPolicyCopy.ttl_secs, thriftPolicy.ttl_secs);

    // NOTE: Make ttl equal for comparing policy. Everything else
    // must be same
    thriftPolicyCopy.ttl_secs = thriftPolicy.ttl_secs;
    EXPECT_EQ(thriftPolicyCopy, thriftPolicy);
  }

  // Verify getTtlDuration(). Remaining time must be less or equal
  EXPECT_LE(policy.getTtlDuration().count(), thriftPolicy.ttl_secs * 1000);

  // Verify isActive()
  EXPECT_TRUE(policy.isActive());

  // Verify match
  {
    thrift::UnicastRoute route;
    route.dest = toIpPrefix("10.0.0.0/8");
    EXPECT_TRUE(policy.match(route));

    route.dest = toIpPrefix("99.0.0.0/8");
    EXPECT_FALSE(policy.match(route));
  }
}

TEST(RibPolicy, IsActive) {
  // Create policy with validity = 1 second
  const auto policyStatement =
      createPolicyStatement({toIpPrefix("10.0.0.0/8")}, 1, {});
  const auto thriftPolicy = createPolicy({policyStatement}, 1);
  auto policy = RibPolicy(thriftPolicy);

  // Policy is valid
  EXPECT_TRUE(policy.isActive());

  // Wait & check again. Should not be active anymore
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  EXPECT_FALSE(policy.isActive());
}

/**
 * Test intends to verify the apply action logic for policy. Only first
 * transformation is applied.
 */
TEST(RibPolicy, ApplyAction) {
  const auto stmt1 =
      createPolicyStatement({toIpPrefix("fc01::/64")}, 1, {{"area1", 99}});
  const auto stmt2 = createPolicyStatement(
      {toIpPrefix("fc00::/64"), toIpPrefix("fc02::/64")}, 1, {{"area2", 99}});
  auto policy = RibPolicy(createPolicy({stmt1, stmt2}, 1));

  const auto nh1 = createNextHop(
      toBinaryAddress("fe80::1"), "iface1", 0, std::nullopt, false, "area1");
  const auto nh2 = createNextHop(
      toBinaryAddress("fe80::1"), "iface2", 0, std::nullopt, false, "area2");

  // Apply policy on fc01::/64 (stmt1 gets applied)
  {
    const auto constRoute =
        createUnicastRoute(toIpPrefix("fc01::/64"), {nh1, nh2});
    auto route = constRoute; // Mutable copy

    EXPECT_TRUE(policy.applyAction(route));
    ASSERT_EQ(2, route.nextHops.size());
    EXPECT_EQ(99, route.nextHops.at(0).weight);
    EXPECT_EQ(1, route.nextHops.at(1).weight);
  }

  // Apply policy on fc02::/64 (stmt1 gets applied)
  {
    const auto constRoute =
        createUnicastRoute(toIpPrefix("fc02::/64"), {nh1, nh2});
    auto route = constRoute; // Mutable copy

    EXPECT_TRUE(policy.applyAction(route));
    ASSERT_EQ(2, route.nextHops.size());
    EXPECT_EQ(1, route.nextHops.at(0).weight);
    EXPECT_EQ(99, route.nextHops.at(1).weight);
  }

  // Apply policy on fc03::/64 (non matching prefix)
  {
    const auto constRoute =
        createUnicastRoute(toIpPrefix("fc03::/64"), {nh1, nh2});
    auto route = constRoute; // Mutable copy

    EXPECT_FALSE(policy.applyAction(route));
    EXPECT_EQ(constRoute, route); // Route shouldn't be modified
  }
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
