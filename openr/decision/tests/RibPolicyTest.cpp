/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>

#include <fb303/ServiceData.h>
#include <folly/IPAddress.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/common/Util.h>
#include <openr/decision/RibPolicy.h>

using namespace openr;

namespace {

thrift::RibPolicyStatement
createPolicyStatement(
    std::optional<std::vector<thrift::IpPrefix>> const& prefixes,
    std::optional<std::vector<std::string>> const& tags,
    int32_t defaultWeight,
    std::map<std::string, int32_t> areaToWeight,
    std::map<std::string, int32_t> nbrToWeight = {}) {
  thrift::RibPolicyStatement p;
  *p.name_ref() = "TestPolicyStatement";
  p.matcher_ref()->prefixes_ref().from_optional(prefixes);
  p.matcher_ref()->tags_ref().from_optional(tags);
  p.action_ref()->set_weight_ref() = thrift::RibRouteActionWeight{};
  p.action_ref()->set_weight_ref()->default_weight_ref() = defaultWeight;
  *p.action_ref()->set_weight_ref()->area_to_weight_ref() = areaToWeight;
  *p.action_ref()->set_weight_ref()->neighbor_to_weight_ref() = nbrToWeight;
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
  std::vector<thrift::IpPrefix> prefixes{toIpPrefix("fc00::/64")};
  const auto thriftPolicyStatement = createPolicyStatement(
      prefixes, std::nullopt, 1, {{"area1", 0}, {"area2", 2}});
  auto policyStatement = RibPolicyStatement(thriftPolicyStatement);

  const auto nhDefault =
      createNextHop(toBinaryAddress("fe80::1"), "iface-default");
  const auto nh1 = createNextHop(
      toBinaryAddress("fe80::1"), "iface1", 0, std::nullopt, "area1");
  const auto nh2 = createNextHop(
      toBinaryAddress("fe80::1"), "iface2", 0, std::nullopt, "area2");

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

TEST(RibPolicyStatement, Match) {
  // Statement with only prefix matcher
  {
    std::vector<thrift::IpPrefix> prefixes{toIpPrefix("10.0.0.0/8")};
    auto thriftStatement =
        createPolicyStatement(prefixes, std::nullopt, 1, {{"test-area", 2}});
    auto policyStatement = RibPolicyStatement(thriftStatement);

    // Verify match
    RibUnicastEntry match(folly::IPAddress::createNetwork("10.0.0.0/8"));
    match.bestPrefixEntry.tags_ref()->insert("COMMODITY:EGRESS");
    EXPECT_TRUE(policyStatement.match(match));

    // Verify no match
    RibUnicastEntry noMatch(folly::IPAddress::createNetwork("11.0.0.0/8"));
    noMatch.bestPrefixEntry.tags_ref()->insert("COMMODITY:EGRESS");
    EXPECT_FALSE(policyStatement.match(noMatch));
  }

  // Statement with only tag matcher
  {
    std::vector<std::string> tags{"COMMODITY:EGRESS"};
    auto thriftStatement =
        createPolicyStatement(std::nullopt, tags, 1, {{"test-area", 2}});
    auto policyStatement = RibPolicyStatement(thriftStatement);

    // Verify match
    RibUnicastEntry match(folly::IPAddress::createNetwork("11.0.0.0/8"));
    match.bestPrefixEntry.tags_ref()->insert("COMMODITY:EGRESS");
    EXPECT_TRUE(policyStatement.match(match));

    // Verify no match
    RibUnicastEntry noMatch(folly::IPAddress::createNetwork("11.0.0.0/8"));
    noMatch.bestPrefixEntry.tags_ref()->insert("COMMODITY:INGRESS:pod1");
    EXPECT_FALSE(policyStatement.match(noMatch));
  }

  // Statement with both prefix and tag matchers
  {
    std::vector<thrift::IpPrefix> prefixes{toIpPrefix("10.0.0.0/8")};
    std::vector<std::string> tags{"COMMODITY:EGRESS"};
    auto thriftStatement =
        createPolicyStatement(prefixes, tags, 1, {{"test-area", 2}});
    auto policyStatement = RibPolicyStatement(thriftStatement);

    // Verify match
    RibUnicastEntry match(folly::IPAddress::createNetwork("10.0.0.0/8"));
    match.bestPrefixEntry.tags_ref()->insert("COMMODITY:EGRESS");
    EXPECT_TRUE(policyStatement.match(match));

    // Verify prefix doesn't match
    RibUnicastEntry noMatchPrefix(
        folly::IPAddress::createNetwork("11.0.0.0/8"));
    noMatchPrefix.bestPrefixEntry.tags_ref()->insert("COMMODITY:EGRESS");
    EXPECT_FALSE(policyStatement.match(noMatchPrefix));

    // Verify tag doesn't match
    RibUnicastEntry noMatchTag(folly::IPAddress::createNetwork("10.0.0.0/8"));
    noMatchTag.bestPrefixEntry.tags_ref()->insert("COMMODITY:INGRESS:pod1");
    EXPECT_FALSE(policyStatement.match(noMatchTag));

    // Verify tag doesn't match
    RibUnicastEntry noMatchPrefixTag(
        folly::IPAddress::createNetwork("11.0.0.0/8"));
    noMatchPrefixTag.bestPrefixEntry.tags_ref()->insert(
        "COMMODITY:INGRES:pod1");
    EXPECT_FALSE(policyStatement.match(noMatchPrefixTag));
  }

  // Statement no prefix or tag matchers
  {
    std::vector<thrift::IpPrefix> prefixes{};
    std::vector<std::string> tags{};
    auto thriftStatement =
        createPolicyStatement(prefixes, tags, 1, {{"test-area", 2}});
    auto policyStatement = RibPolicyStatement(thriftStatement);

    // Statement will not match with anything
    RibUnicastEntry noMatch(folly::IPAddress::createNetwork("10.0.0.0/8"));
    noMatch.bestPrefixEntry.tags_ref()->insert("COMMODITY:EGRESS");
    EXPECT_FALSE(policyStatement.match(noMatch));
  }
}

TEST(RibPolicy, ApiTest) {
  std::vector<thrift::IpPrefix> prefixes{toIpPrefix("10.0.0.0/8")};
  std::vector<std::string> tags{"TAG1"};
  const auto policyStatement =
      createPolicyStatement(prefixes, tags, 1, {{"test-area", 2}});
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
    matchEntry.bestPrefixEntry.tags_ref()->insert("TAG1");
    EXPECT_TRUE(policy.match(matchEntry));

    RibUnicastEntry nonMatchEntry(
        folly::IPAddress::createNetwork("99.0.0.0/8"));
    nonMatchEntry.bestPrefixEntry.tags_ref()->insert("TAG1");
    EXPECT_FALSE(policy.match(nonMatchEntry));
  }
}

TEST(RibPolicy, IsActive) {
  // Create policy with validity = 1 second
  std::vector<thrift::IpPrefix> prefixes{toIpPrefix("10.0.0.0/8")};
  const auto policyStatement =
      createPolicyStatement(prefixes, std::nullopt, 1, {});
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
  std::vector<thrift::IpPrefix> prefixes1{toIpPrefix("fc01::/64")};
  const auto stmt1 =
      createPolicyStatement(prefixes1, std::nullopt, 1, {{"area1", 99}});
  std::vector<thrift::IpPrefix> prefixes2{
      toIpPrefix("fc00::/64"), toIpPrefix("fc02::/64")};
  const auto stmt2 =
      createPolicyStatement(prefixes2, std::nullopt, 1, {{"area2", 99}});
  auto policy = RibPolicy(createPolicy({stmt1, stmt2}, 1));

  const auto nh1 = createNextHop(
      toBinaryAddress("fe80::1"), "iface1", 0, std::nullopt, "area1");
  const auto nh2 = createNextHop(
      toBinaryAddress("fe80::1"), "iface2", 0, std::nullopt, "area2");

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
  std::vector<thrift::IpPrefix> prefixes1{toIpPrefix("fc01::/64")};
  const auto stmt1 = createPolicyStatement(
      prefixes1, std::nullopt, 1, {{"area1", 99}}, {{"nbr3", 98}});
  std::vector<thrift::IpPrefix> prefixes2{
      toIpPrefix("fc00::/64"), toIpPrefix("fc02::/64")};
  const auto stmt2 =
      createPolicyStatement(prefixes2, std::nullopt, 1, {{"area2", 0}});
  auto policy = RibPolicy(createPolicy({stmt1, stmt2}, 1));

  const auto nh1 = createNextHop(
      toBinaryAddress("fe80::1"), "iface1", 0, std::nullopt, "area1", "nbr1");
  const auto nh2 = createNextHop(
      toBinaryAddress("fe80::1"), "iface2", 0, std::nullopt, "area2", "nbr2");
  const auto nh3 = createNextHop(
      toBinaryAddress("fe80::1"), "iface3", 0, std::nullopt, "area1", "nbr3");

  RibUnicastEntry const entry1(
      folly::IPAddress::createNetwork("fc01::/64"), {nh1, nh2, nh3});
  RibUnicastEntry const entry2(
      folly::IPAddress::createNetwork("fc02::/64"), {nh2});
  {
    std::unordered_map<folly::CIDRNetwork, RibUnicastEntry> entries;
    entries.emplace(entry1.prefix, entry1);
    entries.emplace(entry2.prefix, entry2);

    auto const change = policy.applyPolicy(entries);

    EXPECT_THAT(
        change.updatedRoutes, testing::UnorderedElementsAre(entry1.prefix));
    EXPECT_TRUE(change.deletedRoutes.empty());
    auto counters = facebook::fb303::fbData->getCounters();
    EXPECT_EQ(1, counters.at("decision.rib_policy.invalidated_routes.count"));

    EXPECT_THAT(entries, testing::SizeIs(2));

    auto expectNh1 = nh1;
    expectNh1.weight_ref() = 99;

    auto expectNh2 = nh2;
    expectNh2.weight_ref() = 1;

    auto expectNh3 = nh3;
    expectNh3.weight_ref() = 98;

    EXPECT_THAT(
        entries.at(entry1.prefix).nexthops,
        testing::UnorderedElementsAre(expectNh1, expectNh2, expectNh3));
    EXPECT_EQ(entries.at(entry2.prefix).nexthops, entry2.nexthops);
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
