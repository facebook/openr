/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <forward_list>
#include <thread>

#include <folly/Memory.h>
#include <folly/SocketAddress.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/tests/mocks/MockIoProvider.h>
#include <openr/tests/mocks/MockIoProviderUtils.h>

namespace {

const int kMinIpv6PktSize{1280};
const int kMockedUdpPort{6666};
const int kPollTimeout{10};
const int kRecvBufferSize{1024};
const std::string kDiscardMulticastAddr("ff01::1");

void
checkPacketContent(const std::string& packet, const struct msghdr& msg) {
  EXPECT_EQ(
      packet,
      std::string(
          reinterpret_cast<const char*>(msg.msg_iov->iov_base), packet.size()));
}

void
waitForDataToRead(const int fd) {
  while (true) {
    folly::netops::PollDescriptor pfd;
    pfd.fd = folly::NetworkSocket::fromFd(fd);
    pfd.events = POLLIN;
    int ret = folly::netops::poll(
        &pfd, 1, -1 /* negative value means infinite timeout */);

    if (ret > 0) {
      return;
    } else if (ret == EINTR) {
      // A signal occurred while the system call was in progress.
      // No error actually occurred.
      continue;
    } else {
      throw folly::AsyncSocketException(
          folly::AsyncSocketException::INTERNAL_ERROR,
          fmt::format("poll fd: {} failed", fd));
    }
  }
}
} // anonymous namespace.

using namespace openr;
using namespace openr::MockIoProviderUtils;
//
// This test sends packets along the follow topology.
//
// 3-node topology: 1 <-> 2 (2-node bidirectional)
//                  3 (1-node island)
//
TEST(MockIoProviderTestSetup, TwoNodesAndOneNodeTest) {
  folly::IPAddressV6 ipAddr1V6("fe80::1");
  folly::IPAddressV6 ipAddr2V6("fe80::2");
  folly::IPAddressV6 ipAddr3V6("fe80::3");

  std::string ifName1("iface1");
  std::string ifName2("iface2");
  std::string ifName3("iface3");

  int ifIndex1 = 1;
  int ifIndex2 = 2;
  int ifIndex3 = 3;

  auto mockIoProvider = std::make_shared<MockIoProvider>();

  // Start mock IoProvider thread
  std::thread mockIoProviderThread([&]() {
    LOG(INFO) << "Starting mockIoProvider thread.";
    mockIoProvider->start();
    LOG(INFO) << "mockIoProvider thread got stopped.";
  });
  mockIoProvider->waitUntilRunning();

  mockIoProvider->addIfNameIfIndex(
      {{ifName1, ifIndex1}, {ifName2, ifIndex2}, {ifName3, ifIndex3}});

  // Bidirectional connectivity between 1 and 2, while 3 just an island.
  ConnectedIfPairs connectedPairs = {
      {ifName1, {{ifName2, 100}}},
      {ifName2, {{ifName1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  int fd1 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex1, folly::IPAddress(kDiscardMulticastAddr));

  int fd2 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex2, folly::IPAddress(kDiscardMulticastAddr));

  int fd3 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex3, folly::IPAddress(kDiscardMulticastAddr));

  struct msghdr sendMsg;
  AlignedCtrlBuf<struct in6_pktinfo> sendUnion;
  sockaddr_storage dstAddrStorage;
  struct iovec sendEntry;
  std::string packet1(
      "This is a single-destination multicast message #1 from node1 to node2.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet1.c_str()),
          .len = packet1.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex1,
          .srcIPAddr = ipAddr1V6,
          .dstIPAddr = ipAddr2V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      (openr::MockIoProviderUtils::bufferArgs<char[kRecvBufferSize]>){
          .msg = recvMsg,
          .data = recvBuf,
          .len = kMinIpv6PktSize,
          .entry = recvEntry,
          .u = recvUnion,
      },
      srcAddrStorage);

  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);

  std::string packet2(
      "This is a single-destination multicast message #2 in reverse direction "
      "from node2 to node1.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet2.c_str()),
          .len = packet2.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex2,
          .srcIPAddr = ipAddr2V6,
          .dstIPAddr = ipAddr1V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet2.size(), mockIoProvider->sendmsg(fd2, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);

  // Cleanup
  mockIoProvider->stop();
  mockIoProviderThread.join();
}

//
// This test sends packets along the follow topology
// with the two nodes running in separate thread for
// checking concurrent packet transfer.
//
// 2-node topology: 1 <-> 2 (2-node bidirectional)
//
TEST(MockIoProviderTestSetup, TwoConcurrentNodesTest) {
  folly::IPAddressV6 ipAddr1V6("fe80::1");
  folly::IPAddressV6 ipAddr2V6("fe80::2");

  std::string ifName1("iface1");
  std::string ifName2("iface2");

  int ifIndex1 = 1;
  int ifIndex2 = 2;

  auto mockIoProvider = std::make_shared<MockIoProvider>();

  // Start mock IoProvider thread
  std::thread mockIoProviderThread([&]() {
    LOG(INFO) << "Starting mockIoProvider thread.";
    mockIoProvider->start();
    LOG(INFO) << "mockIoProvider thread got stopped.";
  });
  mockIoProvider->waitUntilRunning();

  mockIoProvider->addIfNameIfIndex({{ifName1, ifIndex1}, {ifName2, ifIndex2}});

  // Bidirectional connectivity between 1 and 2.
  ConnectedIfPairs connectedPairs = {
      {ifName1, {{ifName2, 100}}},
      {ifName2, {{ifName1, 100}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  int fd1 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex1, folly::IPAddress(kDiscardMulticastAddr));

  int fd2 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex2, folly::IPAddress(kDiscardMulticastAddr));

  std::string packet1(
      "This is a single-destination multicast message #1 from node1 to node2.");
  std::string packet2(
      "This is a single-destination multicast message #2 in reverse direction "
      "from node2 to node1.");
  std::string packet3(
      "This is yet another single-destined multicast mesg #3 from N2 to N1.");

  std::thread childThread([&]() {
    LOG(INFO) << "Starting child thread...";

    struct msghdr sendMsg;
    AlignedCtrlBuf<struct in6_pktinfo> sendUnion;
    sockaddr_storage dstAddrStorage;
    struct iovec sendEntry;

    // Send packet2.
    prepareSendMessage(
        (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
            .msg = sendMsg,
            .data = const_cast<char*>(packet2.c_str()),
            .len = packet2.size(),
            .entry = sendEntry,
            .u = sendUnion,
        },
        (struct openr::MockIoProviderUtils::networkArgs){
            .srcIfIndex = ifIndex2,
            .srcIPAddr = ipAddr2V6,
            .dstIPAddr = ipAddr1V6,
            .dstPort = kMockedUdpPort,
            .dstAddrStorage = dstAddrStorage,
        });
    EXPECT_EQ(
        packet2.size(), mockIoProvider->sendmsg(fd2, &sendMsg, MSG_DONTWAIT));

    struct msghdr recvMsg;
    unsigned char recvBuf[kMinIpv6PktSize];
    struct iovec recvEntry;
    AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
    sockaddr_storage srcAddrStorage;

    // To receive packet1.
    prepareRecvMessage(
        (openr::MockIoProviderUtils::bufferArgs<char[kRecvBufferSize]>){
            .msg = recvMsg,
            .data = recvBuf,
            .len = kMinIpv6PktSize,
            .entry = recvEntry,
            .u = recvUnion,
        },
        srcAddrStorage);

    // Wait for data availability for read.
    waitForDataToRead(fd2);

    // Receiving packet1 and checking.
    EXPECT_EQ(
        packet1.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
    checkPacketContent(packet1, recvMsg);
    EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));

    // Sanity check as ifName1 would only send out 1 mesg.
    EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));

    // Send packet3.
    MockIoProviderUtils::prepareSendMessage(
        (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
            .msg = sendMsg,
            .data = const_cast<char*>(packet3.c_str()),
            .len = packet3.size(),
            .entry = sendEntry,
            .u = sendUnion,
        },
        (struct openr::MockIoProviderUtils::networkArgs){
            .srcIfIndex = ifIndex2,
            .srcIPAddr = ipAddr2V6,
            .dstIPAddr = ipAddr1V6,
            .dstPort = kMockedUdpPort,
            .dstAddrStorage = dstAddrStorage,
        });
    EXPECT_EQ(
        packet3.size(), mockIoProvider->sendmsg(fd2, &sendMsg, MSG_DONTWAIT));

    // Sanity check again.
    EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));

    LOG(INFO) << "Stopping child thread...";
  });

  struct msghdr sendMsg;
  MockIoProviderUtils::AlignedCtrlBuf<struct in6_pktinfo> sendUnion;
  sockaddr_storage dstAddrStorage;
  struct iovec sendEntry;

  // Send packet1.
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet1.c_str()),
          .len = packet1.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex1,
          .srcIPAddr = ipAddr1V6,
          .dstIPAddr = ipAddr2V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;

  // To receive packet2.
  prepareRecvMessage(
      (openr::MockIoProviderUtils::bufferArgs<char[kRecvBufferSize]>){
          .msg = recvMsg,
          .data = recvBuf,
          .len = kMinIpv6PktSize,
          .entry = recvEntry,
          .u = recvUnion,
      },
      srcAddrStorage);

  // Wait for data availability for read.
  waitForDataToRead(fd1);

  // Receiving packet2 and checking.
  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // To receive packet3.
  // Wait for data availability for read.
  waitForDataToRead(fd1);

  // Receiving packet3 and checking.
  EXPECT_EQ(
      packet3.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet3, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // Sanity check on no more packets.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));

  childThread.join();

  // Cleanup
  mockIoProvider->stop();
  mockIoProviderThread.join();
}

//
// This test sends packets along the follow topology.
//
// 4-node full ring topology: 1 <-> 2
//                            ^     ^
//                            |     |
//                            v     v
//                            4 <-> 3
// Each node has a 2-degree outgoing connection with adjacent neighbours.
//
TEST(MockIoProviderTestSetup, DuplexFullRingOfNodesTest) {
  folly::IPAddressV6 ipAddr1V6("fe80::1");
  folly::IPAddressV6 ipAddr2V6("fe80::2");
  folly::IPAddressV6 ipAddr3V6("fe80::3");
  folly::IPAddressV6 ipAddr4V6("fe80::4");

  std::string ifName1("iface1");
  std::string ifName2("iface2");
  std::string ifName3("iface3");
  std::string ifName4("iface4");

  int ifIndex1 = 1;
  int ifIndex2 = 2;
  int ifIndex3 = 3;
  int ifIndex4 = 4;

  auto mockIoProvider = std::make_shared<MockIoProvider>();

  // Start mock IoProvider thread
  std::thread mockIoProviderThread([&]() {
    LOG(INFO) << "Starting mockIoProvider thread.";
    mockIoProvider->start();
    LOG(INFO) << "mockIoProvider thread got stopped.";
  });
  mockIoProvider->waitUntilRunning();

  mockIoProvider->addIfNameIfIndex(
      {{ifName1, ifIndex1},
       {ifName2, ifIndex2},
       {ifName3, ifIndex3},
       {ifName4, ifIndex4}});

  // Bidirectional connectivity between 1 and 2, while 3 just an island.
  ConnectedIfPairs connectedPairs = {
      {ifName1, {{ifName2, 10}, {ifName4, 10}}},
      {ifName2, {{ifName3, 10}, {ifName1, 10}}},
      {ifName3, {{ifName4, 10}, {ifName2, 10}}},
      {ifName4, {{ifName1, 10}, {ifName3, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  int fd1 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex1, folly::IPAddress(kDiscardMulticastAddr));

  int fd2 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex2, folly::IPAddress(kDiscardMulticastAddr));

  int fd3 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex3, folly::IPAddress(kDiscardMulticastAddr));

  int fd4 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex4, folly::IPAddress(kDiscardMulticastAddr));

  struct msghdr sendMsg;
  AlignedCtrlBuf<struct in6_pktinfo> sendUnion;
  sockaddr_storage dstAddrStorage;
  struct iovec sendEntry;
  std::string packet1(
      "This is a multicast message #1 from node1 to node2 and node4.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet1.c_str()),
          .len = packet1.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex1,
          .srcIPAddr = ipAddr1V6,
          .dstIPAddr = ipAddr2V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      (openr::MockIoProviderUtils::bufferArgs<char[kRecvBufferSize]>){
          .msg = recvMsg,
          .data = recvBuf,
          .len = kMinIpv6PktSize,
          .entry = recvEntry,
          .u = recvUnion,
      },
      srcAddrStorage);

  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex4, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);

  std::string packet2(
      "This is a multicast message #2 from node2 to node1 + node3.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet2.c_str()),
          .len = packet2.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex2,
          .srcIPAddr = ipAddr2V6,
          .dstIPAddr = ipAddr3V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet2.size(), mockIoProvider->sendmsg(fd2, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex3, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet3(
      "This is a multicast message #3 from node3 to nodes 4 & 2.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet3.c_str()),
          .len = packet3.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex3,
          .srcIPAddr = ipAddr3V6,
          .dstIPAddr = ipAddr4V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet3.size(), mockIoProvider->sendmsg(fd3, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet3.size(), mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet3, recvMsg);
  EXPECT_EQ(ifIndex4, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet3.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet3, recvMsg);
  EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet4(
      "This is a multicast message #4 from node4 to nodes:1/3.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet4.c_str()),
          .len = packet4.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex4,
          .srcIPAddr = ipAddr4V6,
          .dstIPAddr = ipAddr1V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet4.size(), mockIoProvider->sendmsg(fd4, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet4.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet4, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet4.size(), mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet4, recvMsg);
  EXPECT_EQ(ifIndex3, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  // Cleanup
  mockIoProvider->stop();
  mockIoProviderThread.join();
}

//
// This test sends packets along the follow topology.
//
// 4-node partial ring topology: 1 <-> 2
//                               ^     |
//                               |     |
//                               |     v
//                               4 --> 3
// The nodes has non-uniform outgoing degree of connectivity.
//
TEST(MockIoProviderTestSetup, PartialRingOfNodesTest) {
  folly::IPAddressV6 ipAddr1V6("fe80::1");
  folly::IPAddressV6 ipAddr2V6("fe80::2");
  folly::IPAddressV6 ipAddr3V6("fe80::3");
  folly::IPAddressV6 ipAddr4V6("fe80::4");

  std::string ifName1("iface1");
  std::string ifName2("iface2");
  std::string ifName3("iface3");
  std::string ifName4("iface4");

  int ifIndex1 = 1;
  int ifIndex2 = 2;
  int ifIndex3 = 3;
  int ifIndex4 = 4;

  auto mockIoProvider = std::make_shared<MockIoProvider>();

  // Start mock IoProvider thread
  std::thread mockIoProviderThread([&]() {
    LOG(INFO) << "Starting mockIoProvider thread.";
    mockIoProvider->start();
    LOG(INFO) << "mockIoProvider thread got stopped.";
  });
  mockIoProvider->waitUntilRunning();

  mockIoProvider->addIfNameIfIndex(
      {{ifName1, ifIndex1},
       {ifName2, ifIndex2},
       {ifName3, ifIndex3},
       {ifName4, ifIndex4}});

  // Bidirectional connectivity between 1 and 2, while 3 just an island.
  ConnectedIfPairs connectedPairs = {
      {ifName1, {{ifName2, 10}}},
      {ifName2, {{ifName3, 10}, {ifName1, 10}}},
      {ifName4, {{ifName1, 10}, {ifName3, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  int fd1 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex1, folly::IPAddress(kDiscardMulticastAddr));

  int fd2 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex2, folly::IPAddress(kDiscardMulticastAddr));

  int fd3 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex3, folly::IPAddress(kDiscardMulticastAddr));

  int fd4 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex4, folly::IPAddress(kDiscardMulticastAddr));

  struct msghdr sendMsg;
  AlignedCtrlBuf<struct in6_pktinfo> sendUnion;
  sockaddr_storage dstAddrStorage;
  struct iovec sendEntry;
  std::string packet1(
      "This is a single-destination multicast message #1 from node1 just to "
      "node2 only.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet1.c_str()),
          .len = packet1.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex1,
          .srcIPAddr = ipAddr1V6,
          .dstIPAddr = ipAddr2V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      (openr::MockIoProviderUtils::bufferArgs<char[kRecvBufferSize]>){
          .msg = recvMsg,
          .data = recvBuf,
          .len = kMinIpv6PktSize,
          .entry = recvEntry,
          .u = recvUnion,
      },
      srcAddrStorage);

  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);

  std::string packet2(
      "This is a multicast message #2 from node2 to node1 + node3.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet2.c_str()),
          .len = packet2.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex2,
          .srcIPAddr = ipAddr2V6,
          .dstIPAddr = ipAddr3V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet2.size(), mockIoProvider->sendmsg(fd2, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex3, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);

  std::string packet3("This is an anycast message #3 from node3 to nowhere.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet3.c_str()),
          .len = packet3.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex3,
          .srcIPAddr = ipAddr3V6,
          .dstIPAddr = ipAddr4V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(-1, mockIoProvider->sendmsg(fd3, &sendMsg, MSG_DONTWAIT));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);

  std::string packet4(
      "This is a multicast message #4 from node4 to nodes 1, 3.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet4.c_str()),
          .len = packet4.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex4,
          .srcIPAddr = ipAddr4V6,
          .dstIPAddr = ipAddr1V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet4.size(), mockIoProvider->sendmsg(fd4, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet4.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet4, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet4.size(), mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet4, recvMsg);
  EXPECT_EQ(ifIndex3, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);

  // Cleanup
  mockIoProvider->stop();
  mockIoProviderThread.join();
}

//
// This test sends packets along the follow topology.
//
// 4-node full star topology: 2 <-> 1 <-> 3
//                                  ^
//                                  |
//                                  v
//                                  4
// Each node has a 1-degree outgoing connection except node 1, which has
// 3-degree outgoing connectivities.
//
TEST(MockIoProviderTestSetup, DuplexStarOfNodesTest) {
  folly::IPAddressV6 ipAddr1V6("fe80::1");
  folly::IPAddressV6 ipAddr2V6("fe80::2");
  folly::IPAddressV6 ipAddr3V6("fe80::3");
  folly::IPAddressV6 ipAddr4V6("fe80::4");

  std::string ifName1("iface1");
  std::string ifName2("iface2");
  std::string ifName3("iface3");
  std::string ifName4("iface4");

  int ifIndex1 = 1;
  int ifIndex2 = 2;
  int ifIndex3 = 3;
  int ifIndex4 = 4;

  auto mockIoProvider = std::make_shared<MockIoProvider>();

  // Start mock IoProvider thread
  std::thread mockIoProviderThread([&]() {
    LOG(INFO) << "Starting mockIoProvider thread.";
    mockIoProvider->start();
    LOG(INFO) << "mockIoProvider thread got stopped.";
  });
  mockIoProvider->waitUntilRunning();

  mockIoProvider->addIfNameIfIndex(
      {{ifName1, ifIndex1},
       {ifName2, ifIndex2},
       {ifName3, ifIndex3},
       {ifName4, ifIndex4}});

  // Bidirectional connectivity between 1 and 2, while 3 just an island.
  ConnectedIfPairs connectedPairs = {
      {ifName1, {{ifName2, 10}, {ifName3, 10}, {ifName4, 10}}},
      {ifName2, {{ifName1, 10}}},
      {ifName3, {{ifName1, 10}}},
      {ifName4, {{ifName1, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  int fd1 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex1, folly::IPAddress(kDiscardMulticastAddr));

  int fd2 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex2, folly::IPAddress(kDiscardMulticastAddr));

  int fd3 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex3, folly::IPAddress(kDiscardMulticastAddr));

  int fd4 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex4, folly::IPAddress(kDiscardMulticastAddr));

  struct msghdr sendMsg;
  AlignedCtrlBuf<struct in6_pktinfo> sendUnion;
  sockaddr_storage dstAddrStorage;
  struct iovec sendEntry;
  std::string packet1(
      "This is a multicast message #1 from node1 to node2, node3, and node4.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet1.c_str()),
          .len = packet1.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex1,
          .srcIPAddr = ipAddr1V6,
          .dstIPAddr = ipAddr2V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      (openr::MockIoProviderUtils::bufferArgs<char[kRecvBufferSize]>){
          .msg = recvMsg,
          .data = recvBuf,
          .len = kMinIpv6PktSize,
          .entry = recvEntry,
          .u = recvUnion,
      },
      srcAddrStorage);

  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex3, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex4, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet2(
      "This is a single-destination multicast message #2 from node2 to node1.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet2.c_str()),
          .len = packet2.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex2,
          .srcIPAddr = ipAddr2V6,
          .dstIPAddr = ipAddr1V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet2.size(), mockIoProvider->sendmsg(fd2, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet3(
      "This is a single-destination multicast message #3 from node3 to node "
      "#1.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet3.c_str()),
          .len = packet3.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex3,
          .srcIPAddr = ipAddr3V6,
          .dstIPAddr = ipAddr1V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet3.size(), mockIoProvider->sendmsg(fd3, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet3.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet3, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet4(
      "This is a single-destination multicast message #4 from node4 to node "
      "no. 1 the origin.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet4.c_str()),
          .len = packet4.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex4,
          .srcIPAddr = ipAddr4V6,
          .dstIPAddr = ipAddr1V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet4.size(), mockIoProvider->sendmsg(fd4, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet4.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet4, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  // Cleanup
  mockIoProvider->stop();
  mockIoProviderThread.join();
}

//
// This test sends packets along the follow topology.
//
// 4-node partial star topology: 2 <-> 1 --> 3
//                                     ^
//                                     |
//                                     |
//                                     4
// Some connectivities are missing from the prior full star topo.
//
TEST(MockIoProviderTestSetup, PartialStarOfNodesTest) {
  folly::IPAddressV6 ipAddr1V6("fe80::1");
  folly::IPAddressV6 ipAddr2V6("fe80::2");
  folly::IPAddressV6 ipAddr3V6("fe80::3");
  folly::IPAddressV6 ipAddr4V6("fe80::4");

  std::string ifName1("iface1");
  std::string ifName2("iface2");
  std::string ifName3("iface3");
  std::string ifName4("iface4");

  int ifIndex1 = 1;
  int ifIndex2 = 2;
  int ifIndex3 = 3;
  int ifIndex4 = 4;

  auto mockIoProvider = std::make_shared<MockIoProvider>();

  // Start mock IoProvider thread
  std::thread mockIoProviderThread([&]() {
    LOG(INFO) << "Starting mockIoProvider thread.";
    mockIoProvider->start();
    LOG(INFO) << "mockIoProvider thread got stopped.";
  });
  mockIoProvider->waitUntilRunning();

  mockIoProvider->addIfNameIfIndex(
      {{ifName1, ifIndex1},
       {ifName2, ifIndex2},
       {ifName3, ifIndex3},
       {ifName4, ifIndex4}});

  // Bidirectional connectivity between 1 and 2, while 3 just an island.
  ConnectedIfPairs connectedPairs = {
      {ifName1, {{ifName2, 10}, {ifName3, 10}}},
      {ifName2, {{ifName1, 10}}},
      {ifName4, {{ifName1, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  int fd1 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex1, folly::IPAddress(kDiscardMulticastAddr));

  int fd2 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex2, folly::IPAddress(kDiscardMulticastAddr));

  int fd3 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex3, folly::IPAddress(kDiscardMulticastAddr));

  int fd4 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex4, folly::IPAddress(kDiscardMulticastAddr));

  struct msghdr sendMsg;
  AlignedCtrlBuf<struct in6_pktinfo> sendUnion;
  sockaddr_storage dstAddrStorage;
  struct iovec sendEntry;
  std::string packet1(
      "This is a multicast message #1 from node1 to node2 & node3.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet1.c_str()),
          .len = packet1.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex1,
          .srcIPAddr = ipAddr1V6,
          .dstIPAddr = ipAddr2V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      (openr::MockIoProviderUtils::bufferArgs<char[kRecvBufferSize]>){
          .msg = recvMsg,
          .data = recvBuf,
          .len = kMinIpv6PktSize,
          .entry = recvEntry,
          .u = recvUnion,
      },
      srcAddrStorage);

  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex3, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);

  std::string packet2(
      "This is a single-destination multicast message #2 from node 2 to node "
      "1.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet2.c_str()),
          .len = packet2.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex2,
          .srcIPAddr = ipAddr2V6,
          .dstIPAddr = ipAddr1V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet2.size(), mockIoProvider->sendmsg(fd2, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);

  std::string packet3(
      "This is an anycast message no.3 from node no.3 to nowhere.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet3.c_str()),
          .len = packet3.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex3,
          .srcIPAddr = ipAddr3V6,
          .dstIPAddr = ipAddr1V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(-1, mockIoProvider->sendmsg(fd3, &sendMsg, MSG_DONTWAIT));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);

  std::string packet4(
      "This is a single-destination multicast message no.4 from node no.4 to "
      "node no.1.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet4.c_str()),
          .len = packet4.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex4,
          .srcIPAddr = ipAddr4V6,
          .dstIPAddr = ipAddr1V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet4.size(), mockIoProvider->sendmsg(fd4, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet4.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet4, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);

  // Cleanup
  mockIoProvider->stop();
  mockIoProviderThread.join();
}

//
// This test sends packets along the follow topology.
//
//                            +-----------+
//                            |           |
//                            v           v
// 4-node full star topology: 2 <-> 1 <-> 3
//                            ^     ^     ^
//                            |     |     |
//                            |     v     |
//                            +---> 4 <---+
// Each node has 3-degree outgoing connectivities.
//
TEST(MockIoProviderTestSetup, DuplexFullMeshOfNodesTest) {
  folly::IPAddressV6 ipAddr1V6("fe80::1");
  folly::IPAddressV6 ipAddr2V6("fe80::2");
  folly::IPAddressV6 ipAddr3V6("fe80::3");
  folly::IPAddressV6 ipAddr4V6("fe80::4");

  std::string ifName1("iface1");
  std::string ifName2("iface2");
  std::string ifName3("iface3");
  std::string ifName4("iface4");

  int ifIndex1 = 1;
  int ifIndex2 = 2;
  int ifIndex3 = 3;
  int ifIndex4 = 4;

  auto mockIoProvider = std::make_shared<MockIoProvider>();

  // Start mock IoProvider thread
  std::thread mockIoProviderThread([&]() {
    LOG(INFO) << "Starting mockIoProvider thread.";
    mockIoProvider->start();
    LOG(INFO) << "mockIoProvider thread got stopped.";
  });
  mockIoProvider->waitUntilRunning();

  mockIoProvider->addIfNameIfIndex(
      {{ifName1, ifIndex1},
       {ifName2, ifIndex2},
       {ifName3, ifIndex3},
       {ifName4, ifIndex4}});

  // Bidirectional connectivity between 1 and 2, while 3 just an island.
  ConnectedIfPairs connectedPairs = {
      {ifName1, {{ifName2, 10}, {ifName3, 10}, {ifName4, 10}}},
      {ifName2, {{ifName3, 10}, {ifName4, 10}, {ifName1, 10}}},
      {ifName3, {{ifName4, 10}, {ifName1, 10}, {ifName2, 10}}},
      {ifName4, {{ifName1, 10}, {ifName2, 10}, {ifName3, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  int fd1 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex1, folly::IPAddress(kDiscardMulticastAddr));

  int fd2 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex2, folly::IPAddress(kDiscardMulticastAddr));

  int fd3 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex3, folly::IPAddress(kDiscardMulticastAddr));

  int fd4 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex4, folly::IPAddress(kDiscardMulticastAddr));

  struct msghdr sendMsg;
  AlignedCtrlBuf<struct in6_pktinfo> sendUnion;
  sockaddr_storage dstAddrStorage;
  struct iovec sendEntry;
  std::string packet1(
      "This is a multicast message #1 from node1 to node2, node3, and node4.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet1.c_str()),
          .len = packet1.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex1,
          .srcIPAddr = ipAddr1V6,
          .dstIPAddr = ipAddr2V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      (openr::MockIoProviderUtils::bufferArgs<char[kRecvBufferSize]>){
          .msg = recvMsg,
          .data = recvBuf,
          .len = kMinIpv6PktSize,
          .entry = recvEntry,
          .u = recvUnion,
      },
      srcAddrStorage);

  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex3, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex4, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet2(
      "This is a multicast message #2 from node 2 to node 3, node 4, and node "
      "1.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet2.c_str()),
          .len = packet2.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex2,
          .srcIPAddr = ipAddr2V6,
          .dstIPAddr = ipAddr3V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet2.size(), mockIoProvider->sendmsg(fd2, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex3, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex4, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet3(
      "This is a multicast message #3 from node#3 everyone else.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet3.c_str()),
          .len = packet3.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex3,
          .srcIPAddr = ipAddr3V6,
          .dstIPAddr = ipAddr4V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet3.size(), mockIoProvider->sendmsg(fd3, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet3.size(), mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet3, recvMsg);
  EXPECT_EQ(ifIndex4, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet3.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet3, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet3.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet3, recvMsg);
  EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet4(
      "This is a multicast message #4 from node number 4 to nodes 1,2,3.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet4.c_str()),
          .len = packet4.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex4,
          .srcIPAddr = ipAddr4V6,
          .dstIPAddr = ipAddr1V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet4.size(), mockIoProvider->sendmsg(fd4, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet4.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet4, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet4.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet4, recvMsg);
  EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet4.size(), mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet4, recvMsg);
  EXPECT_EQ(ifIndex3, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  // Cleanup
  mockIoProvider->stop();
  mockIoProviderThread.join();
}

//
// This test sends packets along the follow topology.
//
// 4-node full star topology: 2 <-> 1 --> 3
//                            ^     |     ^
//                            |     |     |
//                            |     v     |
//                            +---> 4 ----+
// Each node has 3-degree outgoing connectivities.
//
TEST(MockIoProviderTestSetup, PartialMeshOfNodesTest) {
  folly::IPAddressV6 ipAddr1V6("fe80::1");
  folly::IPAddressV6 ipAddr2V6("fe80::2");
  folly::IPAddressV6 ipAddr3V6("fe80::3");
  folly::IPAddressV6 ipAddr4V6("fe80::4");

  std::string ifName1("iface1");
  std::string ifName2("iface2");
  std::string ifName3("iface3");
  std::string ifName4("iface4");

  int ifIndex1 = 1;
  int ifIndex2 = 2;
  int ifIndex3 = 3;
  int ifIndex4 = 4;

  auto mockIoProvider = std::make_shared<MockIoProvider>();

  // Start mock IoProvider thread
  std::thread mockIoProviderThread([&]() {
    LOG(INFO) << "Starting mockIoProvider thread.";
    mockIoProvider->start();
    LOG(INFO) << "mockIoProvider thread got stopped.";
  });
  mockIoProvider->waitUntilRunning();

  mockIoProvider->addIfNameIfIndex(
      {{ifName1, ifIndex1},
       {ifName2, ifIndex2},
       {ifName3, ifIndex3},
       {ifName4, ifIndex4}});

  // Bidirectional connectivity between 1 and 2, while 3 just an island.
  ConnectedIfPairs connectedPairs = {
      {ifName1, {{ifName2, 10}, {ifName3, 10}, {ifName4, 10}}},
      {ifName2, {{ifName4, 10}, {ifName1, 10}}},
      {ifName4, {{ifName2, 10}, {ifName3, 10}}},
  };
  mockIoProvider->setConnectedPairs(connectedPairs);

  int fd1 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex1, folly::IPAddress(kDiscardMulticastAddr));

  int fd2 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex2, folly::IPAddress(kDiscardMulticastAddr));

  int fd3 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex3, folly::IPAddress(kDiscardMulticastAddr));

  int fd4 = createSocketAndJoinGroup(
      mockIoProvider, ifIndex4, folly::IPAddress(kDiscardMulticastAddr));

  struct msghdr sendMsg;
  AlignedCtrlBuf<struct in6_pktinfo> sendUnion;
  sockaddr_storage dstAddrStorage;
  struct iovec sendEntry;
  std::string packet1(
      "This is a multicast message #1 from node1 to node2, node3, and node4.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet1.c_str()),
          .len = packet1.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex1,
          .srcIPAddr = ipAddr1V6,
          .dstIPAddr = ipAddr2V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      (openr::MockIoProviderUtils::bufferArgs<char[kRecvBufferSize]>){
          .msg = recvMsg,
          .data = recvBuf,
          .len = kMinIpv6PktSize,
          .entry = recvEntry,
          .u = recvUnion,
      },
      srcAddrStorage);

  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex3, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet1.size(), mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet1, recvMsg);
  EXPECT_EQ(ifIndex4, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_THROW(
      mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT),
      std::invalid_argument);
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet2(
      "This is a multicast message #2 from node 2 to node 4 and node 1.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet2.c_str()),
          .len = packet2.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex2,
          .srcIPAddr = ipAddr2V6,
          .dstIPAddr = ipAddr4V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet2.size(), mockIoProvider->sendmsg(fd2, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex4, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet2.size(), mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet2, recvMsg);
  EXPECT_EQ(ifIndex1, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet3(
      "This is an anycast message no.3 from node no.3 to nowhere else.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet3.c_str()),
          .len = packet3.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex3,
          .srcIPAddr = ipAddr3V6,
          .dstIPAddr = ipAddr4V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(-1, mockIoProvider->sendmsg(fd3, &sendMsg, MSG_DONTWAIT));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet4(
      "This is a multicast message #4 from node number 4 to nodes 2 and 3.");
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = const_cast<char*>(packet4.c_str()),
          .len = packet4.size(),
          .entry = sendEntry,
          .u = sendUnion,
      },
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex4,
          .srcIPAddr = ipAddr4V6,
          .dstIPAddr = ipAddr2V6,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  EXPECT_EQ(
      packet4.size(), mockIoProvider->sendmsg(fd4, &sendMsg, MSG_DONTWAIT));

  EXPECT_EQ(
      packet4.size(), mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet4, recvMsg);
  EXPECT_EQ(ifIndex2, getMsgIfIndex(&recvMsg));
  EXPECT_EQ(
      packet4.size(), mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  checkPacketContent(packet4, recvMsg);
  EXPECT_EQ(ifIndex3, getMsgIfIndex(&recvMsg));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  // Cleanup
  mockIoProvider->stop();
  mockIoProviderThread.join();
}

int
main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  return RUN_ALL_TESTS();
}
