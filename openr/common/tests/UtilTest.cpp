/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>
#include <memory>
#include <utility>

#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <sodium.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Types_types.h>

using namespace std;
using namespace openr;
using FwdType = openr::thrift::PrefixForwardingType;
using FwdAlgo = openr::thrift::PrefixForwardingAlgorithm;

const auto node11Area1 =
    std::make_pair<std::string, std::string>("node11", "area1");
const auto node12Area1 =
    std::make_pair<std::string, std::string>("node12", "area1");
const auto node13Area1 =
    std::make_pair<std::string, std::string>("node13", "area1");
const auto node21Area2 =
    std::make_pair<std::string, std::string>("node21", "area2");
const auto node22Area2 =
    std::make_pair<std::string, std::string>("node22", "area2");

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
  std::string area{thrift::Types_constants::kDefaultArea()};
  thrift::IpPrefix ipPrefix;
  folly::IPAddress addr;
  int plen{0};
};

thrift::PrefixEntry
createPrefixEntry(int32_t pp, int32_t sp, int32_t d) {
  thrift::PrefixEntry prefixEntry;
  prefixEntry.metrics_ref() = createMetrics(pp, sp, d);
  return prefixEntry;
};

std::shared_ptr<thrift::PrefixEntry>
createPrefixEntryPtr(int32_t pp, int32_t sp, int32_t d) {
  return std::make_shared<thrift::PrefixEntry>(createPrefixEntry(pp, sp, d));
};

TEST(UtilTest, NetworkUtilTest) {
  //
  // Test for: toString()
  //
  {
    folly::IPAddress v4{"192.168.0.2"};
    folly::IPAddress v6{"fe80::2"};

    EXPECT_EQ(v4.str(), toString(toBinaryAddress(v4)));
    EXPECT_EQ(v6.str(), toString(toBinaryAddress(v6)));

    thrift::BinaryAddress empty;
    EXPECT_EQ("", toString(empty));
  }

  //
  // Test for: toIpPrefix()
  //
  {
    // Positive test for valid ip addrs(V4 + V6)
    const std::string v4Addr{"10.1.1.1/32"};
    const std::string v6Addr{"2620::1/128"};
    thrift::IpPrefix prefixV4 = toIpPrefix(v4Addr);
    thrift::IpPrefix prefixV6 = toIpPrefix(v6Addr);

    EXPECT_EQ("10.1.1.1", toString(*prefixV4.prefixAddress_ref()));
    EXPECT_EQ(32, *prefixV4.prefixLength_ref());
    EXPECT_EQ("2620::1", toString(*prefixV6.prefixAddress_ref()));
    EXPECT_EQ(128, *prefixV6.prefixLength_ref());

    // Negative test for invalid ip addr length(V4 + V6)
    const std::string invalidV4Addr{"10.1.1.1/36"};
    const std::string invalidV6Addr{"2620::1/130"};

    EXPECT_THROW(toIpPrefix(invalidV4Addr), thrift::OpenrError);
    EXPECT_THROW(toIpPrefix(invalidV6Addr), thrift::OpenrError);
  }

  //
  // Test for: toIPNetwork()
  //
  {
    // Negative test for invalid ip addr length(V4 + V6)
    folly::IPAddress v4Addr = folly::IPAddress("10.1.1.1");
    folly::IPAddress v6Addr = folly::IPAddress("2620::1");
    const uint16_t invalidV4Len = 36; // ATTN: max mask length is 32 for IPV4
    const uint16_t invalidV6Len = 130; // ATTN: max mask length is 128 for IPV6

    thrift::IpPrefix v4Prefix, v6Prefix;
    v4Prefix.prefixAddress_ref() = toBinaryAddress(v4Addr);
    v4Prefix.prefixLength_ref() = invalidV4Len;
    v6Prefix.prefixAddress_ref() = toBinaryAddress(v6Addr);
    v6Prefix.prefixLength_ref() = invalidV6Len;

    EXPECT_THROW(toIPNetwork(v4Prefix), thrift::OpenrError);
    EXPECT_THROW(toIPNetwork(v6Prefix), thrift::OpenrError);
  }

  //
  // Test for: createIpPrefix()
  //
  {
    folly::IPAddress v4Addr = folly::IPAddress("10.1.1.1");
    thrift::BinaryAddress v4AddrBin = toBinaryAddress(v4Addr);
    const uint16_t v4Len = 32;

    thrift::IpPrefix v4Prefix = createIpPrefix(v4AddrBin, v4Len);
    EXPECT_EQ(v4Prefix.prefixAddress_ref().value(), v4AddrBin);
    EXPECT_EQ(v4Prefix.prefixLength_ref().value(), v4Len);
  }
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

  // this should pass
  k1.pkey = "prefix:nodename0:pod-108:[ff00::0/64]";
  k1.area = "pod-108";
  k1.shouldPass = true;
  strToItems.push_back(k1);

  // this should pass
  k1.pkey = "prefix:nodename0:dc.fabric01.pod001:[ff00::0/64]";
  k1.area = "dc.fabric01.pod001";
  k1.shouldPass = true;
  strToItems.push_back(k1);

  // this should pass
  k1.pkey = "prefix:nodename0:dc_fabric01_pod001:[ff00::0/64]";
  k1.area = "dc_fabric01_pod001";
  k1.shouldPass = true;
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

  for (const auto& keys : strToItems) {
    auto prefixStr = PrefixKey::fromStr(keys.pkey);
    if (keys.shouldPass) {
      EXPECT_EQ(prefixStr.value().getNodeName(), keys.node);
      EXPECT_EQ(prefixStr.value().getCIDRNetwork(), keys.ipaddr);
      EXPECT_EQ(prefixStr.value().getPrefixArea(), keys.area);
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
        fmt::format("{}/{}", keys.addr.str(), keys.plen));
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
  const folly::CIDRNetwork seedPrefix{
      folly::IPAddress{"face:b00c::1"}, seedPrefixLen};

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

TEST(UtilTest, addPerfEventTest) {
  {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, "node1", "LINK_UP");
    EXPECT_EQ(perfEvents.events_ref()->size(), 1);
    EXPECT_EQ(*perfEvents.events_ref()[0].nodeName_ref(), "node1");
    EXPECT_EQ(*perfEvents.events_ref()[0].eventDescr_ref(), "LINK_UP");
  }

  {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, "node1", "LINK_UP");
    addPerfEvent(perfEvents, "node2", "LINK_DOWN");
    EXPECT_EQ(perfEvents.events_ref()->size(), 2);
    EXPECT_EQ(*perfEvents.events_ref()[0].nodeName_ref(), "node1");
    EXPECT_EQ(*perfEvents.events_ref()[0].eventDescr_ref(), "LINK_UP");
    EXPECT_EQ(*perfEvents.events_ref()[1].nodeName_ref(), "node2");
    EXPECT_EQ(*perfEvents.events_ref()[1].eventDescr_ref(), "LINK_DOWN");
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
    thrift::PerfEvent event1 = createPerfEvent("node1", "LINK_UP", 100);
    perfEvents.events_ref()->emplace_back(std::move(event1));
    thrift::PerfEvent event2 = createPerfEvent("node1", "DECISION_RECVD", 200);
    perfEvents.events_ref()->emplace_back(std::move(event2));
    thrift::PerfEvent event3 = createPerfEvent("node1", "SPF_CALCULATE", 300);
    perfEvents.events_ref()->emplace_back(std::move(event3));
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
    thrift::PerfEvent event1 = createPerfEvent("node1", "LINK_UP", 100);
    perfEvents.events_ref()->emplace_back(std::move(event1));
    thrift::PerfEvent event2 = createPerfEvent("node1", "DECISION_RECVD", 200);
    perfEvents.events_ref()->emplace_back(std::move(event2));
    thrift::PerfEvent event3 = createPerfEvent("node1", "SPF_CALCULATE", 300);
    perfEvents.events_ref()->emplace_back(std::move(event3));
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

TEST(UtilTest, findDeltaRoutes) {
  thrift::RouteDatabase oldRouteDb;
  *oldRouteDb.thisNodeName_ref() = "node-1";
  oldRouteDb.unicastRoutes_ref()->emplace_back(
      createUnicastRoute(prefix2, {path1_2_1, path1_2_2}));
  oldRouteDb.mplsRoutes_ref()->emplace_back(
      createMplsRoute(2, {path1_2_1_swap, path1_2_2_swap}));

  thrift::RouteDatabase newRouteDb;
  *newRouteDb.thisNodeName_ref() = "node-1";
  newRouteDb.unicastRoutes_ref()->emplace_back(
      createUnicastRoute(prefix2, {path1_2_1, path1_2_2, path1_2_3}));
  newRouteDb.mplsRoutes_ref()->emplace_back(
      createMplsRoute(2, {path1_2_1_swap, path1_2_2_swap, path1_2_3_swap}));

  const auto& res1 =
      findDeltaRoutes(std::move(newRouteDb), std::move(oldRouteDb));

  EXPECT_EQ(res1.unicastRoutesToUpdate_ref()->size(), 1);
  EXPECT_EQ(*res1.unicastRoutesToUpdate_ref(), *newRouteDb.unicastRoutes_ref());
  EXPECT_EQ(res1.unicastRoutesToDelete_ref()->size(), 0);
  EXPECT_EQ(res1.mplsRoutesToUpdate_ref()->size(), 1);
  EXPECT_EQ(*res1.mplsRoutesToUpdate_ref(), *newRouteDb.mplsRoutes_ref());
  EXPECT_EQ(res1.mplsRoutesToDelete_ref()->size(), 0);

  // add more unicastRoutes in newRouteDb
  newRouteDb.unicastRoutes_ref()->emplace_back(
      createUnicastRoute(prefix3, {path1_3_1, path1_3_2}));
  newRouteDb.mplsRoutes_ref()->emplace_back(
      createMplsRoute(3, {path1_3_1_swap, path1_3_2_swap}));

  const auto& res2 =
      findDeltaRoutes(std::move(newRouteDb), std::move(oldRouteDb));
  EXPECT_EQ(res2.unicastRoutesToUpdate_ref()->size(), 2);
  EXPECT_EQ(*res2.unicastRoutesToUpdate_ref(), *newRouteDb.unicastRoutes_ref());
  EXPECT_EQ(res2.unicastRoutesToDelete_ref()->size(), 0);
  EXPECT_EQ(res2.mplsRoutesToUpdate_ref()->size(), 2);
  EXPECT_EQ(*res2.mplsRoutesToUpdate_ref(), *newRouteDb.mplsRoutes_ref());
  EXPECT_EQ(res2.mplsRoutesToDelete_ref()->size(), 0);

  // empty out newRouteDb
  newRouteDb.unicastRoutes_ref()->clear();
  newRouteDb.mplsRoutes_ref()->clear();
  const auto& res3 =
      findDeltaRoutes(std::move(newRouteDb), std::move(oldRouteDb));
  EXPECT_EQ(res3.unicastRoutesToUpdate_ref()->size(), 0);
  EXPECT_EQ(res3.unicastRoutesToDelete_ref()->size(), 1);
  EXPECT_EQ(res3.unicastRoutesToDelete_ref()->at(0), prefix2);
  EXPECT_EQ(res3.mplsRoutesToUpdate_ref()->size(), 0);
  EXPECT_EQ(res3.mplsRoutesToDelete_ref()->size(), 1);
  EXPECT_EQ(res3.mplsRoutesToDelete_ref()->at(0), 2);
}

TEST(UtilTest, MplsActionValidate) {
  //
  // PHP
  //
  {
    thrift::MplsAction mplsAction;
    mplsAction.action_ref() = thrift::MplsActionCode::PHP;
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
    mplsAction.action_ref() = thrift::MplsActionCode::POP_AND_LOOKUP;
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
    mplsAction.action_ref() = thrift::MplsActionCode::SWAP;
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
    mplsAction.action_ref() = thrift::MplsActionCode::PUSH;
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
  PrefixEntries prefixes;

  // Default case (empty entries)
  EXPECT_EQ(
      std::nullopt, getPrefixForwardingTypeAndAlgorithm("area1", prefixes, {}));

  prefixes[{"node1", "area1"}] = std::make_shared<thrift::PrefixEntry>(
      createPrefixEntry(toIpPrefix("10.0.0.0/8")));
  prefixes[{"node2", "area1"}] = std::make_shared<thrift::PrefixEntry>(
      createPrefixEntry(toIpPrefix("10.0.0.0/8")));
  prefixes[{"node3", "area1"}] = std::make_shared<thrift::PrefixEntry>(
      createPrefixEntry(toIpPrefix("10.0.0.0/8")));

  std::set<NodeAndArea> bestNodeAreas = {
      {"node1", "area1"}, {"node2", "area1"}, {"node3", "area1"}};

  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::IP, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm("area1", prefixes, bestNodeAreas));
  EXPECT_EQ(
      std::nullopt,
      getPrefixForwardingTypeAndAlgorithm("area2", prefixes, bestNodeAreas));

  prefixes[{"node3", "area1"}]->forwardingType_ref() = FwdType::SR_MPLS;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::IP, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm("area1", prefixes, bestNodeAreas));

  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::SR_MPLS, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm(
          "area1", prefixes, {{"node3", "area1"}}));

  prefixes[{"node2", "area1"}]->forwardingType_ref() = FwdType::SR_MPLS;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::IP, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm("area1", prefixes, bestNodeAreas));

  prefixes[{"node1", "area1"}]->forwardingType_ref() = FwdType::SR_MPLS;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::SR_MPLS, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm("area1", prefixes, bestNodeAreas));

  prefixes[{"node3", "area1"}]->forwardingAlgorithm_ref() =
      FwdAlgo::KSP2_ED_ECMP;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::SR_MPLS, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm("area1", prefixes, bestNodeAreas));

  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(
          FwdType::SR_MPLS, FwdAlgo::KSP2_ED_ECMP)),
      getPrefixForwardingTypeAndAlgorithm(
          "area1", prefixes, {{"node3", "area1"}}));

  prefixes[{"node2", "area1"}]->forwardingAlgorithm_ref() =
      FwdAlgo::KSP2_ED_ECMP;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::SR_MPLS, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm("area1", prefixes, bestNodeAreas));

  prefixes[{"node1", "area1"}]->forwardingAlgorithm_ref() =
      FwdAlgo::KSP2_ED_ECMP;
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(
          FwdType::SR_MPLS, FwdAlgo::KSP2_ED_ECMP)),
      getPrefixForwardingTypeAndAlgorithm("area1", prefixes, bestNodeAreas));

  prefixes[{"node4", "area2"}] = std::make_shared<thrift::PrefixEntry>(
      createPrefixEntry(toIpPrefix("10.0.0.0/8")));
  bestNodeAreas.insert({"node4", "area2"});
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(
          FwdType::SR_MPLS, FwdAlgo::KSP2_ED_ECMP)),
      getPrefixForwardingTypeAndAlgorithm("area1", prefixes, bestNodeAreas));
  EXPECT_EQ(
      (std::make_pair<FwdType, FwdAlgo>(FwdType::IP, FwdAlgo::SP_ECMP)),
      getPrefixForwardingTypeAndAlgorithm("area2", prefixes, bestNodeAreas));
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
  mv.metrics_ref()->resize(numMetrics);

  for (int64_t i = 0; i < numMetrics; i++) {
    mv.metrics_ref()[i].type_ref() = i;
    mv.metrics_ref()[i].priority_ref() = i;
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
  entity.op_ref() = thrift::CompareType::WIN_IF_PRESENT;
  entity.isBestPathTieBreaker_ref() = false;
  EXPECT_EQ(resultForLoner(entity), CompareResult::WINNER);
  entity.isBestPathTieBreaker_ref() = true;
  EXPECT_EQ(resultForLoner(entity), CompareResult::TIE_WINNER);

  entity.op_ref() = thrift::CompareType::WIN_IF_NOT_PRESENT;
  entity.isBestPathTieBreaker_ref() = false;
  EXPECT_EQ(resultForLoner(entity), CompareResult::LOOSER);
  entity.isBestPathTieBreaker_ref() = true;
  EXPECT_EQ(resultForLoner(entity), CompareResult::TIE_LOOSER);

  entity.op_ref() = thrift::CompareType::IGNORE_IF_NOT_PRESENT;
  entity.isBestPathTieBreaker_ref() = false;
  EXPECT_EQ(resultForLoner(entity), CompareResult::TIE);
  entity.isBestPathTieBreaker_ref() = true;
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

  l.version_ref() = 1;
  r.version_ref() = 2;
  EXPECT_EQ(CompareResult::ERROR, compareMetricVectors(l, r));
  r.version_ref() = 1;

  int64_t numMetrics = 5;
  l.metrics_ref()->resize(numMetrics);
  r.metrics_ref()->resize(numMetrics);
  for (int64_t i = 0; i < numMetrics; ++i) {
    l.metrics_ref()[i].type_ref() = i;
    l.metrics_ref()[i].priority_ref() = i;
    l.metrics_ref()[i].op_ref() = thrift::CompareType::WIN_IF_PRESENT;
    l.metrics_ref()[i].isBestPathTieBreaker_ref() = false;
    *l.metrics_ref()[i].metric_ref() = {i};

    r.metrics_ref()[i].type_ref() = i;
    r.metrics_ref()[i].priority_ref() = i;
    r.metrics_ref()[i].op_ref() = thrift::CompareType::WIN_IF_PRESENT;
    r.metrics_ref()[i].isBestPathTieBreaker_ref() = false;
    *r.metrics_ref()[i].metric_ref() = {i};
  }

  EXPECT_EQ(CompareResult::TIE, compareMetricVectors(l, r));

  r.metrics_ref()[numMetrics - 2].metric_ref()->front()--;
  EXPECT_EQ(CompareResult::WINNER, compareMetricVectors(l, r));
  EXPECT_EQ(CompareResult::LOOSER, compareMetricVectors(r, l));

  r.metrics_ref()[numMetrics - 2].isBestPathTieBreaker_ref() = true;
  EXPECT_EQ(CompareResult::ERROR, compareMetricVectors(l, r));
  l.metrics_ref()[numMetrics - 2].isBestPathTieBreaker_ref() = true;
  EXPECT_EQ(CompareResult::TIE_WINNER, compareMetricVectors(l, r));
  EXPECT_EQ(CompareResult::TIE_LOOSER, compareMetricVectors(r, l));

  r.metrics_ref()->resize(numMetrics - 1);
  EXPECT_EQ(CompareResult::WINNER, compareMetricVectors(l, r));
  EXPECT_EQ(CompareResult::LOOSER, compareMetricVectors(r, l));

  // make type different but keep priority the same
  (*l.metrics_ref()[0].type_ref())--;
  EXPECT_EQ(CompareResult::ERROR, compareMetricVectors(l, r));
  EXPECT_EQ(CompareResult::ERROR, compareMetricVectors(r, l));
  (*l.metrics_ref()[0].type_ref())++;

  // change op for l loner;
  l.metrics_ref()[numMetrics - 1].op_ref() =
      thrift::CompareType::WIN_IF_NOT_PRESENT;
  EXPECT_EQ(CompareResult::LOOSER, compareMetricVectors(l, r));
  EXPECT_EQ(CompareResult::WINNER, compareMetricVectors(r, l));

  l.metrics_ref()[numMetrics - 1].op_ref() =
      thrift::CompareType::IGNORE_IF_NOT_PRESENT;
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
        {"KEY1", createPrefixEntry(0, 0, 0)}};
    const auto bestKeys = selectBestPrefixMetrics(prefixes);
    EXPECT_EQ(1, bestKeys.size());
    EXPECT_EQ(1, bestKeys.count("KEY1"));
  }

  //
  // Multiple entries. Single best route, tie on path-preference (prefer higher)
  //
  {
    std::unordered_map<std::string, thrift::PrefixEntry> prefixes = {
        {"KEY1", createPrefixEntry(100, 0, 0)},
        {"KEY2", createPrefixEntry(200, 0, 0)},
        {"KEY3", createPrefixEntry(300, 0, 0)}};
    const auto bestKeys = selectBestPrefixMetrics(prefixes);
    EXPECT_EQ(1, bestKeys.size());
    EXPECT_EQ(1, bestKeys.count("KEY3"));
  }

  //
  // Multiple entries. Single best route, tie on source-preference (prefer
  // higher)
  //
  {
    std::unordered_map<std::string, thrift::PrefixEntry> prefixes = {
        {"KEY1", createPrefixEntry(100, 10, 0)},
        {"KEY2", createPrefixEntry(100, 200, 0)},
        {"KEY3", createPrefixEntry(100, 30, 0)}};
    const auto bestKeys = selectBestPrefixMetrics(prefixes);
    EXPECT_EQ(1, bestKeys.size());
    EXPECT_EQ(1, bestKeys.count("KEY2"));
  }

  //
  // Multiple entries. Single best route, tie on distance (prefer lower)
  //
  {
    std::unordered_map<std::string, thrift::PrefixEntry> prefixes = {
        {"KEY1", createPrefixEntry(100, 10, 1)},
        {"KEY2", createPrefixEntry(100, 10, 2)},
        {"KEY3", createPrefixEntry(100, 10, 3)}};
    const auto bestKeys = selectBestPrefixMetrics(prefixes);
    EXPECT_EQ(1, bestKeys.size());
    EXPECT_EQ(1, bestKeys.count("KEY1"));
  }

  //
  // Multiple entries. Multiple best routes
  //
  {
    std::unordered_map<std::string, thrift::PrefixEntry> prefixes = {
        {"KEY1", createPrefixEntry(100, 10, 1)},
        {"KEY2", createPrefixEntry(100, 10, 2)},
        {"KEY3", createPrefixEntry(100, 10, 1)},
        {"KEY4", createPrefixEntry(100, 10, 1)},
        {"KEY5", createPrefixEntry(100, 10, 2)}};
    const auto bestKeys = selectBestPrefixMetrics(prefixes);
    EXPECT_EQ(3, bestKeys.size());
    EXPECT_EQ(1, bestKeys.count("KEY1"));
    EXPECT_EQ(1, bestKeys.count("KEY3"));
    EXPECT_EQ(1, bestKeys.count("KEY4"));
  }
}

TEST(UtilTest, SelectRoutesShortestDistance) {
  const auto kShortestAlgorithm =
      thrift::RouteSelectionAlgorithm::SHORTEST_DISTANCE;
  // No entry. Returns empty set
  {
    PrefixEntries prefixes;
    EXPECT_EQ(0, selectRoutes(prefixes, kShortestAlgorithm).size());
  }

  // Single entry. Returns the entry itself
  {
    PrefixEntries prefixes = {{node11Area1, createPrefixEntryPtr(0, 0, 0)}};
    const auto ret = selectRoutes(prefixes, kShortestAlgorithm);
    EXPECT_EQ(1, ret.size());
    EXPECT_EQ(1, ret.count(node11Area1));
  }

  // Multiple entries. Single best route, tie on path-preference (prefer higher)
  {
    PrefixEntries prefixes = {
        {node11Area1, createPrefixEntryPtr(100, 0, 0)},
        {node12Area1, createPrefixEntryPtr(200, 0, 0)},
        {node21Area2, createPrefixEntryPtr(300, 0, 0)}};
    const auto ret = selectRoutes(prefixes, kShortestAlgorithm);
    EXPECT_EQ(1, ret.size());
    EXPECT_EQ(1, ret.count(node21Area2));
  }

  // Multiple entries. Single best route, tie on source-preference (prefer
  // higher)
  {
    PrefixEntries prefixes = {
        {node11Area1, createPrefixEntryPtr(100, 10, 0)},
        {node12Area1, createPrefixEntryPtr(100, 200, 0)},
        {node21Area2, createPrefixEntryPtr(100, 30, 0)}};
    const auto ret = selectRoutes(prefixes, kShortestAlgorithm);
    EXPECT_EQ(1, ret.size());
    EXPECT_EQ(1, ret.count(node12Area1));
  }

  // Multiple entries. Single best route, tie on distance (prefer lower)
  {
    PrefixEntries prefixes = {
        {node11Area1, createPrefixEntryPtr(100, 10, 1)},
        {node12Area1, createPrefixEntryPtr(100, 10, 2)},
        {node21Area2, createPrefixEntryPtr(100, 10, 3)}};
    const auto ret = selectRoutes(prefixes, kShortestAlgorithm);
    EXPECT_EQ(1, ret.size());
    EXPECT_EQ(1, ret.count(node11Area1));
  }

  // Multiple entries. Multiple best routes
  {
    PrefixEntries prefixes = {
        {node11Area1, createPrefixEntryPtr(100, 10, 1)},
        {node12Area1, createPrefixEntryPtr(100, 10, 2)},
        {node13Area1, createPrefixEntryPtr(100, 10, 1)},
        {node21Area2, createPrefixEntryPtr(100, 10, 1)},
        {node22Area2, createPrefixEntryPtr(100, 10, 2)}};
    const auto ret = selectRoutes(prefixes, kShortestAlgorithm);
    EXPECT_EQ(3, ret.size());
    EXPECT_EQ(1, ret.count(node11Area1));
    EXPECT_EQ(1, ret.count(node13Area1));
    EXPECT_EQ(1, ret.count(node21Area2));
  }
}

TEST(UtilTest, SelectRoutesShortestDistance2) {
  const auto kAlgorithm =
      thrift::RouteSelectionAlgorithm::K_SHORTEST_DISTANCE_2;

  // Single entry. Returns the entry itself
  {
    PrefixEntries prefixes = {{node11Area1, createPrefixEntryPtr(0, 0, 0)}};
    const auto ret = selectRoutes(prefixes, kAlgorithm);
    EXPECT_EQ(1, ret.size());
    EXPECT_EQ(1, ret.count(node11Area1));
  }

  // Multiple entries. Single best and second best routes, tie on distance
  // (select shortest-distance-2)
  {
    PrefixEntries prefixes = {
        {node11Area1, createPrefixEntryPtr(100, 10, 1)},
        {node12Area1, createPrefixEntryPtr(100, 10, 3)},
        {node13Area1, createPrefixEntryPtr(100, 10, 2)},
        {node21Area2, createPrefixEntryPtr(100, 10, 4)}};
    const auto ret = selectRoutes(prefixes, kAlgorithm);
    EXPECT_EQ(2, ret.size());
    EXPECT_EQ(1, ret.count(node11Area1));
    EXPECT_EQ(1, ret.count(node13Area1));
  }

  // Multiple entries. Multi best and second best routes, tie on distance
  // (select shortest-distance-2)
  {
    PrefixEntries prefixes = {
        {node11Area1, createPrefixEntryPtr(100, 10, 1)},
        {node12Area1, createPrefixEntryPtr(100, 10, 3)},
        {node13Area1, createPrefixEntryPtr(100, 10, 2)},
        {node21Area2, createPrefixEntryPtr(100, 10, 1)},
        {node22Area2, createPrefixEntryPtr(100, 10, 2)}};
    const auto ret = selectRoutes(prefixes, kAlgorithm);
    EXPECT_EQ(4, ret.size());
    EXPECT_EQ(1, ret.count(node11Area1));
    EXPECT_EQ(1, ret.count(node13Area1));
    EXPECT_EQ(1, ret.count(node21Area2));
    EXPECT_EQ(1, ret.count(node22Area2));
  }
}

TEST(UtilTest, SelectRoutesPerAreaShortestDistance) {
  const auto kAlgorithm =
      thrift::RouteSelectionAlgorithm::PER_AREA_SHORTEST_DISTANCE;
  // Single area multiple entries. Tie on distance (prefer lower)
  {
    PrefixEntries prefixes = {
        {node11Area1, createPrefixEntryPtr(100, 10, 1)},
        {node12Area1, createPrefixEntryPtr(100, 10, 3)},
        {node13Area1, createPrefixEntryPtr(100, 10, 2)}};
    const auto ret = selectRoutes(prefixes, kAlgorithm);
    EXPECT_EQ(1, ret.size());
    EXPECT_EQ(1, ret.count(node11Area1));
  }

  // Multi areas multiple entries. Single best entry is selected in each area.
  {
    PrefixEntries prefixes = {
        {node11Area1, createPrefixEntryPtr(100, 10, 1)},
        {node12Area1, createPrefixEntryPtr(100, 10, 3)},
        {node13Area1, createPrefixEntryPtr(100, 10, 2)},
        {node21Area2, createPrefixEntryPtr(100, 10, 10)},
        {node22Area2, createPrefixEntryPtr(100, 10, 20)}};
    const auto ret = selectRoutes(prefixes, kAlgorithm);
    EXPECT_EQ(2, ret.size());
    EXPECT_EQ(1, ret.count(node11Area1));
    EXPECT_EQ(1, ret.count(node21Area2));
  }

  //
  // Multi areas multiple entries. Multi best entries are selected in each area.
  //
  {
    PrefixEntries prefixes = {
        {node11Area1, createPrefixEntryPtr(100, 10, 1)},
        {node12Area1, createPrefixEntryPtr(100, 10, 3)},
        {node13Area1, createPrefixEntryPtr(100, 10, 1)},
        {node21Area2, createPrefixEntryPtr(100, 10, 10)},
        {node22Area2, createPrefixEntryPtr(100, 10, 10)}};
    const auto ret = selectRoutes(prefixes, kAlgorithm);
    EXPECT_EQ(4, ret.size());
    EXPECT_EQ(1, ret.count(node11Area1));
    EXPECT_EQ(1, ret.count(node13Area1));
    EXPECT_EQ(1, ret.count(node21Area2));
    EXPECT_EQ(1, ret.count(node22Area2));
  }
}

TEST(UtilTest, SelectBestNodeAreaTest) {
  // Topology
  // Area A: node1-node2
  // Area B: node1-node3-node2
  std::unordered_map<std::string, LinkState> areaLinkStates;

  // Area A
  LinkState linkStateAreaA{"areaA"};
  // node1
  auto adj12 = createAdjacency(
      "node2", "1/2", "2/1", "fe80::11", "192.168.0.2", 1, 10001);
  auto adjDbNode1AreaA = createAdjDb("node1", {adj12}, 101, false, "areaA");
  linkStateAreaA.updateAdjacencyDatabase(adjDbNode1AreaA);
  // node2
  auto adj21 = createAdjacency(
      "node1", "2/1", "1/2", "fe80::21", "192.168.0.1", 1, 10002);
  auto adjDbNode2AreaA = createAdjDb("node2", {adj21}, 102, false, "areaA");
  linkStateAreaA.updateAdjacencyDatabase(adjDbNode2AreaA);
  areaLinkStates.emplace("areaA", linkStateAreaA);

  // Area B
  LinkState linkStateAreaB{"areaB"};
  // node1
  auto adj13 = createAdjacency(
      "node3", "1/3", "3/1", "fe80::13", "192.168.0.3", 1, 10003);
  auto adjDbNode1AreaB = createAdjDb("node1", {adj13}, 201, false, "areaB");
  linkStateAreaB.updateAdjacencyDatabase(adjDbNode1AreaB);
  // node2
  auto adj23 = createAdjacency(
      "node3", "2/3", "3/2", "fe80::23", "192.168.0.1", 1, 10003);
  auto adjDbNode2AreaB = createAdjDb("node2", {adj23}, 202, false, "areaB");
  linkStateAreaB.updateAdjacencyDatabase(adjDbNode2AreaB);
  // node3
  auto adj31 = createAdjacency(
      "node1", "3/1", "1/3", "fe80::31", "192.168.0.2", 1, 10003);
  auto adj32 = createAdjacency(
      "node2", "3/2", "2/3", "fe80::32", "192.168.0.2", 1, 10003);
  auto adjDbNode3AreaB =
      createAdjDb("node3", {adj31, adj32}, 203, false, "areaB");
  linkStateAreaB.updateAdjacencyDatabase(adjDbNode3AreaB);
  areaLinkStates.emplace("areaB", linkStateAreaB);

  {
    // Multiple entries. Each node will choose local as best. If a node announce
    // best entry to two areas, choose the one with lower area id
    // (based on std::map key hash)
    std::set<NodeAndArea> nodeAreaSet{
        {"node1", "areaA"}, {"node1", "areaB"}, {"node2", "areaA"}};

    const auto node1bestKey =
        std::make_pair<std::string, std::string>("node1", "areaA");
    EXPECT_EQ(
        node1bestKey, selectBestNodeArea(nodeAreaSet, "node1", areaLinkStates));

    const auto node2bestKey =
        std::make_pair<std::string, std::string>("node2", "areaA");
    EXPECT_EQ(
        node2bestKey, selectBestNodeArea(nodeAreaSet, "node2", areaLinkStates));
  }

  {
    // Multiple entries from areas. Prefer NodeAndArea with smallest IGP
    // distance. IGP distances of node2-node1 are 1/2 in areaA/areaB
    // respectively.
    std::set<NodeAndArea> nodeAreaSet{{"node1", "areaA"}, {"node1", "areaB"}};

    const auto node1bestKey =
        std::make_pair<std::string, std::string>("node1", "areaA");
    EXPECT_EQ(
        node1bestKey, selectBestNodeArea(nodeAreaSet, "node2", areaLinkStates));
  }
}

TEST(UtilTest, BuildInfoConstruction) {
  thrift::BuildInfo info = getBuildInfoThrift();
  EXPECT_EQ(info.buildUser_ref().value(), BuildInfo::getBuildUser());
  EXPECT_EQ(info.buildTime_ref().value(), BuildInfo::getBuildTime());
  EXPECT_EQ(
      info.buildTimeUnix_ref().value(),
      static_cast<int64_t>(BuildInfo::getBuildTimeUnix()));
  EXPECT_EQ(info.buildHost_ref().value(), BuildInfo::getBuildHost());
  EXPECT_EQ(info.buildPath_ref().value(), BuildInfo::getBuildPath());
  EXPECT_EQ(info.buildRevision_ref().value(), BuildInfo::getBuildRevision());
  EXPECT_EQ(
      info.buildRevisionCommitTimeUnix_ref().value(),
      static_cast<int64_t>(BuildInfo::getBuildRevisionCommitTimeUnix()));
  EXPECT_EQ(
      info.buildUpstreamRevision_ref().value(),
      BuildInfo::getBuildUpstreamRevision());
  EXPECT_EQ(
      info.buildUpstreamRevisionCommitTimeUnix_ref().value(),
      BuildInfo::getBuildUpstreamRevisionCommitTimeUnix());
  EXPECT_EQ(
      info.buildPackageName_ref().value(), BuildInfo::getBuildPackageName());
  EXPECT_EQ(
      info.buildPackageVersion_ref().value(),
      BuildInfo::getBuildPackageVersion());
  EXPECT_EQ(
      info.buildPackageRelease_ref().value(),
      BuildInfo::getBuildPackageRelease());
  EXPECT_EQ(info.buildPlatform_ref().value(), BuildInfo::getBuildPlatform());
  EXPECT_EQ(info.buildRule_ref().value(), BuildInfo::getBuildRule());
  EXPECT_EQ(info.buildType_ref().value(), BuildInfo::getBuildType());
  EXPECT_EQ(info.buildTool_ref().value(), BuildInfo::getBuildTool());
  EXPECT_EQ(info.buildMode_ref().value(), BuildInfo::getBuildMode());
}

TEST(UtilTest, AllocPrefixConstruction) {
  thrift::IpPrefix ipPrefix = toIpPrefix("192.0.0.0/8");
  const int64_t prefixLen = 24;
  const int64_t prefixIndex = 4;
  thrift::AllocPrefix allocPrefix =
      createAllocPrefix(ipPrefix, prefixLen, prefixIndex);
  EXPECT_EQ(allocPrefix.seedPrefix_ref().value(), ipPrefix);
  EXPECT_EQ(allocPrefix.allocPrefixLen_ref().value(), prefixLen);
  EXPECT_EQ(allocPrefix.allocPrefixIndex_ref().value(), prefixIndex);
}

TEST(UtilTest, PerfEventConstruction) {
  thrift::PerfEvent perfEvent =
      createPerfEvent("Test Node", "Test Perf Event Construction", 100);
  EXPECT_EQ(perfEvent.nodeName_ref().value(), "Test Node");
  EXPECT_EQ(perfEvent.eventDescr_ref().value(), "Test Perf Event Construction");
  EXPECT_EQ(perfEvent.unixTs_ref().value(), 100);
}

TEST(UtilTest, KvStoreFloodRateConstruction) {
  const int32_t flood_msg_per_sec = 3;
  const int32_t flood_msg_burst_size = 4;
  thrift::KvstoreFloodRate floodRate =
      createKvstoreFloodRate(flood_msg_per_sec, flood_msg_burst_size);
  EXPECT_EQ(floodRate.flood_msg_per_sec_ref().value(), flood_msg_per_sec);
  EXPECT_EQ(floodRate.flood_msg_burst_size_ref().value(), flood_msg_burst_size);
}

TEST(UtilTest, OpenrVersionsConstruction) {
  const int32_t version = 60;
  const int32_t lowestVersion = 30;
  thrift::OpenrVersions openrVersions =
      createOpenrVersions(version, lowestVersion);
  EXPECT_EQ(openrVersions.version_ref().value(), version);
  EXPECT_EQ(openrVersions.lowestSupportedVersion_ref().value(), lowestVersion);
}

TEST(UtilTest, ThriftValueConstruction) {
  const int64_t version = 41;
  const int64_t ttl = 42;
  const int64_t ttlVersion = 43;
  const int64_t hash = 44;

  thrift::Value thriftVal = createThriftValue(
      version, "test originator", "test data", ttl, ttlVersion, hash);
  EXPECT_EQ(thriftVal.version_ref().value(), version);
  EXPECT_EQ(thriftVal.originatorId_ref().value(), "test originator");
  EXPECT_EQ(thriftVal.value_ref().value(), "test data");
  EXPECT_EQ(thriftVal.ttl_ref().value(), ttl);
  EXPECT_EQ(thriftVal.ttlVersion_ref().value(), ttlVersion);
  EXPECT_EQ(thriftVal.hash_ref().value(), hash);
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
