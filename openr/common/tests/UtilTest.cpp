/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>

#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <sodium.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>

using namespace std;
using namespace openr;
using FwdType = openr::thrift::PrefixForwardingType;
using FwdAlgo = openr::thrift::PrefixForwardingAlgorithm;

const auto prefix1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto prefix2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto prefix3 = toIpPrefix("::ffff:10.3.3.3/128");

const auto path1_2_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_1"),
    1);
const auto path1_2_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_2"),
    2);
const auto path1_2_3 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_3"),
    3);
const auto path1_3_1 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_1"),
    1);
const auto path1_3_2 = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_2"),
    2);

const auto path1_2_1_swap = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_1"),
    1,
    createMplsAction(thrift::MplsActionCode::SWAP, 1));
const auto path1_2_2_swap = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_2"),
    2,
    createMplsAction(thrift::MplsActionCode::SWAP, 1));
const auto path1_2_3_swap = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_3"),
    3,
    createMplsAction(thrift::MplsActionCode::SWAP, 1));
const auto path1_3_1_swap = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_1"),
    1,
    createMplsAction(thrift::MplsActionCode::SWAP, 1));
const auto path1_3_2_swap = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_2"),
    2,
    createMplsAction(thrift::MplsActionCode::SWAP, 1));

const auto path1_2_1_php = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_1"),
    1,
    createMplsAction(thrift::MplsActionCode::PHP));
const auto path1_2_2_php = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_2"),
    2,
    createMplsAction(thrift::MplsActionCode::PHP));
const auto path1_2_3_php = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_3"),
    3,
    createMplsAction(thrift::MplsActionCode::PHP));
const auto path1_3_1_php = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_1"),
    1,
    createMplsAction(thrift::MplsActionCode::PHP));
const auto path1_3_2_php = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::3")),
    std::string("iface_1_3_2"),
    2,
    createMplsAction(thrift::MplsActionCode::PHP));

const auto path1_2_2_pop = createNextHop(
    toBinaryAddress(folly::IPAddress("fe80::2")),
    std::string("iface_1_2_1"),
    2,
    createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP));

struct PrefixKeyEntry {
  bool shouldPass{false};
  std::string pkey{};
  std::string node{};
  folly::CIDRNetwork ipaddr;
  std::string area{thrift::KvStore_constants::kDefaultArea()};
  thrift::IpPrefix ipPrefix;
  folly::IPAddress addr;
  int plen{0};
};

TEST(UtilTest, NetworkUtilTest) {
  folly::IPAddress v4{"192.168.0.2"};
  folly::IPAddress v6{"fe80::2"};

  EXPECT_EQ(v4.str(), toString(toBinaryAddress(v4)));
  EXPECT_EQ(v6.str(), toString(toBinaryAddress(v6)));

  thrift::BinaryAddress empty;
  EXPECT_EQ("", toString(empty));
}

TEST(UtilTest, PrefixKeyTest) {
  std::vector<PrefixKeyEntry> strToItems;

  // tests for parsing prefix key

  // prefix key
  PrefixKeyEntry k1;

  // this should fail, all parameters expected to be std::nullopt
  k1.pkey = "prefix:[ff00::1]";
  k1.shouldPass = false;
  strToItems.push_back(k1);

  // this should fail, all parameters expected to be std::nullopt
  k1.pkey = "prefix:node-name:e.1:[ff00::1/100]";
  k1.shouldPass = false;
  strToItems.push_back(k1);

  // this should fail, all parameters expected to be std::nullopt
  k1.pkey = "prefix:node_name:a:[ff00::1/a]";
  k1.shouldPass = false;
  strToItems.push_back(k1);

  // this should fail, all parameters expected to be std::nullopt
  k1.pkey = "prefix:nodename:0:0:[ff00::1/129]";
  k1.shouldPass = false;
  strToItems.push_back(k1);

  // this should pass
  k1.pkey = "prefix:nodename0:0:[ff00::1/128]";
  k1.node = "nodename0";
  k1.ipaddr = folly::IPAddress::createNetwork("ff00::1/128");
  k1.area = "0";
  k1.ipPrefix = toIpPrefix("ff00::1/128");
  k1.shouldPass = true;
  strToItems.push_back(k1);

  // this should pass
  k1.pkey = "prefix:nodename0:pod108:[ff00::0/64]";
  k1.node = "nodename0";
  k1.ipaddr = folly::IPAddress::createNetwork("ff00::0/64");
  k1.area = "pod108";
  k1.ipPrefix = toIpPrefix("ff00::0/64");
  k1.shouldPass = true;
  strToItems.push_back(k1);

  // this should fail
  k1.pkey = "prefix:nodename0:pod-108:[ff00::0/64]";
  k1.shouldPass = false;
  strToItems.push_back(k1);

  // this should fail, all paremeters expected to by std::nullopt
  k1.pkey = "prefix:nodename.0.0:3:[ff00::1/2334]";
  k1.shouldPass = false;
  strToItems.push_back(k1);

  // this should fail, all paremeters expected to by std::nullopt
  k1.pkey = "prefix:nodename.0.0:33:[192.168.0.1/343]";
  k1.shouldPass = false;
  strToItems.push_back(k1);

  // this should pass
  k1.pkey = "prefix:nodename.0.0:10:[192.168.0.0/3]";
  k1.node = "nodename.0.0";
  k1.ipaddr = folly::IPAddress::createNetwork("192.168.0.0/3");
  k1.area = "10";
  k1.ipPrefix = toIpPrefix("192.168.0.0/3");
  k1.shouldPass = true;
  strToItems.push_back(k1);

  // this should pass
  k1.pkey = "prefix:nodename.0.0:99:[::0/0]";
  k1.node = "nodename.0.0";
  k1.ipaddr = folly::IPAddress::createNetwork("::0/0");
  k1.area = "99";
  k1.ipPrefix = toIpPrefix("::0/0");
  k1.shouldPass = true;
  strToItems.push_back(k1);

  // this should pass
  k1.pkey = "prefix:nodename.0.0:10:[0.0.0.0/19]";
  k1.node = "nodename.0.0";
  k1.ipaddr = folly::IPAddress::createNetwork("0.0.0.0/19");
  k1.area = "10";
  k1.ipPrefix = toIpPrefix("0.0.0.0/19");
  k1.shouldPass = true;
  strToItems.push_back(k1);

  // this should fail
  k1.pkey = "prefix:nodename.0.0:10:99.0:[0.0.0.0/19]";
  k1.shouldPass = false;
  strToItems.push_back(k1);

  // this should fail
  k1.pkey = "prefix:nodename.0.0:99-h:[0.0.0.0/19]";
  k1.shouldPass = false;
  strToItems.push_back(k1);

  for (const auto& keys : strToItems) {
    auto prefixStr = PrefixKey::fromStr(keys.pkey);
    if (keys.shouldPass) {
      EXPECT_EQ(prefixStr.value().getNodeName(), keys.node);
      EXPECT_EQ(prefixStr.value().getCIDRNetwork(), keys.ipaddr);
      EXPECT_EQ(prefixStr.value().getPrefixArea(), keys.area);
      EXPECT_EQ(prefixStr.value().getIpPrefix(), keys.ipPrefix);
    } else {
      EXPECT_FALSE(prefixStr.hasValue());
    }
  }

  // tests for forming prefix key
  std::vector<PrefixKeyEntry> itemsToStr;

  PrefixKeyEntry k2;

  k2.node = "ebb.0.0";
  k2.addr = folly::IPAddress("ff00::0");
  k2.plen = 16;
  k2.area = "1";
  k2.pkey = "prefix:ebb.0.0:1:[ff00::/16]";
  itemsToStr.push_back(k2);

  // address will be masked
  k2.node = "ebb-0-0";
  k2.addr = folly::IPAddress("ff00::1");
  k2.plen = 16;
  k2.area = "01";
  k2.pkey = "prefix:ebb-0-0:01:[ff00::/16]";
  itemsToStr.push_back(k2);

  // address will be masked
  k2.node = "ebb-0-0";
  k2.addr = folly::IPAddress("192.168.0.1");
  k2.plen = 16;
  k2.area = "1";
  k2.pkey = "prefix:ebb-0-0:1:[192.168.0.0/16]";
  itemsToStr.push_back(k2);

  for (const auto& keys : itemsToStr) {
    auto ipaddress = folly::IPAddress::createNetwork(
        folly::sformat("{}/{}", keys.addr.str(), keys.plen));
    auto prefixStr = PrefixKey(keys.node, ipaddress, keys.area);
    EXPECT_EQ(prefixStr.getPrefixKey(), keys.pkey);
  }
}

TEST(UtilTest, GetNodeNameFromKeyTest) {
  const std::unordered_map<std::string, std::string> expectedIo = {
      {"prefix:node1", "node1"},
      {"prefix:nodename.0.0:10:[0.0.0.0/0]", "nodename.0.0"},
      {"", ""},
      {"adj:", ""},
      {"adj", ""}};
  for (auto const& io : expectedIo) {
    EXPECT_EQ(getNodeNameFromKey(io.first), io.second);
  }
}

// test getNthPrefix()
TEST(UtilTest, getNthPrefix) {
  // v6 allocation parameters
  const uint32_t seedPrefixLen = 32;
  const uint32_t allocPrefixLen = seedPrefixLen + 5;
  const folly::CIDRNetwork seedPrefix{folly::IPAddress{"face:b00c::1"},
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
  EXPECT_THROW(getNthPrefix(v4SeedPrefix, 24, 256), std::invalid_argument);
  // 2. alloc block is bigger than seed prefix block
  EXPECT_THROW(getNthPrefix(v4SeedPrefix, 15, 0), std::invalid_argument);
}

TEST(UtilTest, checkIncludeExcludeRegex) {
  // Re2 support POSIX regular expressions by default, so we don't need
  // extended flag. It's much better than std::regex, so we don't need optimize
  // flag either
  re2::RE2::Options options;
  options.set_case_sensitive(false);
  auto includeRegexList =
      std::make_shared<re2::RE2::Set>(options, re2::RE2::ANCHOR_BOTH);
  std::string regexErr;
  includeRegexList->Add("eth.*", &regexErr);
  includeRegexList->Add("terra", &regexErr);
  includeRegexList->Add("po.*", &regexErr);
  EXPECT_TRUE(includeRegexList->Compile());
  auto excludeRegexList =
      std::make_shared<re2::RE2::Set>(options, re2::RE2::ANCHOR_BOTH);
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
      std::make_shared<re2::RE2::Set>(options, re2::RE2::ANCHOR_BOTH);
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
    thrift::PerfEvent event1{apache::thrift::FRAGILE, "node1", "LINK_UP", 100};
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

TEST(UtilTest, getDurationBetweenPerfEventsTest) {
  {
    thrift::PerfEvents perfEvents;
    auto maybeDuration =
        getDurationBetweenPerfEvents(perfEvents, "LINK_UP", "SPF_CALCULATE");
    EXPECT_TRUE(maybeDuration.hasError());
  }

  {
    thrift::PerfEvents perfEvents;
    thrift::PerfEvent event1{apache::thrift::FRAGILE, "node1", "LINK_UP", 100};
    perfEvents.events.emplace_back(std::move(event1));
    thrift::PerfEvent event2{
        apache::thrift::FRAGILE, "node1", "DECISION_RECVD", 200};
    perfEvents.events.emplace_back(std::move(event2));
    thrift::PerfEvent event3{
        apache::thrift::FRAGILE, "node1", "SPF_CALCULATE", 300};
    perfEvents.events.emplace_back(std::move(event3));
    auto maybeDuration =
        getDurationBetweenPerfEvents(perfEvents, "LINK_UP", "SPF_CALCULATE");
    EXPECT_EQ(maybeDuration.value().count(), 200);
    maybeDuration = getDurationBetweenPerfEvents(
        perfEvents, "DECISION_RECVD", "SPF_CALCULATE");
    EXPECT_EQ(maybeDuration.value().count(), 100);
    maybeDuration = getDurationBetweenPerfEvents(
        perfEvents, "NO_SUCH_NAME", "SPF_CALCULATE");
    EXPECT_TRUE(maybeDuration.hasError());
    maybeDuration = getDurationBetweenPerfEvents(
        perfEvents, "SPF_CALCULATE", "DECISION_RECVD");
    EXPECT_TRUE(maybeDuration.hasError());
    maybeDuration = getDurationBetweenPerfEvents(
        perfEvents, "DECISION_RECVD", "NO_SUCH_NAME");
    EXPECT_TRUE(maybeDuration.hasError());
  }
}

TEST(UtilTest, getBestNextHopsUnicast) {
  auto bestNextHops = getBestNextHopsUnicast({path1_2_1, path1_2_2});
  EXPECT_EQ(bestNextHops.size(), 1);
  EXPECT_EQ(bestNextHops.at(0).address.ifName_ref(), "iface_1_2_1");
  EXPECT_EQ(
      bestNextHops.at(0).address.addr,
      toBinaryAddress(folly::IPAddress("fe80::2")).addr);
  EXPECT_EQ(bestNextHops.at(0).metric, 1);

  bestNextHops =
      getBestNextHopsUnicast({path1_2_1, path1_2_2, path1_2_3, path1_3_1});
  EXPECT_EQ(
      bestNextHops, std::vector<thrift::NextHopThrift>({path1_2_1, path1_3_1}));

  auto path1_2_2_updated = path1_2_2;
  path1_2_2_updated.useNonShortestRoute = true;
  bestNextHops = getBestNextHopsUnicast(
      {path1_2_1, path1_2_2_updated, path1_2_3, path1_3_1});
  EXPECT_EQ(
      bestNextHops,
      std::vector<thrift::NextHopThrift>(
          {path1_2_1, path1_2_2_updated, path1_3_1}));
}

TEST(UtilTest, getBestNextHopsMpls) {
  // Validate pop route
  auto bestNextHops = getBestNextHopsMpls({path1_2_2_pop});
  EXPECT_EQ(bestNextHops, std::vector<thrift::NextHopThrift>({path1_2_2_pop}));

  // Validate all swap routes (choose min metric)
  bestNextHops = getBestNextHopsMpls({
      path1_2_1_swap,
      path1_2_2_swap,
      path1_2_3_swap,
      path1_3_1_swap,
      path1_3_2_swap,
  });
  EXPECT_EQ(
      bestNextHops,
      std::vector<thrift::NextHopThrift>({path1_2_1_swap, path1_3_1_swap}));

  // Validate all php routes (choose min metric)
  bestNextHops = getBestNextHopsMpls({
      path1_2_1_php,
      path1_2_2_php,
      path1_2_3_php,
      path1_3_1_php,
      path1_3_2_php,
  });
  EXPECT_EQ(
      bestNextHops,
      std::vector<thrift::NextHopThrift>({path1_2_1_php, path1_3_1_php}));

  // Choose min metric in mix of swap and php
  bestNextHops = getBestNextHopsMpls(
      {path1_2_1_swap, path1_2_2_php, path1_3_1_swap, path1_3_2_php});
  EXPECT_EQ(
      bestNextHops,
      std::vector<thrift::NextHopThrift>({path1_2_1_swap, path1_3_1_swap}));

  // Choose min metric in mix of swap and php
  bestNextHops = getBestNextHopsMpls(
      {path1_2_1_php, path1_2_2_swap, path1_3_1_php, path1_3_2_swap});
  EXPECT_EQ(
      bestNextHops,
      std::vector<thrift::NextHopThrift>({path1_2_1_php, path1_3_1_php}));

  // Prefer PHP over SWAP for metric tie
  bestNextHops = getBestNextHopsMpls({path1_2_1_swap, path1_3_1_php});
  EXPECT_EQ(bestNextHops, std::vector<thrift::NextHopThrift>({path1_3_1_php}));
}

TEST(UtilTest, findDeltaRoutes) {
  thrift::RouteDatabase oldRouteDb;
  oldRouteDb.thisNodeName = "node-1";
  oldRouteDb.unicastRoutes.emplace_back(
      createUnicastRoute(prefix2, {path1_2_1, path1_2_2}));
  oldRouteDb.mplsRoutes.emplace_back(
      createMplsRoute(2, {path1_2_1_swap, path1_2_2_swap}));

  thrift::RouteDatabase newRouteDb;
  newRouteDb.thisNodeName = "node-1";
  newRouteDb.unicastRoutes.emplace_back(
      createUnicastRoute(prefix2, {path1_2_1, path1_2_2, path1_2_3}));
  newRouteDb.mplsRoutes.emplace_back(
      createMplsRoute(2, {path1_2_1_swap, path1_2_2_swap, path1_2_3_swap}));

  const auto& res1 =
      findDeltaRoutes(std::move(newRouteDb), std::move(oldRouteDb));

  EXPECT_EQ(res1.unicastRoutesToUpdate.size(), 1);
  EXPECT_EQ(res1.unicastRoutesToUpdate, newRouteDb.unicastRoutes);
  EXPECT_EQ(res1.unicastRoutesToDelete.size(), 0);
  EXPECT_EQ(res1.mplsRoutesToUpdate.size(), 1);
  EXPECT_EQ(res1.mplsRoutesToUpdate, newRouteDb.mplsRoutes);
  EXPECT_EQ(res1.mplsRoutesToDelete.size(), 0);

  // add more unicastRoutes in newRouteDb
  newRouteDb.unicastRoutes.emplace_back(
      createUnicastRoute(prefix3, {path1_3_1, path1_3_2}));
  newRouteDb.mplsRoutes.emplace_back(
      createMplsRoute(3, {path1_3_1_swap, path1_3_2_swap}));

  const auto& res2 =
      findDeltaRoutes(std::move(newRouteDb), std::move(oldRouteDb));
  EXPECT_EQ(res2.unicastRoutesToUpdate.size(), 2);
  EXPECT_EQ(res2.unicastRoutesToUpdate, newRouteDb.unicastRoutes);
  EXPECT_EQ(res2.unicastRoutesToDelete.size(), 0);
  EXPECT_EQ(res2.mplsRoutesToUpdate.size(), 2);
  EXPECT_EQ(res2.mplsRoutesToUpdate, newRouteDb.mplsRoutes);
  EXPECT_EQ(res2.mplsRoutesToDelete.size(), 0);

  // empty out newRouteDb
  newRouteDb.unicastRoutes.clear();
  newRouteDb.mplsRoutes.clear();
  const auto& res3 =
      findDeltaRoutes(std::move(newRouteDb), std::move(oldRouteDb));
  EXPECT_EQ(res3.unicastRoutesToUpdate.size(), 0);
  EXPECT_EQ(res3.unicastRoutesToDelete.size(), 1);
  EXPECT_EQ(res3.unicastRoutesToDelete.at(0), prefix2);
  EXPECT_EQ(res3.mplsRoutesToUpdate.size(), 0);
  EXPECT_EQ(res3.mplsRoutesToDelete.size(), 1);
  EXPECT_EQ(res3.mplsRoutesToDelete.at(0), 2);
}

TEST(UtilTest, MplsLabelValidate) {
  EXPECT_TRUE(isMplsLabelValid(0));
  EXPECT_TRUE(isMplsLabelValid(1132));
  EXPECT_TRUE(isMplsLabelValid((1 << 20) - 1));
  EXPECT_FALSE(isMplsLabelValid(1 << 20));
  EXPECT_FALSE(isMplsLabelValid(1 << 30));
}

TEST(UtilTest, MplsActionValidate) {
  //
  // PHP
  //
  {
    thrift::MplsAction mplsAction;
    mplsAction.action = thrift::MplsActionCode::PHP;
    EXPECT_NO_FATAL_FAILURE(checkMplsAction(mplsAction));

    mplsAction.swapLabel_ref() = 1;
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.swapLabel_ref().reset();

    mplsAction.pushLabels_ref() = std::vector<int32_t>();
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.pushLabels_ref().reset();
  }

  //
  // POP_AND_LOOKUP
  //
  {
    thrift::MplsAction mplsAction;
    mplsAction.action = thrift::MplsActionCode::POP_AND_LOOKUP;
    EXPECT_NO_FATAL_FAILURE(checkMplsAction(mplsAction));

    mplsAction.swapLabel_ref() = 1;
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.swapLabel_ref().reset();

    mplsAction.pushLabels_ref() = std::vector<int32_t>();
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.pushLabels_ref().reset();
  }

  //
  // SWAP
  //
  {
    thrift::MplsAction mplsAction;
    mplsAction.action = thrift::MplsActionCode::SWAP;
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");

    mplsAction.swapLabel_ref() = 1;
    EXPECT_NO_FATAL_FAILURE(checkMplsAction(mplsAction));

    mplsAction.pushLabels_ref() = std::vector<int32_t>();
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.pushLabels_ref().reset();
  }

  //
  // PUSH
  //
  {
    thrift::MplsAction mplsAction;
    mplsAction.action = thrift::MplsActionCode::PUSH;
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");

    mplsAction.swapLabel_ref() = 1;
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.swapLabel_ref().reset();

    mplsAction.pushLabels_ref() = std::vector<int32_t>();
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");

    mplsAction.pushLabels_ref()->push_back(1);
    EXPECT_NO_FATAL_FAILURE(checkMplsAction(mplsAction));
  }
}

TEST(UtilTest, getPrefixForwardingTypeAndAlgorithm) {
  thrift::PrefixEntries prefixes;

  // Default case (empty entries)
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::IP, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm(prefixes));

  prefixes[{"node1", "area1"}] = createPrefixEntry(toIpPrefix("10.0.0.0/8"));
  prefixes[{"node2", "area1"}] = createPrefixEntry(toIpPrefix("10.0.0.0/8"));
  prefixes[{"node3", "area1"}] = createPrefixEntry(toIpPrefix("10.0.0.0/8"));

  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::IP, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm(prefixes));

  prefixes[{"node3", "area1"}].forwardingType_ref() = FwdType::SR_MPLS;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::IP, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm(prefixes));

  prefixes[{"node2", "area1"}].forwardingType_ref() = FwdType::SR_MPLS;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::IP, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm(prefixes));

  prefixes[{"node1", "area1"}].forwardingType_ref() = FwdType::SR_MPLS;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::SR_MPLS, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm(prefixes));

  prefixes[{"node3", "area1"}].forwardingAlgorithm_ref() =
      FwdAlgo::KSP2_ED_ECMP;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::SR_MPLS, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm(prefixes));

  prefixes[{"node2", "area1"}].forwardingAlgorithm_ref() =
      FwdAlgo::KSP2_ED_ECMP;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::SR_MPLS, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm(prefixes));

  prefixes[{"node1", "area1"}].forwardingAlgorithm_ref() =
      FwdAlgo::KSP2_ED_ECMP;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(
          FwdType::SR_MPLS, FwdAlgo::KSP2_ED_ECMP)),
      getPrefixForwardingTypeAndAlgorithm(prefixes));
}

using namespace openr::MetricVectorUtils;
TEST(MetricVectorUtilsTest, CompareResultInverseOperator) {
  EXPECT_EQ(CompareResult::WINNER, !CompareResult::LOOSER);
  EXPECT_EQ(!CompareResult::WINNER, CompareResult::LOOSER);

  EXPECT_EQ(CompareResult::TIE, !CompareResult::TIE);

  EXPECT_EQ(CompareResult::TIE_WINNER, !CompareResult::TIE_LOOSER);
  EXPECT_EQ(!CompareResult::TIE_WINNER, CompareResult::TIE_LOOSER);

  EXPECT_EQ(CompareResult::ERROR, !CompareResult::ERROR);
}

TEST(MetricVectorUtilsTest, isDecisive) {
  EXPECT_TRUE(isDecisive(CompareResult::WINNER));
  EXPECT_TRUE(isDecisive(CompareResult::LOOSER));
  EXPECT_TRUE(isDecisive(CompareResult::ERROR));

  EXPECT_FALSE(isDecisive(CompareResult::TIE_WINNER));
  EXPECT_FALSE(isDecisive(CompareResult::TIE_LOOSER));
  EXPECT_FALSE(isDecisive(CompareResult::TIE));
}

TEST(MetricVectorUtilsTest, sortMetricVector) {
  thrift::MetricVector mv;

  int64_t const numMetrics = 5;

  // default construct some MetricEntities
  mv.metrics.resize(numMetrics);

  for (int64_t i = 0; i < numMetrics; i++) {
    mv.metrics[i].type = i;
    mv.metrics[i].priority = i;
  }

  EXPECT_FALSE(isSorted(mv));
  sortMetricVector(mv);
  EXPECT_TRUE(isSorted(mv));
}

TEST(MetricVectorUtilsTest, compareMetrics) {
  EXPECT_EQ(CompareResult::TIE, compareMetrics({}, {}, true));
  EXPECT_EQ(CompareResult::ERROR, compareMetrics({1}, {}, true));
  EXPECT_EQ(CompareResult::TIE, compareMetrics({1, 2}, {1, 2}, true));

  EXPECT_EQ(CompareResult::WINNER, compareMetrics({2}, {1}, false));
  EXPECT_EQ(CompareResult::LOOSER, compareMetrics({2, 1}, {2, 3}, false));

  EXPECT_EQ(CompareResult::TIE_WINNER, compareMetrics({-1}, {-2}, true));
  EXPECT_EQ(CompareResult::TIE_LOOSER, compareMetrics({1, 1}, {2, 0}, true));
}

TEST(MetricVectorUtilsTest, resultForLoner) {
  thrift::MetricEntity entity;
  entity.op = thrift::CompareType::WIN_IF_PRESENT;
  entity.isBestPathTieBreaker = false;
  EXPECT_EQ(resultForLoner(entity), CompareResult::WINNER);
  entity.isBestPathTieBreaker = true;
  EXPECT_EQ(resultForLoner(entity), CompareResult::TIE_WINNER);

  entity.op = thrift::CompareType::WIN_IF_NOT_PRESENT;
  entity.isBestPathTieBreaker = false;
  EXPECT_EQ(resultForLoner(entity), CompareResult::LOOSER);
  entity.isBestPathTieBreaker = true;
  EXPECT_EQ(resultForLoner(entity), CompareResult::TIE_LOOSER);

  entity.op = thrift::CompareType::IGNORE_IF_NOT_PRESENT;
  entity.isBestPathTieBreaker = false;
  EXPECT_EQ(resultForLoner(entity), CompareResult::TIE);
  entity.isBestPathTieBreaker = true;
  EXPECT_EQ(resultForLoner(entity), CompareResult::TIE);
}

TEST(MetricVectorUtilsTest, maybeUpdate) {
  CompareResult result = CompareResult::TIE;
  maybeUpdate(result, CompareResult::TIE_WINNER);
  EXPECT_EQ(result, CompareResult::TIE_WINNER);

  maybeUpdate(result, CompareResult::TIE_LOOSER);
  EXPECT_EQ(result, CompareResult::TIE_WINNER);

  maybeUpdate(result, CompareResult::WINNER);
  EXPECT_EQ(result, CompareResult::WINNER);

  maybeUpdate(result, CompareResult::TIE_WINNER);
  EXPECT_EQ(result, CompareResult::WINNER);

  maybeUpdate(result, CompareResult::ERROR);
  EXPECT_EQ(result, CompareResult::ERROR);
}

TEST(MetricVectorUtilsTest, compareMetricVectors) {
  thrift::MetricVector l, r;
  EXPECT_EQ(CompareResult::TIE, compareMetricVectors(l, r));

  l.version = 1;
  r.version = 2;
  EXPECT_EQ(CompareResult::ERROR, compareMetricVectors(l, r));
  r.version = 1;

  int64_t numMetrics = 5;
  l.metrics.resize(numMetrics);
  r.metrics.resize(numMetrics);
  for (int64_t i = 0; i < numMetrics; ++i) {
    l.metrics[i].type = i;
    l.metrics[i].priority = i;
    l.metrics[i].op = thrift::CompareType::WIN_IF_PRESENT;
    l.metrics[i].isBestPathTieBreaker = false;
    l.metrics[i].metric = {i};

    r.metrics[i].type = i;
    r.metrics[i].priority = i;
    r.metrics[i].op = thrift::CompareType::WIN_IF_PRESENT;
    r.metrics[i].isBestPathTieBreaker = false;
    r.metrics[i].metric = {i};
  }

  EXPECT_EQ(CompareResult::TIE, compareMetricVectors(l, r));

  r.metrics[numMetrics - 2].metric.front()--;
  EXPECT_EQ(CompareResult::WINNER, compareMetricVectors(l, r));
  EXPECT_EQ(CompareResult::LOOSER, compareMetricVectors(r, l));

  r.metrics[numMetrics - 2].isBestPathTieBreaker = true;
  EXPECT_EQ(CompareResult::ERROR, compareMetricVectors(l, r));
  l.metrics[numMetrics - 2].isBestPathTieBreaker = true;
  EXPECT_EQ(CompareResult::TIE_WINNER, compareMetricVectors(l, r));
  EXPECT_EQ(CompareResult::TIE_LOOSER, compareMetricVectors(r, l));

  r.metrics.resize(numMetrics - 1);
  EXPECT_EQ(CompareResult::WINNER, compareMetricVectors(l, r));
  EXPECT_EQ(CompareResult::LOOSER, compareMetricVectors(r, l));

  // make type different but keep priority the same
  l.metrics[0].type--;
  EXPECT_EQ(CompareResult::ERROR, compareMetricVectors(l, r));
  EXPECT_EQ(CompareResult::ERROR, compareMetricVectors(r, l));
  l.metrics[0].type++;

  // change op for l loner;
  l.metrics[numMetrics - 1].op = thrift::CompareType::WIN_IF_NOT_PRESENT;
  EXPECT_EQ(CompareResult::LOOSER, compareMetricVectors(l, r));
  EXPECT_EQ(CompareResult::WINNER, compareMetricVectors(r, l));

  l.metrics[numMetrics - 1].op = thrift::CompareType::IGNORE_IF_NOT_PRESENT;
  EXPECT_EQ(CompareResult::TIE_WINNER, compareMetricVectors(l, r));
  EXPECT_EQ(CompareResult::TIE_LOOSER, compareMetricVectors(r, l));
}

TEST(UtilTest, FunctionExecutionTime) {
  LOG_FN_EXECUTION_TIME;
}

TEST(UtilTest, AddJitter) {
  std::chrono::milliseconds t1_ms(100);
  std::chrono::seconds t2_s(200);
  double pct = 20;

  auto t1_jitter_ms = addJitter<std::chrono::milliseconds>(t1_ms, pct);
  auto t2_jitter_s = addJitter<std::chrono::seconds>(t2_s, pct);
  CHECK(
      t1_jitter_ms >= (1 - pct / 100.0) * t1_ms and
      t1_jitter_ms <= (1 + pct / 100.0) * t1_jitter_ms);
  CHECK(
      t2_jitter_s >= (1 - pct / 100.0) * t2_s and
      t2_jitter_s <= (1 + pct / 100.0) * t2_jitter_s);
}

TEST(UtilTest, BestMetricsSelection) {
  auto createMetrics = [](int32_t pp, int32_t sp, int32_t d) {
    thrift::PrefixEntry prefixEntry;
    prefixEntry.metrics_ref()->path_preference_ref() = pp;
    prefixEntry.metrics_ref()->source_preference_ref() = sp;
    prefixEntry.metrics_ref()->distance_ref() = d;
    return prefixEntry;
  };

  //
  // No entry. Returns empty set
  //
  {
    std::unordered_map<std::string, thrift::PrefixEntry> prefixes;
    EXPECT_EQ(0, selectBestPrefixMetrics(prefixes).size());
  }

  //
  // Single entry. Returns the entry itself
  //
  {
    std::unordered_map<std::string, thrift::PrefixEntry> prefixes = {
        {"KEY1", createMetrics(0, 0, 0)}};
    const auto bestKeys = selectBestPrefixMetrics(prefixes);
    EXPECT_EQ(1, bestKeys.size());
    EXPECT_EQ(1, bestKeys.count("KEY1"));
  }

  //
  // Multiple entries. Single best route, tie on source-preference
  // (prefer higher)
  //
  {
    std::unordered_map<std::string, thrift::PrefixEntry> prefixes = {
        {"KEY1", createMetrics(100, 0, 0)},
        {"KEY2", createMetrics(200, 0, 0)},
        {"KEY3", createMetrics(300, 0, 0)}};
    const auto bestKeys = selectBestPrefixMetrics(prefixes);
    EXPECT_EQ(1, bestKeys.size());
    EXPECT_EQ(1, bestKeys.count("KEY3"));
  }

  //
  // Multiple entries. Single best route, tie on local-preference
  // (prefer higher)
  //
  {
    std::unordered_map<std::string, thrift::PrefixEntry> prefixes = {
        {"KEY1", createMetrics(100, 10, 0)},
        {"KEY2", createMetrics(100, 200, 0)},
        {"KEY3", createMetrics(100, 30, 0)}};
    const auto bestKeys = selectBestPrefixMetrics(prefixes);
    EXPECT_EQ(1, bestKeys.size());
    EXPECT_EQ(1, bestKeys.count("KEY2"));
  }

  //
  // Multiple entries. Single best route, tie on distance (prefer lower)
  //
  {
    std::unordered_map<std::string, thrift::PrefixEntry> prefixes = {
        {"KEY1", createMetrics(100, 10, 1)},
        {"KEY2", createMetrics(100, 10, 2)},
        {"KEY3", createMetrics(100, 10, 3)}};
    const auto bestKeys = selectBestPrefixMetrics(prefixes);
    EXPECT_EQ(1, bestKeys.size());
    EXPECT_EQ(1, bestKeys.count("KEY1"));
  }

  //
  // Multiple entries. Multiple best routes
  //
  {
    std::unordered_map<std::string, thrift::PrefixEntry> prefixes = {
        {"KEY1", createMetrics(100, 10, 1)},
        {"KEY2", createMetrics(100, 10, 2)},
        {"KEY3", createMetrics(100, 10, 1)},
        {"KEY4", createMetrics(100, 10, 1)},
        {"KEY5", createMetrics(100, 10, 2)}};
    const auto bestKeys = selectBestPrefixMetrics(prefixes);
    EXPECT_EQ(3, bestKeys.size());
    EXPECT_EQ(1, bestKeys.count("KEY1"));
    EXPECT_EQ(1, bestKeys.count("KEY3"));
    EXPECT_EQ(1, bestKeys.count("KEY4"));
  }
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
