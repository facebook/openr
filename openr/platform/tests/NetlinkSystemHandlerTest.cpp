/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <fbzmq/async/ZmqEventLoop.h>

#include <openr/nl/tests/FakeNetlinkProtocolSocket.h>
#include <openr/platform/NetlinkSystemHandler.h>

using namespace ::testing;
using namespace openr;

namespace {

fbnl::IfAddress
createIfAddress(const int ifIndex, const std::string& addrMask) {
  const auto network = folly::IPAddress::createNetwork(addrMask, -1, false);
  fbnl::IfAddressBuilder builder;
  builder.setIfIndex(ifIndex);
  builder.setPrefix(network);
  if (network.first.isLoopback()) {
    builder.setScope(RT_SCOPE_HOST);
  } else if (network.first.isLinkLocal()) {
    builder.setScope(RT_SCOPE_LINK);
  } else {
    builder.setScope(RT_SCOPE_UNIVERSE);
  }
  return builder.build();
}

fbnl::Link
createLink(
    const int ifIndex,
    const std::string& ifName,
    bool isUp = true,
    bool isLoopback = false) {
  fbnl::LinkBuilder builder;
  builder.setIfIndex(ifIndex);
  builder.setLinkName(ifName);
  if (isUp) {
    builder.setFlags(IFF_RUNNING);
  }
  if (isLoopback) {
    builder.setFlags(IFF_LOOPBACK);
  }
  return builder.build();
}

} // namespace

TEST(SystemHandler, getAllLinks) {
  fbzmq::ZmqEventLoop evl;
  fbnl::FakeNetlinkProtocolSocket nlSock(&evl);
  NetlinkSystemHandler handler(&nlSock);

  // Empty links
  auto links = handler.semifuture_getAllLinks().get();
  EXPECT_EQ(0, links->size());

  // Set addresses and links
  EXPECT_EQ(0, nlSock.addLink(createLink(1, "eth0")).get());
  EXPECT_EQ(0, nlSock.addIfAddress(createIfAddress(1, "192.168.0.3/31")).get());
  EXPECT_EQ(
      -ENXIO, nlSock.addIfAddress(createIfAddress(2, "fc00::3/127")).get());

  // Verify link status and addresses shows up
  links = handler.semifuture_getAllLinks().get();
  ASSERT_EQ(1, links->size());

  const auto& link = links->at(0);
  EXPECT_TRUE(link.isUp);
  EXPECT_EQ("eth0", link.ifName);
  ASSERT_EQ(1, link.networks.size());
  EXPECT_EQ("192.168.0.3/31", toString(link.networks.at(0)));
}

TEST(SystemHandler, addRemoveIfaceAddresses) {
  fbzmq::ZmqEventLoop evl;
  fbnl::FakeNetlinkProtocolSocket nlSock(&evl);
  NetlinkSystemHandler handler(&nlSock);
  const auto ifAddr = createIfAddress(1, "192.168.0.3/31");
  const auto ifPrefix = toIpPrefix(ifAddr.getPrefix().value());
  const std::vector<thrift::IpPrefix> ifPrefixes{ifPrefix};

  // Add link eth0
  EXPECT_EQ(0, nlSock.addLink(createLink(1, "eth0")).get());

  // Add address on eth0 and verify
  {
    auto retval = handler.semifuture_addIfaceAddresses(
        std::make_unique<std::string>(std::string("eth0")),
        std::make_unique<std::vector<thrift::IpPrefix>>(ifPrefixes));
    EXPECT_NO_THROW(std::move(retval).get());
    auto addrs = nlSock.getAllIfAddresses().get();
    ASSERT_EQ(1, addrs.size());
    EXPECT_EQ(ifAddr, addrs.at(0));
  }

  {
    auto retval = handler.semifuture_getIfaceAddresses(
        std::make_unique<std::string>(std::string("eth0")),
        AF_INET,
        RT_SCOPE_UNIVERSE);
    auto addrs = std::move(retval).get();
    ASSERT_EQ(1, addrs->size());
    EXPECT_EQ(ifPrefix, addrs->at(0));
  }

  // Remove address from eth0 and verify
  {
    auto retval = handler.semifuture_removeIfaceAddresses(
        std::make_unique<std::string>(std::string("eth0")),
        std::make_unique<std::vector<thrift::IpPrefix>>(ifPrefixes));
    EXPECT_NO_THROW(std::move(retval).get());
    auto addrs = nlSock.getAllIfAddresses().get();
    EXPECT_EQ(0, addrs.size());
  }
}

TEST(SystemHandler, syncIfaceAddresses) {
  fbzmq::ZmqEventLoop evl;
  fbnl::FakeNetlinkProtocolSocket nlSock(&evl);
  NetlinkSystemHandler handler(&nlSock);
  const auto ifAddr1 = createIfAddress(1, "192.168.1.3/31"); // v4 global
  const auto ifAddr2 = createIfAddress(1, "192.168.2.3/31"); // v4 global
  const auto ifAddr3 = createIfAddress(1, "192.168.3.3/31"); // v4 global
  const auto ifAddr4 = createIfAddress(1, "127.0.0.1/32"); // v4 host
  const auto ifAddr11 = createIfAddress(1, "fc00::3/127"); // v6 global

  // Add link eth0
  EXPECT_EQ(0, nlSock.addLink(createLink(1, "eth0")).get());

  // Add addr2, addr3 and addr11 in nlSock
  EXPECT_EQ(0, nlSock.addIfAddress(ifAddr2).get());
  EXPECT_EQ(0, nlSock.addIfAddress(ifAddr3).get());
  EXPECT_EQ(0, nlSock.addIfAddress(ifAddr4).get());
  EXPECT_EQ(0, nlSock.addIfAddress(ifAddr11).get());

  // Sync addr1 and addr2 for AF_INET family
  {
    std::vector<thrift::IpPrefix> addrs{
        toIpPrefix(ifAddr1.getPrefix().value()),
        toIpPrefix(ifAddr2.getPrefix().value())};
    auto retval = handler.semifuture_syncIfaceAddresses(
        std::make_unique<std::string>(std::string("eth0")),
        AF_INET,
        RT_SCOPE_UNIVERSE,
        std::make_unique<std::vector<thrift::IpPrefix>>(std::move(addrs)));
    EXPECT_NO_THROW(std::move(retval).get());
  }

  // Verify that addr1 is added and addr3 no longer exists. In fake
  // implementation addrs are returned in the order they're added.
  {
    auto addrs = nlSock.getAllIfAddresses().get();
    ASSERT_EQ(4, addrs.size());
    EXPECT_EQ(ifAddr2, addrs.at(0));
    EXPECT_EQ(ifAddr4, addrs.at(1));
    EXPECT_EQ(ifAddr11, addrs.at(2));
    EXPECT_EQ(ifAddr1, addrs.at(3));
  }
}
