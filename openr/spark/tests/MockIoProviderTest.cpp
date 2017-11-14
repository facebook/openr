/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <forward_list>
#include <thread>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Memory.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/SocketAddress.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/Constants.h>

#include "MockIoProvider.h"

namespace {

const int kMinIpv6PktSize{1280};
const int kMockedUdpPort{6666};
const int kPollTimeout{10};
const int kRecvBufferSize{1024};
const std::string kDiscardMulticastAddr("ff01::1");

template <typename T>
union AlignedCtrlBuf {
  char cbuf[CMSG_SPACE(sizeof(T))];
  struct cmsghdr align;
};

void
prepareSendMessage(
    struct msghdr& msg,
    const int srcIfIndex,
    const folly::IPAddress& srcIPAddr,
    const folly::IPAddress& dstIPAddr,
    const std::string& packet,
    AlignedCtrlBuf<struct in6_pktinfo>& u,
    sockaddr_storage& dstAddrStorage,
    struct iovec& entry) {
  msg.msg_control = u.cbuf;
  msg.msg_controllen = sizeof(u.cbuf);

  folly::SocketAddress dstSockAddr(dstIPAddr, kMockedUdpPort);
  dstSockAddr.getAddress(&dstAddrStorage);
  msg.msg_name = reinterpret_cast<void*>(&dstAddrStorage);
  msg.msg_namelen = dstSockAddr.getActualSize();

  struct cmsghdr* cmsg{nullptr};
  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = IPPROTO_IPV6;
  cmsg->cmsg_type = IPV6_PKTINFO;
  cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));

  auto pktinfo = (struct in6_pktinfo*)CMSG_DATA(cmsg);
  pktinfo->ipi6_ifindex = srcIfIndex;
  std::memcpy(&pktinfo->ipi6_addr, srcIPAddr.bytes(), srcIPAddr.byteCount());

  entry.iov_base = const_cast<char*>(packet.c_str());
  entry.iov_len = packet.size();

  msg.msg_iov = &entry;
  msg.msg_iovlen = 1;
}

void
prepareRecvMessage(
    struct msghdr& msg,
    unsigned char* buf,
    int len,
    struct iovec& entry,
    AlignedCtrlBuf<char[kRecvBufferSize]>& u,
    sockaddr_storage& srcAddrStorage) {
  std::memset(&msg, 0, sizeof(msg));

  entry.iov_base = buf;
  entry.iov_len = len;

  msg.msg_iov = &entry;
  msg.msg_iovlen = 1;

  std::memset(&u.cbuf[0], 0, sizeof(u.cbuf));
  msg.msg_control = u.cbuf;
  msg.msg_controllen = sizeof(u.cbuf);

  std::memset(&srcAddrStorage, 0, sizeof(srcAddrStorage));
  msg.msg_name = &srcAddrStorage;
  msg.msg_namelen = sizeof(sockaddr_storage);
}

int
getMsgIfIndex(struct msghdr* msg) {
  for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(msg); cmsg;
       cmsg = CMSG_NXTHDR(msg, cmsg)) {
    if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
      auto pktinfo = (struct in6_pktinfo*)CMSG_DATA(cmsg);
      return pktinfo->ipi6_ifindex;
    }
  }
  return -1;
}

void
checkPacketContent(const std::string& packet, const struct msghdr& msg) {
  EXPECT_EQ(
      packet,
      std::string(
          reinterpret_cast<const char*>(msg.msg_iov->iov_base), packet.size()));
}

int
createSocketAndJoinGroup(
    std::shared_ptr<openr::MockIoProvider> mockIoProvider,
    int ifIndex,
    folly::IPAddress /* mcastGroup */) {
  int fd = mockIoProvider->socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  CHECK(fd);

  struct ipv6_mreq mreq;
  mreq.ipv6mr_interface = ifIndex;
  if (mockIoProvider->setsockopt(
          fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) != 0) {
    LOG(ERROR) << "setsockopt on ipv6_join_group failed with error "
               << folly::errnoStr(errno);
  }
  return fd;
}

void
waitForDataToRead(const int fd) {
  std::vector<fbzmq::PollItem> items = {{nullptr, fd, ZMQ_POLLIN, 0}};

  // poll until we have received any message.
  fbzmq::poll(items, folly::none);
}

} // anonymous namespace.

using namespace openr;

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
      sendMsg,
      ifIndex1,
      ipAddr1V6,
      ipAddr2V6,
      packet1,
      sendUnion,
      dstAddrStorage,
      sendEntry);
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      recvMsg, recvBuf, kMinIpv6PktSize, recvEntry, recvUnion, srcAddrStorage);

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
      sendMsg,
      ifIndex2,
      ipAddr2V6,
      ipAddr1V6,
      packet2,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
        sendMsg,
        ifIndex2,
        ipAddr2V6,
        ipAddr1V6,
        packet2,
        sendUnion,
        dstAddrStorage,
        sendEntry);
    EXPECT_EQ(
        packet2.size(), mockIoProvider->sendmsg(fd2, &sendMsg, MSG_DONTWAIT));

    struct msghdr recvMsg;
    unsigned char recvBuf[kMinIpv6PktSize];
    struct iovec recvEntry;
    AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
    sockaddr_storage srcAddrStorage;

    // To receive packet1.
    prepareRecvMessage(
        recvMsg,
        recvBuf,
        kMinIpv6PktSize,
        recvEntry,
        recvUnion,
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
    prepareSendMessage(
        sendMsg,
        ifIndex2,
        ipAddr2V6,
        ipAddr1V6,
        packet3,
        sendUnion,
        dstAddrStorage,
        sendEntry);
    EXPECT_EQ(
        packet3.size(), mockIoProvider->sendmsg(fd2, &sendMsg, MSG_DONTWAIT));

    // Sanity check again.
    EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));

    LOG(INFO) << "Stopping child thread...";
  });

  struct msghdr sendMsg;
  AlignedCtrlBuf<struct in6_pktinfo> sendUnion;
  sockaddr_storage dstAddrStorage;
  struct iovec sendEntry;

  // Send packet1.
  prepareSendMessage(
      sendMsg,
      ifIndex1,
      ipAddr1V6,
      ipAddr2V6,
      packet1,
      sendUnion,
      dstAddrStorage,
      sendEntry);
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;

  // To receive packet2.
  prepareRecvMessage(
      recvMsg, recvBuf, kMinIpv6PktSize, recvEntry, recvUnion, srcAddrStorage);

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

  mockIoProvider->addIfNameIfIndex({{ifName1, ifIndex1},
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
      sendMsg,
      ifIndex1,
      ipAddr1V6,
      ipAddr2V6,
      packet1,
      sendUnion,
      dstAddrStorage,
      sendEntry);
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      recvMsg, recvBuf, kMinIpv6PktSize, recvEntry, recvUnion, srcAddrStorage);

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
      sendMsg,
      ifIndex2,
      ipAddr2V6,
      ipAddr3V6,
      packet2,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
      sendMsg,
      ifIndex3,
      ipAddr3V6,
      ipAddr4V6,
      packet3,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
      sendMsg,
      ifIndex4,
      ipAddr4V6,
      ipAddr1V6,
      packet4,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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

  mockIoProvider->addIfNameIfIndex({{ifName1, ifIndex1},
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
      sendMsg,
      ifIndex1,
      ipAddr1V6,
      ipAddr2V6,
      packet1,
      sendUnion,
      dstAddrStorage,
      sendEntry);
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      recvMsg, recvBuf, kMinIpv6PktSize, recvEntry, recvUnion, srcAddrStorage);

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
      sendMsg,
      ifIndex2,
      ipAddr2V6,
      ipAddr3V6,
      packet2,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
      sendMsg,
      ifIndex3,
      ipAddr3V6,
      ipAddr4V6,
      packet3,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
      sendMsg,
      ifIndex4,
      ipAddr4V6,
      ipAddr1V6,
      packet4,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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

  mockIoProvider->addIfNameIfIndex({{ifName1, ifIndex1},
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
      sendMsg,
      ifIndex1,
      ipAddr1V6,
      ipAddr2V6,
      packet1,
      sendUnion,
      dstAddrStorage,
      sendEntry);
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      recvMsg, recvBuf, kMinIpv6PktSize, recvEntry, recvUnion, srcAddrStorage);

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
      sendMsg,
      ifIndex2,
      ipAddr2V6,
      ipAddr1V6,
      packet2,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
      sendMsg,
      ifIndex3,
      ipAddr3V6,
      ipAddr1V6,
      packet3,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
      sendMsg,
      ifIndex4,
      ipAddr4V6,
      ipAddr1V6,
      packet4,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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

  mockIoProvider->addIfNameIfIndex({{ifName1, ifIndex1},
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
      sendMsg,
      ifIndex1,
      ipAddr1V6,
      ipAddr2V6,
      packet1,
      sendUnion,
      dstAddrStorage,
      sendEntry);
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      recvMsg, recvBuf, kMinIpv6PktSize, recvEntry, recvUnion, srcAddrStorage);

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
      sendMsg,
      ifIndex2,
      ipAddr2V6,
      ipAddr1V6,
      packet2,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
      sendMsg,
      ifIndex3,
      ipAddr3V6,
      ipAddr1V6,
      packet3,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
      sendMsg,
      ifIndex4,
      ipAddr4V6,
      ipAddr1V6,
      packet4,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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

  mockIoProvider->addIfNameIfIndex({{ifName1, ifIndex1},
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
      sendMsg,
      ifIndex1,
      ipAddr1V6,
      ipAddr2V6,
      packet1,
      sendUnion,
      dstAddrStorage,
      sendEntry);
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      recvMsg, recvBuf, kMinIpv6PktSize, recvEntry, recvUnion, srcAddrStorage);

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
      sendMsg,
      ifIndex2,
      ipAddr2V6,
      ipAddr3V6,
      packet2,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
      sendMsg,
      ifIndex3,
      ipAddr3V6,
      ipAddr4V6,
      packet3,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
      sendMsg,
      ifIndex4,
      ipAddr4V6,
      ipAddr1V6,
      packet4,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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

  mockIoProvider->addIfNameIfIndex({{ifName1, ifIndex1},
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
      sendMsg,
      ifIndex1,
      ipAddr1V6,
      ipAddr2V6,
      packet1,
      sendUnion,
      dstAddrStorage,
      sendEntry);
  EXPECT_EQ(
      packet1.size(), mockIoProvider->sendmsg(fd1, &sendMsg, MSG_DONTWAIT));

  struct msghdr recvMsg;
  unsigned char recvBuf[kMinIpv6PktSize];
  struct iovec recvEntry;
  AlignedCtrlBuf<char[kRecvBufferSize]> recvUnion;
  sockaddr_storage srcAddrStorage;
  prepareRecvMessage(
      recvMsg, recvBuf, kMinIpv6PktSize, recvEntry, recvUnion, srcAddrStorage);

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
      sendMsg,
      ifIndex2,
      ipAddr2V6,
      ipAddr4V6,
      packet2,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
      sendMsg,
      ifIndex3,
      ipAddr3V6,
      ipAddr4V6,
      packet3,
      sendUnion,
      dstAddrStorage,
      sendEntry);
  EXPECT_EQ(-1, mockIoProvider->sendmsg(fd3, &sendMsg, MSG_DONTWAIT));

  // Sanity check.
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd1, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd2, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd3, &recvMsg, MSG_DONTWAIT));
  EXPECT_EQ(-1, mockIoProvider->recvmsg(fd4, &recvMsg, MSG_DONTWAIT));

  std::string packet4(
      "This is a multicast message #4 from node number 4 to nodes 2 and 3.");
  prepareSendMessage(
      sendMsg,
      ifIndex4,
      ipAddr4V6,
      ipAddr2V6,
      packet4,
      sendUnion,
      dstAddrStorage,
      sendEntry);
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
