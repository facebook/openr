/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <sodium.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Util.h>

using namespace std;
using namespace openr;

const auto prefix1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto prefix2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto prefix3 = toIpPrefix("::ffff:10.3.3.3/128");

const auto path1_2_1 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::2")), "iface_1_2_1", 1);
const auto path1_2_2 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::2")), "iface_1_2_2", 2);
const auto path1_2_3 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::2")), "iface_1_2_3", 1);
const auto path1_3_1 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::3")), "iface_1_3_1", 2);
const auto path1_3_2 =
    createPath(toBinaryAddress(folly::IPAddress("fe80::3")), "iface_1_3_2", 2);

// test getNthPrefix()
TEST(UtilTest, getNthPrefix) {
  // v6 allocation parameters
  const uint32_t seedPrefixLen = 32;
  const uint32_t allocPrefixLen = seedPrefixLen + 5;
  const folly::CIDRNetwork seedPrefix{
      folly::IPAddress{"face:b00c::1"},
      seedPrefixLen};

  // v6
  EXPECT_EQ(
      folly::CIDRNetwork(folly::IPAddress("face:b00c::"), allocPrefixLen),
      getNthPrefix(seedPrefix, allocPrefixLen, 0));
  EXPECT_EQ(
      folly::CIDRNetwork(folly::IPAddress("face:b00c:800::"), allocPrefixLen),
      getNthPrefix(seedPrefix, allocPrefixLen, 1));
  EXPECT_EQ(
      folly::CIDRNetwork(folly::IPAddress("face:b00c:1800::"), allocPrefixLen),
      getNthPrefix(seedPrefix, allocPrefixLen, 3));
  EXPECT_EQ(
      folly::CIDRNetwork(folly::IPAddress("face:b00c:f800::"), allocPrefixLen),
      getNthPrefix(seedPrefix, allocPrefixLen, 31));

  // v4
  const auto v4SeedPrefix = folly::IPAddress::createNetwork("10.1.0.0/16");
  EXPECT_EQ(
      folly::IPAddress::createNetwork("10.1.110.0/24"),
      getNthPrefix(v4SeedPrefix, 24, 110));
  EXPECT_EQ(
      folly::IPAddress::createNetwork("10.1.255.0/24"),
      getNthPrefix(v4SeedPrefix, 24, 255));
  EXPECT_EQ(
      folly::IPAddress::createNetwork("10.1.0.0/16"),
      getNthPrefix(v4SeedPrefix, 16, 0));

  // Some error cases
  // 1. prefixIndex is out of range
  EXPECT_THROW(
      getNthPrefix(v4SeedPrefix, 24, 256), std::invalid_argument);
  // 2. alloc block is bigger than seed prefix block
  EXPECT_THROW(
      getNthPrefix(v4SeedPrefix, 15, 0), std::invalid_argument);
}

TEST(UtilTest, checkIncludeExcludeRegex) {
  // Re2 support POSIX regular expressions by default, so we don't need
  // extended flag. It's much better than std::regex, so we don't need optimize
  // flag either
  re2::RE2::Options options;
  options.set_case_sensitive(false);
  auto includeRegexList =
      std::make_unique<re2::RE2::Set>(options, re2::RE2::ANCHOR_BOTH);
  std::string regexErr;
  includeRegexList->Add("eth.*", &regexErr);
  includeRegexList->Add("terra", &regexErr);
  includeRegexList->Add("po.*", &regexErr);
  EXPECT_TRUE(includeRegexList->Compile());
  auto excludeRegexList =
      std::make_unique<re2::RE2::Set>(options, re2::RE2::ANCHOR_BOTH);
  excludeRegexList->Add(".*pi.*", &regexErr);
  excludeRegexList->Add("^(po)[0-9]{3}$", &regexErr);
  EXPECT_TRUE(excludeRegexList->Compile());

  EXPECT_TRUE(
      checkIncludeExcludeRegex("eth", includeRegexList, excludeRegexList));
  EXPECT_TRUE(
      checkIncludeExcludeRegex("terra", includeRegexList, excludeRegexList));
  EXPECT_TRUE(
      checkIncludeExcludeRegex("eth1-2-3", includeRegexList, excludeRegexList));
  EXPECT_TRUE(
      checkIncludeExcludeRegex("Eth1-2-3", includeRegexList, excludeRegexList));
  EXPECT_FALSE(checkIncludeExcludeRegex(
      "helloterra", includeRegexList, excludeRegexList));
  EXPECT_FALSE(
      checkIncludeExcludeRegex("helloeth", includeRegexList, excludeRegexList));
  EXPECT_FALSE(checkIncludeExcludeRegex(
      "ethpihello", includeRegexList, excludeRegexList));
  EXPECT_FALSE(
      checkIncludeExcludeRegex("terr", includeRegexList, excludeRegexList));
  EXPECT_FALSE(
      checkIncludeExcludeRegex("hello", includeRegexList, excludeRegexList));
  EXPECT_FALSE(
      checkIncludeExcludeRegex("po101", includeRegexList, excludeRegexList));
  EXPECT_TRUE(
      checkIncludeExcludeRegex("po1010", includeRegexList, excludeRegexList));

  excludeRegexList.reset();
  EXPECT_TRUE(
      checkIncludeExcludeRegex("eth", includeRegexList, excludeRegexList));
  excludeRegexList =
      std::make_unique<re2::RE2::Set>(options, re2::RE2::ANCHOR_BOTH);
  excludeRegexList->Add(".*pi.*", &regexErr);
  excludeRegexList->Add("^(po)[0-9]{3}$", &regexErr);
  EXPECT_TRUE(excludeRegexList->Compile());
  includeRegexList.reset();
  EXPECT_FALSE(
      checkIncludeExcludeRegex("eth", includeRegexList, excludeRegexList));
}

TEST(UtilTest, createLoopbackAddr) {
  {
    auto network = folly::IPAddress::createNetwork("fc00::/64");
    auto addr = createLoopbackAddr(network);
    EXPECT_EQ(folly::IPAddress("fc00::1"), addr);
  }

  {
    auto network = folly::IPAddress::createNetwork("fc00::/128");
    auto addr = createLoopbackAddr(network);
    EXPECT_EQ(folly::IPAddress("fc00::"), addr);
  }

  {
    auto network = folly::IPAddress::createNetwork("fc00::1/128");
    auto addr = createLoopbackAddr(network);
    EXPECT_EQ(folly::IPAddress("fc00::1"), addr);
  }

  {
    auto network = folly::IPAddress::createNetwork("10.1.0.0/16");
    auto addr = createLoopbackAddr(network);
    EXPECT_EQ(folly::IPAddress("10.1.0.1"), addr);
  }

  {
    auto network = folly::IPAddress::createNetwork("10.1.0.0/32");
    auto addr = createLoopbackAddr(network);
    EXPECT_EQ(folly::IPAddress("10.1.0.0"), addr);
  }

  {
    auto network = folly::IPAddress::createNetwork("10.1.0.1/32");
    auto addr = createLoopbackAddr(network);
    EXPECT_EQ(folly::IPAddress("10.1.0.1"), addr);
  }
}

TEST(UtilTest, executeShellCommandTest) {
  EXPECT_EQ(0, executeShellCommand("ip link show"));
}

TEST(UtilTest, splitByCommaTest) {
  {
    std::string input{"ab"};
    auto actualOutput = splitByComma(input);
    std::vector<std::string> expectOutput{"ab"};
    EXPECT_EQ(actualOutput, expectOutput);
  }

  {
    std::string input{"ab,cd"};
    auto actualOutput = splitByComma(input);
    std::vector<std::string> expectOutput{"ab", "cd"};
    EXPECT_EQ(actualOutput, expectOutput);
  }

  {
    std::string input{"ab,cd, ef"};
    auto actualOutput = splitByComma(input);
    std::vector<std::string> expectOutput{"ab", "cd", " ef"};
    EXPECT_EQ(actualOutput, expectOutput);
  }

  {
    std::string input{""};
    auto actualOutput = splitByComma(input);
    std::vector<std::string> expectOutput{""};
    EXPECT_EQ(actualOutput, expectOutput);
  }
}

TEST(UtilTest, maskToPrefixLenV6Test) {
  {
    struct sockaddr_in6 mask;
    mask.sin6_addr.s6_addr[0] = (uint8_t)'\xFF';
    mask.sin6_addr.s6_addr[1] = (uint8_t)'\xC0';
    EXPECT_EQ(maskToPrefixLen(&mask), 10);
  }

  {
    struct sockaddr_in6 mask;
    mask.sin6_addr.s6_addr[0] = (uint8_t)'\xFE';
    EXPECT_EQ(maskToPrefixLen(&mask), 7);
  }

  {
    struct sockaddr_in6 mask;
    mask.sin6_addr.s6_addr[0] = (uint8_t)'\xFE';
    mask.sin6_addr.s6_addr[1] = (uint8_t)'\xFF';
    EXPECT_EQ(maskToPrefixLen(&mask), 7);
  }
}

TEST(UtilTest, maskToPrefixLenV4Test) {
  {
    struct sockaddr_in mask;
    mask.sin_addr.s_addr = 0xFFF00000;
    EXPECT_EQ(maskToPrefixLen(&mask), 12);
  }

  {
    struct sockaddr_in mask;
    mask.sin_addr.s_addr = 0xFE000000;
    EXPECT_EQ(maskToPrefixLen(&mask), 7);
  }

  {
    struct sockaddr_in mask;
    mask.sin_addr.s_addr = 0xC0000000;
    EXPECT_EQ(maskToPrefixLen(&mask), 2);
  }
}

TEST(UtilTest, addPerfEventTest) {
  {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, "node1", "LINK_UP");
    EXPECT_EQ(perfEvents.events.size(), 1);
    EXPECT_EQ(perfEvents.events[0].nodeName, "node1");
    EXPECT_EQ(perfEvents.events[0].eventDescr, "LINK_UP");
  }

  {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, "node1", "LINK_UP");
    addPerfEvent(perfEvents, "node2", "LINK_DOWN");
    EXPECT_EQ(perfEvents.events.size(), 2);
    EXPECT_EQ(perfEvents.events[0].nodeName, "node1");
    EXPECT_EQ(perfEvents.events[0].eventDescr, "LINK_UP");
    EXPECT_EQ(perfEvents.events[1].nodeName, "node2");
    EXPECT_EQ(perfEvents.events[1].eventDescr, "LINK_DOWN");
  }
}

TEST(UtilTest, sprintPerfEventsTest) {
  {
    thrift::PerfEvents perfEvents;
    auto outputStrs = sprintPerfEvents(perfEvents);
    EXPECT_EQ(outputStrs.size(), 0);
  }

  {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, "node1", "LINK_UP");
    addPerfEvent(perfEvents, "node2", "LINK_DOWN");
    auto outputStrs = sprintPerfEvents(perfEvents);
    EXPECT_EQ(outputStrs.size(), 2);
    EXPECT_EQ(outputStrs[0].find("node: node1, event: LINK_UP"), 0);
    EXPECT_EQ(outputStrs[1].find("node: node2, event: LINK_DOWN"), 0);
  }
}

TEST(UtilTest, getTotalPerfEventsDurationTest) {
  {
    thrift::PerfEvents perfEvents;
    auto duration = getTotalPerfEventsDuration(perfEvents);
    EXPECT_EQ(duration.count(), 0);
  }

  {
    thrift::PerfEvents perfEvents;
    thrift::PerfEvent event1{
      apache::thrift::FRAGILE, "node1", "LINK_UP", 100};
    perfEvents.events.emplace_back(std::move(event1));
    thrift::PerfEvent event2{
      apache::thrift::FRAGILE, "node1", "DECISION_RECVD", 200};
    perfEvents.events.emplace_back(std::move(event2));
    thrift::PerfEvent event3{
      apache::thrift::FRAGILE, "node1", "SPF_CALCULATE", 300};
    perfEvents.events.emplace_back(std::move(event3));
    auto duration = getTotalPerfEventsDuration(perfEvents);
    EXPECT_EQ(duration.count(), 200);
  }
}

TEST(UtilTest, getBestPaths) {
  auto bestPaths = getBestPaths({path1_2_1, path1_2_2});
  EXPECT_EQ(bestPaths.size(), 1);
  EXPECT_EQ(bestPaths.at(0).ifName, "iface_1_2_1");
  EXPECT_EQ(bestPaths.at(0).nextHop,
            toBinaryAddress(folly::IPAddress("fe80::2")));
  EXPECT_EQ(bestPaths.at(0).metric, 1);

  bestPaths = getBestPaths({path1_2_1, path1_2_2, path1_2_3});
  EXPECT_EQ(bestPaths.size(), 2);
  sort(bestPaths.begin(), bestPaths.end());
  EXPECT_EQ(bestPaths.at(0).ifName, "iface_1_2_1");
  EXPECT_EQ(bestPaths.at(0).nextHop,
            toBinaryAddress(folly::IPAddress("fe80::2")));
  EXPECT_EQ(bestPaths.at(0).metric, 1);
  EXPECT_EQ(bestPaths.at(1).ifName, "iface_1_2_3");
  EXPECT_EQ(bestPaths.at(1).nextHop,
            toBinaryAddress(folly::IPAddress("fe80::2")));
  EXPECT_EQ(bestPaths.at(1).metric, 1);
}

TEST(UtilTest, findDeltaRoutes) {
  thrift::RouteDatabase oldRouteDb;
  oldRouteDb.thisNodeName = "node-1";
  oldRouteDb.routes.emplace_back(
      thrift::Route(apache::thrift::FRAGILE, prefix2, {path1_2_1, path1_2_2}));

  thrift::RouteDatabase newRouteDb;
  newRouteDb.thisNodeName = "node-1";
  newRouteDb.routes.emplace_back(thrift::Route(
      apache::thrift::FRAGILE, prefix2, {path1_2_1, path1_2_2, path1_2_3}));

  const auto& res1 =
      findDeltaRoutes(std::move(newRouteDb), std::move(oldRouteDb));

  EXPECT_EQ(res1.first.size(), 1);
  EXPECT_EQ(res1.first, createUnicastRoutes(newRouteDb.routes));
  EXPECT_EQ(res1.second.size(), 0);

  // add more routes in newRouteDb
  newRouteDb.routes.emplace_back(
      thrift::Route(apache::thrift::FRAGILE, prefix3, {path1_3_1, path1_3_2}));

  const auto& res2 =
      findDeltaRoutes(std::move(newRouteDb), std::move(oldRouteDb));
  EXPECT_EQ(res2.first.size(), 2);
  EXPECT_EQ(res2.first, createUnicastRoutes(newRouteDb.routes));
  EXPECT_EQ(res2.second.size(), 0);

  // empty out newRouteDb
  newRouteDb.routes.clear();
  const auto& res3 =
      findDeltaRoutes(std::move(newRouteDb), std::move(oldRouteDb));
  EXPECT_EQ(res3.first.size(), 0);
  EXPECT_EQ(res3.second.size(), 1);
  EXPECT_EQ(res3.second.at(0), prefix2);
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // init sodium security library
  if (::sodium_init() == -1) {
    LOG(ERROR) << "Failed initializing sodium";
    return 1;
  }

  // Run the tests
  return RUN_ALL_TESTS();
}
