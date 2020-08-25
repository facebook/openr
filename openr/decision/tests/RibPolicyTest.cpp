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

#include <folly/IPAddress.h>

using namespace openr;

namespace {

thrift::RibPolicyStatement
createPolicyStatement(
    std::vector<thrift::IpPrefix> const& prefixes,
    int32_t defaultWeight,
    std::map<std::string, int32_t> areaToWeight) {
  thrift::RibPolicyStatement p;
  *p.name_ref() = "TestPolicyStatement";
  p.matcher_ref()->prefixes_ref() = prefixes;
  p.action_ref()->set_weight_ref() = thrift::RibRouteActionWeight{};
  p.action_ref()->set_weight_ref()->default_weight_ref() = defaultWeight;
  *p.action_ref()->set_weight_ref()->area_to_weight_ref() = areaToWeight;
  return p;
}

thrift::RibPolicy
createPolicy(
    std::vector<thrift::RibPolicyStatement> statements, int32_t ttl_secs) {
  thrift::RibPolicy policy;
  *policy.statements_ref() = std::move(statements);
  policy.ttl_secs_ref() = ttl_secs;
  return policy;
}

} // namespace

TEST(RibPolicyStatement, Error) {
  // Create RibPolicyStatement with no action
  {
    thrift::RibPolicyStatement stmt;
    stmt.matcher_ref()->prefixes_ref() = std::vector<thrift::IpPrefix>{};
    EXPECT_THROW((RibPolicyStatement(stmt)), thrift::OpenrError);
  }

  // Create RibPolicyStatement with no matcher
  {
    thrift::RibPolicyStatement stmt;
    stmt.action_ref()->set_weight_ref() = thrift::RibRouteActionWeight{};
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
    RibUnicastEntry entry(
        folly::IPAddress::createNetwork("fd00::/64"), {nhDefault, nh1, nh2});

    const auto constEntry = entry; // const copy

    EXPECT_FALSE(policyStatement.applyAction(entry));
    EXPECT_EQ(constEntry, entry); // Route shouldn't be modified
  }

  // Test with matching prefix. Expect transformed nexthops
  {
    RibUnicastEntry entry(
        folly::IPAddress::createNetwork("fc00::/64"), {nhDefault, nh1, nh2});

    EXPECT_TRUE(policyStatement.applyAction(entry));

    // We should only see two next-hops. `area1` next-hop will be removed
    // because it's weight is set to `0`
    ASSERT_EQ(2, entry.nexthops.size());

    auto nhDefaultModified = nhDefault;
    nhDefaultModified.weight_ref() = 1;

    auto nh2Modified = nh2;
    nh2Modified.weight_ref() = 2;

    EXPECT_THAT(
        entry.nexthops,
        testing::UnorderedElementsAre(nhDefaultModified, nh2Modified));
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
    EXPECT_LE(*thriftPolicyCopy.ttl_secs_ref(), *thriftPolicy.ttl_secs_ref());

    // NOTE: Make ttl equal for comparing policy. Everything else
    // must be same
    thriftPolicyCopy.ttl_secs_ref() = *thriftPolicy.ttl_secs_ref();
    EXPECT_EQ(thriftPolicyCopy, thriftPolicy);
  }

  // Verify getTtlDuration(). Remaining time must be less or equal
  EXPECT_LE(
      policy.getTtlDuration().count(), *thriftPolicy.ttl_secs_ref() * 1000);

  // Verify isActive()
  EXPECT_TRUE(policy.isActive());

  // Verify match
  {
    RibUnicastEntry matchEntry(folly::IPAddress::createNetwork("10.0.0.0/8"));
    EXPECT_TRUE(policy.match(matchEntry));

    RibUnicastEntry nonMatchEntry(
        folly::IPAddress::createNetwork("99.0.0.0/8"));
    EXPECT_FALSE(policy.match(nonMatchEntry));
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
    RibUnicastEntry entry(
        folly::IPAddress::createNetwork("fc01::/64"), {nh1, nh2});

    EXPECT_TRUE(policy.applyAction(entry));
    ASSERT_EQ(2, entry.nexthops.size());

    auto expectNh1 = nh1;
    expectNh1.weight_ref() = 99;

    auto expectNh2 = nh2;
    expectNh2.weight_ref() = 1;

    EXPECT_THAT(
        entry.nexthops, testing::UnorderedElementsAre(expectNh1, expectNh2));
  }

  // Apply policy on fc02::/64 (stmt1 gets applied)
  {
    RibUnicastEntry entry(
        folly::IPAddress::createNetwork("fc02::/64"), {nh1, nh2});

    EXPECT_TRUE(policy.applyAction(entry));
    ASSERT_EQ(2, entry.nexthops.size());

    auto expectNh1 = nh1;
    expectNh1.weight_ref() = 1;

    auto expectNh2 = nh2;
    expectNh2.weight_ref() = 99;

    EXPECT_THAT(
        entry.nexthops, testing::UnorderedElementsAre(expectNh1, expectNh2));
  }

  // Apply policy on fc03::/64 (non matching prefix)
  {
    RibUnicastEntry entry(
        folly::IPAddress::createNetwork("fc03::/64"), {nh1, nh2});

    const auto constEntry = entry; // const copy

    EXPECT_FALSE(policy.applyAction(entry));
    EXPECT_EQ(constEntry, entry); // Route shouldn't be modified
  }
}

TEST(RibPolicy, ApplyPolicy) {
  const auto stmt1 =
      createPolicyStatement({toIpPrefix("fc01::/64")}, 1, {{"area1", 99}});
  const auto stmt2 = createPolicyStatement(
      {toIpPrefix("fc00::/64"), toIpPrefix("fc02::/64")}, 1, {{"area2", 0}});
  auto policy = RibPolicy(createPolicy({stmt1, stmt2}, 1));

  const auto nh1 = createNextHop(
      toBinaryAddress("fe80::1"), "iface1", 0, std::nullopt, false, "area1");
  const auto nh2 = createNextHop(
      toBinaryAddress("fe80::1"), "iface2", 0, std::nullopt, false, "area2");

  RibUnicastEntry const entry1(
      folly::IPAddress::createNetwork("fc01::/64"), {nh1, nh2});
  RibUnicastEntry const entry2(
      folly::IPAddress::createNetwork("fc02::/64"), {nh2});
  {
    std::unordered_map<folly::CIDRNetwork, RibUnicastEntry> entries;
    entries.emplace(entry1.prefix, entry1);
    entries.emplace(entry2.prefix, entry2);

    auto const change = policy.applyPolicy(entries);

    EXPECT_THAT(
        change.updatedRoutes, testing::UnorderedElementsAre(entry1.prefix));
    EXPECT_THAT(
        change.deletedRoutes, testing::UnorderedElementsAre(entry2.prefix));

    EXPECT_THAT(entries, testing::SizeIs(1));

    auto expectNh1 = nh1;
    expectNh1.weight_ref() = 99;

    auto expectNh2 = nh2;
    expectNh2.weight_ref() = 1;
    EXPECT_THAT(
        entries.at(entry1.prefix).nexthops,
        testing::UnorderedElementsAre(expectNh1, expectNh2));
  }

  // wait for policy to expire and expect no changes
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(1));
  EXPECT_FALSE(policy.isActive());
  {
    std::unordered_map<folly::CIDRNetwork, RibUnicastEntry> entries;
    entries.emplace(entry1.prefix, entry1);
    entries.emplace(entry2.prefix, entry2);
    auto const change = policy.applyPolicy(entries);

    EXPECT_THAT(change.updatedRoutes, testing::IsEmpty());
    EXPECT_THAT(change.deletedRoutes, testing::IsEmpty());
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
