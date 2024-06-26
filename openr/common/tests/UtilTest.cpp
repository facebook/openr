/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/MplsUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
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
  std::string area{Constants::kDefaultArea.toString()};
  thrift::IpPrefix ipPrefix;
  folly::IPAddress addr;
  int plen{0};
};

thrift::PrefixEntry
createPrefixEntry(int32_t pp, int32_t sp, int32_t d) {
  thrift::PrefixEntry prefixEntry;
  prefixEntry.metrics() = createMetrics(pp, sp, d);
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

    EXPECT_EQ("10.1.1.1", toString(*prefixV4.prefixAddress()));
    EXPECT_EQ(32, *prefixV4.prefixLength());
    EXPECT_EQ("2620::1", toString(*prefixV6.prefixAddress()));
    EXPECT_EQ(128, *prefixV6.prefixLength());

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
    v4Prefix.prefixAddress() = toBinaryAddress(v4Addr);
    v4Prefix.prefixLength() = invalidV4Len;
    v6Prefix.prefixAddress() = toBinaryAddress(v6Addr);
    v6Prefix.prefixLength() = invalidV6Len;

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
    EXPECT_EQ(v4Prefix.prefixAddress().value(), v4AddrBin);
    EXPECT_EQ(v4Prefix.prefixLength().value(), v4Len);
  }
}

// TODO: migrate PrefixKeyTest into TypesTest with fromStr() validation
/*
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
*/

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
    EXPECT_EQ(perfEvents.events()->size(), 1);
    EXPECT_EQ(*perfEvents.events()[0].nodeName(), "node1");
    EXPECT_EQ(*perfEvents.events()[0].eventDescr(), "LINK_UP");
  }

  {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, "node1", "LINK_UP");
    addPerfEvent(perfEvents, "node2", "LINK_DOWN");
    EXPECT_EQ(perfEvents.events()->size(), 2);
    EXPECT_EQ(*perfEvents.events()[0].nodeName(), "node1");
    EXPECT_EQ(*perfEvents.events()[0].eventDescr(), "LINK_UP");
    EXPECT_EQ(*perfEvents.events()[1].nodeName(), "node2");
    EXPECT_EQ(*perfEvents.events()[1].eventDescr(), "LINK_DOWN");
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
    perfEvents.events()->emplace_back(std::move(event1));
    thrift::PerfEvent event2 = createPerfEvent("node1", "DECISION_RECVD", 200);
    perfEvents.events()->emplace_back(std::move(event2));
    thrift::PerfEvent event3 = createPerfEvent("node1", "SPF_CALCULATE", 300);
    perfEvents.events()->emplace_back(std::move(event3));
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
    perfEvents.events()->emplace_back(std::move(event1));
    thrift::PerfEvent event2 = createPerfEvent("node1", "DECISION_RECVD", 200);
    perfEvents.events()->emplace_back(std::move(event2));
    thrift::PerfEvent event3 = createPerfEvent("node1", "SPF_CALCULATE", 300);
    perfEvents.events()->emplace_back(std::move(event3));
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
  oldRouteDb.thisNodeName() = "node-1";
  oldRouteDb.unicastRoutes()->emplace_back(
      createUnicastRoute(prefix2, {path1_2_1, path1_2_2}));
  oldRouteDb.mplsRoutes()->emplace_back(
      createMplsRoute(2, {path1_2_1_swap, path1_2_2_swap}));

  thrift::RouteDatabase newRouteDb;
  newRouteDb.thisNodeName() = "node-1";
  newRouteDb.unicastRoutes()->emplace_back(
      createUnicastRoute(prefix2, {path1_2_1, path1_2_2, path1_2_3}));
  newRouteDb.mplsRoutes()->emplace_back(
      createMplsRoute(2, {path1_2_1_swap, path1_2_2_swap, path1_2_3_swap}));

  const auto& res1 = findDeltaRoutes(newRouteDb, oldRouteDb);

  EXPECT_EQ(res1.unicastRoutesToUpdate()->size(), 1);
  EXPECT_EQ(*res1.unicastRoutesToUpdate(), *newRouteDb.unicastRoutes());
  EXPECT_EQ(res1.unicastRoutesToDelete()->size(), 0);
  EXPECT_EQ(res1.mplsRoutesToUpdate()->size(), 1);
  EXPECT_EQ(*res1.mplsRoutesToUpdate(), *newRouteDb.mplsRoutes());
  EXPECT_EQ(res1.mplsRoutesToDelete()->size(), 0);

  // add more unicastRoutes in newRouteDb
  newRouteDb.unicastRoutes()->emplace_back(
      createUnicastRoute(prefix3, {path1_3_1, path1_3_2}));
  newRouteDb.mplsRoutes()->emplace_back(
      createMplsRoute(3, {path1_3_1_swap, path1_3_2_swap}));

  const auto& res2 = findDeltaRoutes(newRouteDb, oldRouteDb);
  EXPECT_EQ(res2.unicastRoutesToUpdate()->size(), 2);
  EXPECT_EQ(*res2.unicastRoutesToUpdate(), *newRouteDb.unicastRoutes());
  EXPECT_EQ(res2.unicastRoutesToDelete()->size(), 0);
  EXPECT_EQ(res2.mplsRoutesToUpdate()->size(), 2);
  EXPECT_EQ(*res2.mplsRoutesToUpdate(), *newRouteDb.mplsRoutes());
  EXPECT_EQ(res2.mplsRoutesToDelete()->size(), 0);

  // empty out newRouteDb
  newRouteDb.unicastRoutes()->clear();
  newRouteDb.mplsRoutes()->clear();
  const auto& res3 = findDeltaRoutes(newRouteDb, oldRouteDb);
  EXPECT_EQ(res3.unicastRoutesToUpdate()->size(), 0);
  EXPECT_EQ(res3.unicastRoutesToDelete()->size(), 1);
  EXPECT_EQ(res3.unicastRoutesToDelete()->at(0), prefix2);
  EXPECT_EQ(res3.mplsRoutesToUpdate()->size(), 0);
  EXPECT_EQ(res3.mplsRoutesToDelete()->size(), 1);
  EXPECT_EQ(res3.mplsRoutesToDelete()->at(0), 2);
}

TEST(UtilTest, MplsActionValidate) {
  //
  // PHP
  //
  {
    thrift::MplsAction mplsAction;
    mplsAction.action() = thrift::MplsActionCode::PHP;
    EXPECT_NO_FATAL_FAILURE(checkMplsAction(mplsAction));

    mplsAction.swapLabel() = 1;
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.swapLabel().reset();

    mplsAction.pushLabels() = std::vector<int32_t>();
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.pushLabels().reset();
  }

  //
  // POP_AND_LOOKUP
  //
  {
    thrift::MplsAction mplsAction;
    mplsAction.action() = thrift::MplsActionCode::POP_AND_LOOKUP;
    EXPECT_NO_FATAL_FAILURE(checkMplsAction(mplsAction));

    mplsAction.swapLabel() = 1;
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.swapLabel().reset();

    mplsAction.pushLabels() = std::vector<int32_t>();
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.pushLabels().reset();
  }

  //
  // SWAP
  //
  {
    thrift::MplsAction mplsAction;
    mplsAction.action() = thrift::MplsActionCode::SWAP;
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");

    mplsAction.swapLabel() = 1;
    EXPECT_NO_FATAL_FAILURE(checkMplsAction(mplsAction));

    mplsAction.pushLabels() = std::vector<int32_t>();
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.pushLabels().reset();
  }

  //
  // PUSH
  //
  {
    thrift::MplsAction mplsAction;
    mplsAction.action() = thrift::MplsActionCode::PUSH;
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");

    mplsAction.swapLabel() = 1;
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");
    mplsAction.swapLabel().reset();

    mplsAction.pushLabels() = std::vector<int32_t>();
    EXPECT_DEATH(checkMplsAction(mplsAction), ".*");

    mplsAction.pushLabels()->push_back(1);
    EXPECT_NO_FATAL_FAILURE(checkMplsAction(mplsAction));
  }
}

TEST(UtilTest, hasBestRoutesInAreaTest) {
  PrefixEntries prefixes;

  // Default case (empty entries)
  EXPECT_FALSE(hasBestRoutesInArea("area1", prefixes, {}));

  prefixes[{"node1", "area1"}] = std::make_shared<thrift::PrefixEntry>(
      createPrefixEntry(toIpPrefix("10.0.0.0/8")));
  prefixes[{"node2", "area1"}] = std::make_shared<thrift::PrefixEntry>(
      createPrefixEntry(toIpPrefix("10.0.0.0/8")));
  prefixes[{"node3", "area1"}] = std::make_shared<thrift::PrefixEntry>(
      createPrefixEntry(toIpPrefix("10.0.0.0/8")));

  std::set<NodeAndArea> bestNodeAreas = {
      {"node1", "area1"}, {"node2", "area1"}, {"node3", "area1"}};

  EXPECT_TRUE(hasBestRoutesInArea("area1", prefixes, bestNodeAreas));
  // There are no PrefixEntry with nodes in area2 yet
  EXPECT_FALSE(hasBestRoutesInArea("area2", prefixes, bestNodeAreas));

  EXPECT_TRUE(hasBestRoutesInArea("area1", prefixes, {{"node1", "area1"}}));
  EXPECT_TRUE(hasBestRoutesInArea("area1", prefixes, {{"node2", "area1"}}));

  EXPECT_TRUE(hasBestRoutesInArea("area1", prefixes, {{"node3", "area1"}}));

  EXPECT_TRUE(hasBestRoutesInArea("area1", prefixes, bestNodeAreas));

  //
  // Create a prefix entry with "node1" in "area2"
  //
  prefixes[{"node1", "area2"}] = std::make_shared<thrift::PrefixEntry>(
      createPrefixEntry(toIpPrefix("10.0.0.0/8")));
  // node1 is in both area1 and area2
  EXPECT_TRUE(hasBestRoutesInArea("area2", prefixes, {{"node1", "area2"}}));
  // node2 is not in area2
  EXPECT_FALSE(hasBestRoutesInArea("area2", prefixes, {{"node2", "area2"}}));
  // bestNodeAreas does not have {"node1":"area2"}
  EXPECT_FALSE(hasBestRoutesInArea("area2", prefixes, bestNodeAreas));
  //
  // Create a prefix entry with "node4" in "area2"
  //
  prefixes[{"node4", "area2"}] = std::make_shared<thrift::PrefixEntry>(
      createPrefixEntry(toIpPrefix("10.0.0.0/8")));

  // Insert "node4" in "area2" to bestNodeAreas
  bestNodeAreas.insert({"node4", "area2"});

  EXPECT_TRUE(hasBestRoutesInArea("area1", prefixes, bestNodeAreas));
  EXPECT_TRUE(hasBestRoutesInArea("area2", prefixes, bestNodeAreas));
  // PrefixEntry has node4 is in area2, but no entry of node4 in area1
  EXPECT_FALSE(hasBestRoutesInArea("area1", prefixes, {{"node4", "area1"}}));
  EXPECT_TRUE(hasBestRoutesInArea("area2", prefixes, {{"node4", "area2"}}));
  // this {"node4", "area1"} does not match any prefix entry
  EXPECT_FALSE(hasBestRoutesInArea("area1", prefixes, {{"node4", "area1"}}));
  EXPECT_FALSE(hasBestRoutesInArea("area2", prefixes, {{"node4", "area1"}}));
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

  //
  // Multiple entries. Each node will choose local as best. If a node announce
  // best entry to two areas, choose the one with lower area id
  // (based on std::map key hash)
  //
  {
    std::unordered_map<NodeAndArea, thrift::PrefixEntry> prefixes = {
        {{"node1", "area1"}, createPrefixEntry(100, 10, 1)},
        {{"node1", "area2"}, createPrefixEntry(100, 10, 1)},
        {{"node2", "area1"}, createPrefixEntry(100, 10, 1)}};
    const auto bestKeys = selectBestPrefixMetrics(prefixes);
    EXPECT_EQ(3, bestKeys.size());
    const auto node1bestKey =
        std::make_pair<std::string, std::string>("node1", "area1");
    EXPECT_EQ(node1bestKey, selectBestNodeArea(bestKeys, "node1"));
    const auto node2bestKey =
        std::make_pair<std::string, std::string>("node2", "area1");
    EXPECT_EQ(node2bestKey, selectBestNodeArea(bestKeys, "node2"));
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

TEST(UtilTest, BuildInfoConstruction) {
  thrift::BuildInfo info = getBuildInfoThrift();
  EXPECT_EQ(info.buildUser().value(), BuildInfo::getBuildUser());
  EXPECT_EQ(info.buildTime().value(), BuildInfo::getBuildTime());
  EXPECT_EQ(
      info.buildTimeUnix().value(),
      static_cast<int64_t>(BuildInfo::getBuildTimeUnix()));
  EXPECT_EQ(info.buildHost().value(), BuildInfo::getBuildHost());
  EXPECT_EQ(info.buildPath().value(), BuildInfo::getBuildPath());
  EXPECT_EQ(info.buildRevision().value(), BuildInfo::getBuildRevision());
  EXPECT_EQ(
      info.buildRevisionCommitTimeUnix().value(),
      static_cast<int64_t>(BuildInfo::getBuildRevisionCommitTimeUnix()));
  EXPECT_EQ(
      info.buildUpstreamRevision().value(),
      BuildInfo::getBuildUpstreamRevision());
  EXPECT_EQ(
      info.buildUpstreamRevisionCommitTimeUnix().value(),
      BuildInfo::getBuildUpstreamRevisionCommitTimeUnix());
  EXPECT_EQ(info.buildPackageName().value(), BuildInfo::getBuildPackageName());
  EXPECT_EQ(
      info.buildPackageVersion().value(), BuildInfo::getBuildPackageVersion());
  EXPECT_EQ(
      info.buildPackageRelease().value(), BuildInfo::getBuildPackageRelease());
  EXPECT_EQ(info.buildPlatform().value(), BuildInfo::getBuildPlatform());
  EXPECT_EQ(info.buildRule().value(), BuildInfo::getBuildRule());
  EXPECT_EQ(info.buildType().value(), BuildInfo::getBuildType());
  EXPECT_EQ(info.buildTool().value(), BuildInfo::getBuildTool());
  EXPECT_EQ(info.buildMode().value(), BuildInfo::getBuildMode());
}

TEST(UtilTest, PerfEventConstruction) {
  thrift::PerfEvent perfEvent =
      createPerfEvent("Test Node", "Test Perf Event Construction", 100);
  EXPECT_EQ(perfEvent.nodeName().value(), "Test Node");
  EXPECT_EQ(perfEvent.eventDescr().value(), "Test Perf Event Construction");
  EXPECT_EQ(perfEvent.unixTs().value(), 100);
}

TEST(UtilTest, KvStoreFloodRateConstruction) {
  const int32_t flood_msg_per_sec = 3;
  const int32_t flood_msg_burst_size = 4;
  thrift::KvStoreFloodRate floodRate =
      createKvStoreFloodRate(flood_msg_per_sec, flood_msg_burst_size);
  EXPECT_EQ(floodRate.flood_msg_per_sec().value(), flood_msg_per_sec);
  EXPECT_EQ(floodRate.flood_msg_burst_size().value(), flood_msg_burst_size);
}

TEST(UtilTest, OpenrVersionsConstruction) {
  const int32_t version = 60;
  const int32_t lowestVersion = 30;
  thrift::OpenrVersions openrVersions =
      createOpenrVersions(version, lowestVersion);
  EXPECT_EQ(openrVersions.version().value(), version);
  EXPECT_EQ(openrVersions.lowestSupportedVersion().value(), lowestVersion);
}

TEST(UtilTest, ThriftValueConstruction) {
  const int64_t version = 41;
  const int64_t ttl = 42;
  const int64_t ttlVersion = 43;
  const int64_t hash = 44;

  thrift::Value thriftVal = createThriftValue(
      version, "test originator", "test data", ttl, ttlVersion, hash);
  EXPECT_EQ(thriftVal.version().value(), version);
  EXPECT_EQ(thriftVal.originatorId().value(), "test originator");
  EXPECT_EQ(thriftVal.value().value(), "test data");
  EXPECT_EQ(thriftVal.ttl().value(), ttl);
  EXPECT_EQ(thriftVal.ttlVersion().value(), ttlVersion);
  EXPECT_EQ(thriftVal.hash().value(), hash);
}

TEST(UtilTest, logInitializationEvent) {
  logInitializationEvent("Main", thrift::InitializationEvent::AGENT_CONFIGURED);
  EXPECT_TRUE(facebook::fb303::fbData->hasCounter(
      "initialization.AGENT_CONFIGURED.duration_ms"));
  EXPECT_FALSE(facebook::fb303::fbData->hasCounter(
      "initialization.KVSTORE_SYNCED.duration_ms"));

  logInitializationEvent(
      "moduleA",
      thrift::InitializationEvent::KVSTORE_SYNCED,
      "this is a message.");
  EXPECT_TRUE(facebook::fb303::fbData->hasCounter(
      "initialization.KVSTORE_SYNCED.duration_ms"));
}

TEST(UtilTest, ToStringTest) {
  {
    auto param = std::make_unique<thrift::KeySetParams>();
    param->senderId() = "node1";
    openr::thrift::KeyVals kvs{
        {"key1", createThriftValue(1, "orig1", "data1")}};
    param->keyVals() = kvs;
    auto res = toString(*param.get());
    EXPECT_EQ(
        res,
        "key: key1 version: 1 originatorId: orig1 ttl: -2147483648 ttlVersion: 0 senderId: node1");
  }
  {
    auto param = std::make_unique<thrift::KeyDumpParams>();
    param->senderId() = "node1";
    param->originatorIds() = {"o1", "o2", "o3"};
    param->keys() = {"k1", "k2", "k3"};
    auto res = toString(*param.get());
    EXPECT_EQ(
        res,
        "originatorIds: o1 o2 o3 \nignore ttl: 1\nkeys: k1 k2 k3 senderId: node1");
  }
  {
    std::vector<std::string> input{"hello", "world", "facebook"};
    const auto result = toString(input);
    EXPECT_EQ(result, "hello world facebook ");
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
