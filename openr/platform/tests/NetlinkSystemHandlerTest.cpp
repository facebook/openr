/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/io/async/EventBase.h>

#include <openr/platform/NetlinkSystemHandler.h>
#include <openr/tests/mocks/MockNetlinkProtocolSocket.h>

using namespace ::testing;
using namespace openr;
using namespace openr::fbnl;

TEST(SystemHandler, getAllLinks) {
  folly::EventBase evb;
  fbnl::MockNetlinkProtocolSocket nlSock(&evb);
  NetlinkSystemHandler handler(&nlSock);

  // Empty links
  auto links = handler.semifuture_getAllLinks().get();
  EXPECT_EQ(0, links->size());

  // Set addresses and links
  EXPECT_EQ(0, nlSock.addLink(utils::createLink(1, "eth0")).get());
  EXPECT_EQ(
      0,
      nlSock.addIfAddress(utils::createIfAddress(1, "192.168.0.3/31")).get());
  EXPECT_EQ(
      -ENXIO,
      nlSock.addIfAddress(utils::createIfAddress(2, "fc00::3/127")).get());

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
  folly::EventBase evb;
  fbnl::MockNetlinkProtocolSocket nlSock(&evb);
  NetlinkSystemHandler handler(&nlSock);
  const auto ifAddr = utils::createIfAddress(1, "192.168.0.3/31");
  const auto ifPrefix = toIpPrefix(ifAddr.getPrefix().value());
  const std::vector<thrift::IpPrefix> ifPrefixes{ifPrefix};

  // Add link eth0
  EXPECT_EQ(0, nlSock.addLink(utils::createLink(1, "eth0")).get());

  // Add address on eth0 and verify
  {
    auto retval = handler.semifuture_addIfaceAddresses(
        std::make_unique<std::string>(std::string("eth0")),
        std::make_unique<std::vector<thrift::IpPrefix>>(ifPrefixes));
    EXPECT_NO_THROW(std::move(retval).get());
    auto addrs = nlSock.getAllIfAddresses().get().value();
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
    auto addrs = nlSock.getAllIfAddresses().get().value();
    EXPECT_EQ(0, addrs.size());
  }
}

TEST(SystemHandler, syncIfaceAddresses) {
  folly::EventBase evb;
  fbnl::MockNetlinkProtocolSocket nlSock(&evb);
  NetlinkSystemHandler handler(&nlSock);
  const auto ifAddr1 = utils::createIfAddress(1, "192.168.1.3/31"); // v4 global
  const auto ifAddr2 = utils::createIfAddress(1, "192.168.2.3/31"); // v4 global
  const auto ifAddr3 = utils::createIfAddress(1, "192.168.3.3/31"); // v4 global
  const auto ifAddr4 = utils::createIfAddress(1, "127.0.0.1/32"); // v4 host
  const auto ifAddr11 = utils::createIfAddress(1, "fc00::3/127"); // v6 global

  // Add link eth0
  EXPECT_EQ(0, nlSock.addLink(utils::createLink(1, "eth0")).get());

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
    auto addrs = nlSock.getAllIfAddresses().get().value();
    ASSERT_EQ(4, addrs.size());
    EXPECT_EQ(ifAddr2, addrs.at(0));
    EXPECT_EQ(ifAddr4, addrs.at(1));
    EXPECT_EQ(ifAddr11, addrs.at(2));
    EXPECT_EQ(ifAddr1, addrs.at(3));
  }
}
