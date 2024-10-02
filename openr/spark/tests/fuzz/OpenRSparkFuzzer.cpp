// Copyright 2004-present Facebook. All Rights Reserved.

#include <string>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <openr/config/Config.h>
#include <openr/tests/utils/Utils.h>

#include <openr/spark/IoProvider.h>
#include <openr/spark/SparkWrapper.h>
#include <openr/tests/mocks/MockIoProvider.h>
#include <openr/tests/mocks/MockIoProviderUtils.h>

using namespace openr;

namespace {
const int kMockedUdpPort{6666};

const std::string iface1{"iface1"};
const std::string iface2{"iface2"};

const int ifIndex1{1};
const int ifIndex2{2};

const folly::CIDRNetwork ip1V4 =
    folly::IPAddress::createNetwork("192.168.0.1", 24, false /* apply mask */);
const folly::CIDRNetwork ip2V4 =
    folly::IPAddress::createNetwork("192.168.0.2", 24, false /* apply mask */);
const std::string kDiscardMulticastAddr("ff01::1");

const folly::CIDRNetwork ip1V6 = folly::IPAddress::createNetwork("fe80::1/128");
const folly::CIDRNetwork ip2V6 = folly::IPAddress::createNetwork("fe80::2/128");

// Domain name (same for all Tests except in DomainTest)
const std::string kDomainName("openr_fuzz_test");

/* Call to have MockIo in separate thread, but for fuzzing we want to
 * to process packets inline ideally.
 */
const bool mockIoThread = false;

/* ala SparkTest.cpp */
class Fixture {
 protected:
  void
  MockIoSetUp() {
    mockIoProvider = std::make_shared<MockIoProvider>();

    if (mockIoThread) {
      // Start mock IoProvider thread
      mockIoProviderThread = std::make_unique<std::thread>([this]() {
        LOG(INFO) << "Starting mockIoProvider thread.";
        mockIoProvider->start();
        LOG(INFO) << "mockIoProvider thread got stopped.";
      });
      mockIoProvider->waitUntilRunning();
    } else {
      mockIoProvider->setIsRunning(true);
    }
  }
  std::shared_ptr<SparkWrapper>
  createSpark(
      std::string const& myNodeName,
      std::shared_ptr<const Config> config,
      std::pair<uint32_t, uint32_t> version = std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion)) {
    return std::make_unique<SparkWrapper>(
        myNodeName, version, mockIoProvider, config, true);
  }

  std::unique_ptr<std::thread> mockIoProviderThread{nullptr};

 public:
  Fixture() {
    // Constants::kMaxAllowedPps = 100000000;
    MockIoSetUp();
    mockIoProvider->addIfNameIfIndex({{iface1, ifIndex1}, {iface2, ifIndex2}});
    // connect interfaces directly
    ConnectedIfPairs connectedPairs = {
        {iface1, {{iface2, 10}}},
        {iface2, {{iface1, 10}}},
    };
    mockIoProvider->setConnectedPairs(connectedPairs);
    // Start Spark instance
    auto config = getBasicOpenrConfig("node-1");
    node1 = createSpark("node-1", std::make_shared<Config>(config));
    node1->updateInterfaceDb({InterfaceInfo(
        iface1 /* ifName */,
        true /* isUp */,
        ifIndex1 /* ifIndex */,
        {ip1V4, ip1V6} /* networks */)});
    testfd = MockIoProviderUtils::createSocketAndJoinGroup(
        mockIoProvider, ifIndex2, folly::IPAddress(kDiscardMulticastAddr));
  }
  ~Fixture() {
    mockIoProvider->stop();
  }

  std::shared_ptr<SparkWrapper> node1;
  std::shared_ptr<MockIoProvider> mockIoProvider{nullptr};
  int testfd = -1;
};
static std::shared_ptr<Fixture> fixture{nullptr};

void
fuzzSparkMessage(uint8_t* data, size_t len) {
  struct msghdr sendMsg;
  MockIoProviderUtils::AlignedCtrlBuf<struct in6_pktinfo> sendUnion;
  sockaddr_storage dstAddrStorage;
  struct iovec sendEntry;
  prepareSendMessage(
      (openr::MockIoProviderUtils::bufferArgs<struct in6_pktinfo>){
          .msg = sendMsg,
          .data = data,
          .len = len,
          .entry = sendEntry,
          .u = sendUnion},
      (struct openr::MockIoProviderUtils::networkArgs){
          .srcIfIndex = ifIndex2,
          .srcIPAddr = ip2V6.first,
          .dstIPAddr = ip1V6.first,
          .dstPort = kMockedUdpPort,
          .dstAddrStorage = dstAddrStorage,
      });
  fixture->mockIoProvider->sendmsg(fixture->testfd, &sendMsg, MSG_DONTWAIT);
  fixture->mockIoProvider->processMailboxes();
  fixture->node1->processPacket();
}
}; // namespace

extern "C" int
LLVMFuzzerTestOneInput(uint8_t* Data, size_t Size) {
  fuzzSparkMessage(Data, Size);

  return 0;
}

extern "C" int
LionheadFuzzerCustomInitialize(int*, char***) {
  fixture = std::make_shared<Fixture>();
  fixture->testfd =
      fixture->mockIoProvider->socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);

  return 0;
}
