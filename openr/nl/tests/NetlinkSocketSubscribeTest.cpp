/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdlib>
#include <string>
#include <thread>

#include <folly/Format.h>
#include <folly/ScopeGuard.h>
#include <folly/Subprocess.h>
#include <folly/io/async/EventBase.h>
#include <folly/synchronization/Baton.h>
#include <folly/system/Shell.h>
#include <folly/test/TestUtils.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <openr/nl/NetlinkSocket.h>

using namespace openr;
using namespace openr::fbnl;
using namespace fbzmq;
using namespace folly::literals::shell_literals;

namespace {
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");
const folly::IPAddress kNextHopIp1("fe80::1");
const folly::IPAddress kNextHopIp2("fe80::2");
const folly::IPAddress kNextHopIp3("fe80::3");
const folly::IPAddress kNextHopIp4("10.0.1.1");
const folly::IPAddress kNextHopIp5("10.0.1.2");
const folly::MacAddress kLinkAddr1("01:02:03:04:05:06");
const folly::MacAddress kLinkAddr2("01:02:03:04:05:07");
const folly::MacAddress kDefaultLinkAddr;
const folly::CIDRNetwork kIpAddr1{folly::IPAddress("face:b00c::1"), 128};
const folly::CIDRNetwork kIpAddr2{folly::IPAddress("face:b00c::2"), 128};
const folly::CIDRNetwork kPrefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
const folly::CIDRNetwork kPrefix2{folly::IPAddress("fc00:cafe:3::4"), 128};
const folly::CIDRNetwork kPrefix3{folly::IPAddress("fc00:cafe:3::5"), 128};
const folly::CIDRNetwork kIpAddr3{folly::IPAddress("10.0.0.1"), 32};
const folly::CIDRNetwork kIpAddr4{folly::IPAddress("10.0.0.2"), 32};
const std::chrono::milliseconds kEventLoopTimeout(5000);
const std::chrono::milliseconds kStressTestEventLoopTimeout(30000);

void
addTestNeighborEntry(
    const std::string& ifName,
    const folly::IPAddress& nextHopIp,
    const folly::MacAddress& linkAddr) {
  auto cmd = "ip -6 neigh add {} lladdr {} nud reachable dev {}"_shellify(
      nextHopIp.str().c_str(), linkAddr.toString().c_str(), ifName.c_str());
  folly::Subprocess proc(std::move(cmd));
  EXPECT_EQ(0, proc.wait().exitStatus());
}

void
deleteTestNeighborEntry(
    const std::string& ifName,
    const folly::IPAddress& nextHopIp,
    const folly::MacAddress& linkAddr) {
  // Now delete the neighbor entry from the system
  auto cmd = "ip -6 neigh del {} lladdr {} nud reachable dev {}"_shellify(
      nextHopIp.str().c_str(), linkAddr.toString().c_str(), ifName.c_str());
  folly::Subprocess proc(std::move(cmd));
  EXPECT_EQ(0, proc.wait().exitStatus());
}

} // namespace

class NetlinkSocketSubscribeFixture : public testing::Test {
 public:
  NetlinkSocketSubscribeFixture() = default;
  ~NetlinkSocketSubscribeFixture() override = default;

  void
  SetUp() override {
    if (getuid()) {
      SKIP() << "Must run this test as root";
      return;
    }

    // cleanup old interfaces if any
    auto cmd = "ip link del {}"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    // Ignore result
    proc.wait();

    // add veth interface pair
    cmd = "ip link add {} type veth peer name {}"_shellify(
        kVethNameX.c_str(), kVethNameY.c_str());
    folly::Subprocess proc1(std::move(cmd));
    EXPECT_EQ(0, proc1.wait().exitStatus());

    nlProtocolSocket = std::make_unique<openr::fbnl::NetlinkProtocolSocket>(
        &nlEvb, netlinkEventsQ);
    nlProtocolSocketThread = std::thread([&]() { nlEvb.loopForever(); });
    nlEvb.waitUntilRunning();
  }

  void
  TearDown() override {
    if (getuid()) {
      // Nothing to cleanup if not-root
      return;
    }

    // cleanup veth interfaces
    auto cmd = "ip link del {}"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());

    if (nlEvb.isRunning()) {
      nlEvb.terminateLoopSoon();
      nlProtocolSocketThread.join();
    }
  }

 protected:
  folly::EventBase nlEvb;
  std::unique_ptr<openr::fbnl::NetlinkProtocolSocket> nlProtocolSocket;
  messaging::ReplicateQueue<openr::fbnl::NetlinkEvent> netlinkEventsQ;
  std::thread nlProtocolSocketThread;
};

// EventFunc is to let a UT do whatever it wants for the event
// (like keep track of certain number of events once event loop starts)
// ifNamePrefix is to filter events for certain links only
class MyNetlinkHandler final : public NetlinkSocket::EventsHandler {
 public:
  MyNetlinkHandler(
      std::function<void()> eventFunc, const std::string& ifNamePrefix)
      : eventFunc(eventFunc), ifNamePrefix(ifNamePrefix) {}
  MyNetlinkHandler() = default;
  ~MyNetlinkHandler() override = default;

  void
  setNetlinkSocket(NetlinkSocket* ns) {
    netlinkSocket = ns;
  }
  void
  replaceEventFunc(std::function<void()> eventFunc) {
    this->eventFunc = std::move(eventFunc);
  }

  void
  linkEventFunc(const std::string&, const openr::fbnl::Link& linkEntry) noexcept
      override {
    std::string ifName = linkEntry.getLinkName();
    VLOG(3) << "**Link : " << ifName << (linkEntry.isUp() ? " UP" : " DOWN");
    if (ifName.find(ifNamePrefix) == std::string::npos) {
      return;
    }
    // Ignore link event for down links
    if (links.count(ifName) == 0 && not linkEntry.isUp()) {
      return;
    }
    links[ifName].isUp = linkEntry.isUp();
    if (!linkEntry.isUp()) {
      linkDelEventCount++;
    } else {
      linkAddEventCount++;
    }
    if (eventFunc) {
      eventFunc();
    }
  }

  void
  addrEventFunc(
      const std::string&,
      const openr::fbnl::IfAddress& addrEntry) noexcept override {
    bool isValid = addrEntry.isValid();
    std::string ifName = netlinkSocket->getIfName(addrEntry.getIfIndex()).get();
    VLOG(3) << "**Address : "
            << folly::IPAddress::networkToString(addrEntry.getPrefix().value())
            << "@" << ifName << (isValid ? " ADDED" : " DELETED");
    if (ifName.find(ifNamePrefix) == std::string::npos) {
      return;
    }
    if (isValid) {
      // Ignore the event if address already exists
      if (links[ifName].networks.insert(addrEntry.getPrefix().value()).second) {
        addrAddEventCount++;
      } else {
        LOG(WARNING) << "Duplicate address event: " << addrEntry.str()
                     << ", ifName=" << ifName;
      }
    } else {
      // Ignore the event if address doesn't exists
      if (links[ifName].networks.erase(addrEntry.getPrefix().value())) {
        addrDelEventCount++;
      } else {
        LOG(WARNING) << "Duplicate address event: " << addrEntry.str()
                     << ", ifName=" << ifName;
      }
    }

    if (eventFunc) {
      eventFunc();
    }
  }

  void
  neighborEventFunc(
      const std::string&,
      const openr::fbnl::Neighbor& neighborEntry) noexcept override {
    std::string ifName =
        netlinkSocket->getIfName(neighborEntry.getIfIndex()).get();
    VLOG(3)
        << "** Neighbor entry: " << ifName << " : "
        << neighborEntry.getDestination().str() << " -> "
        << (neighborEntry.getLinkAddress().has_value()
                ? neighborEntry.getLinkAddress().value().toString()
                : "n/a")
        << (neighborEntry.isReachable() ? " : Reachable" : " : Unreachable");

    // Ignore entries on unknown interfaces
    if (ifName.find(ifNamePrefix) == std::string::npos) {
      return;
    }

    auto neighborKey = std::make_pair(ifName, neighborEntry.getDestination());
    if (neighborEntry.isReachable()) {
      auto it = neighbors.insert({neighborKey, neighborEntry});
      it.first->second = neighborEntry; // Override existing if any
      neighborAddEventCount++;
    } else {
      neighbors.erase(neighborKey);
      neighborDelEventCount++;
    }

    if (eventFunc) {
      eventFunc();
    }
  }

  // Making vars public so UT can easily check these
  int linkAddEventCount{0};
  int linkDelEventCount{0};
  int neighborAddEventCount{0};
  int neighborDelEventCount{0};
  int addrAddEventCount{0};
  int addrDelEventCount{0};
  NlLinks links;
  NlNeighbors neighbors;
  std::function<void()> eventFunc{nullptr};
  std::string ifNamePrefix{"vethTest"};
  NetlinkSocket* netlinkSocket{nullptr};

 private:
  MyNetlinkHandler(const MyNetlinkHandler&) = delete;
  MyNetlinkHandler& operator=(const MyNetlinkHandler&) = delete;
};

// Add 2 neighbor entries via test link
// Verify events are received
// Now delete the entries one by one
// Verify neighbor entry is reachable
// Also get and verify links and neighbor states in main thread
TEST_F(NetlinkSocketSubscribeFixture, NeighborMultipleEventTest) {
  ZmqEventLoop zmqLoop;

  auto neighborKey1 = std::make_pair(kVethNameX, kNextHopIp1);
  auto neighborKey2 = std::make_pair(kVethNameY, kNextHopIp2);

  // We expect link and neighbor events to be delivered
  // 4 neighbor event (create and delete)
  // This func helps stop zmq loop when expected events are processed
  std::shared_ptr<MyNetlinkHandler> myHandler =
      std::make_shared<MyNetlinkHandler>(
          [&]() noexcept {}, "vethTest" /* Filter on test links only */);

  NetlinkSocket netlinkSocket(
      &zmqLoop, myHandler.get(), std::move(nlProtocolSocket));
  netlinkSocket.subscribeAllEvents();
  myHandler->setNetlinkSocket(&netlinkSocket);

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() {
    zmqLoop.run();
    zmqLoop.waitUntilStopped();
  });

  zmqLoop.waitUntilRunning();

  // Now add a neighbor entry in the system
  addTestNeighborEntry(kVethNameX, kNextHopIp1, kLinkAddr1);

  // Verify the get* methods
  auto links = netlinkSocket.getAllLinks().get();
  auto neighbors = netlinkSocket.getAllReachableNeighbors().get();

  // Verify we have the link state
  EXPECT_EQ(1, links.count(kVethNameX));
  EXPECT_EQ(1, links.count(kVethNameY));

  // Verify links are not up
  EXPECT_FALSE(links.at(kVethNameX).isUp);
  EXPECT_FALSE(links.at(kVethNameY).isUp);

  // Verify neighbor entry we added
  EXPECT_EQ(1, neighbors.count(neighborKey1));
  EXPECT_EQ(1, links.count(kVethNameY));

  EXPECT_EQ(kLinkAddr1, neighbors.at(neighborKey1).getLinkAddress().value());

  // Now verify our events
  // 1+ neighbor events
  EXPECT_EQ(1, myHandler->neighbors.size());
  EXPECT_LE(1, myHandler->neighborAddEventCount);
  EXPECT_EQ(1, myHandler->neighbors.count(neighborKey1));
  EXPECT_EQ(
      kLinkAddr1,
      myHandler->neighbors.at(neighborKey1).getLinkAddress().value());

  // No link events
  EXPECT_EQ(0, myHandler->links.size());
  EXPECT_EQ(0, myHandler->linkAddEventCount);
  EXPECT_EQ(0, myHandler->linkDelEventCount);

  // Now add another neighbor entry in the system
  addTestNeighborEntry(kVethNameY, kNextHopIp2, kLinkAddr2);

  // Again Verify the get* methods
  links = netlinkSocket.getAllLinks().get();
  neighbors = netlinkSocket.getAllReachableNeighbors().get();

  // Verify we have the link state
  EXPECT_EQ(1, links.count(kVethNameX));
  EXPECT_EQ(1, links.count(kVethNameY));

  // Verify neighbor entry we added
  EXPECT_EQ(1, neighbors.count(neighborKey2));
  EXPECT_EQ(kLinkAddr2, neighbors.at(neighborKey2).getLinkAddress().value());

  // Now verify our events
  // 2+ neighbor events
  EXPECT_EQ(2, myHandler->neighbors.size());
  EXPECT_LE(2, myHandler->neighborAddEventCount);
  EXPECT_EQ(1, myHandler->neighbors.count(neighborKey2));
  EXPECT_EQ(
      kLinkAddr2,
      myHandler->neighbors.at(neighborKey2).getLinkAddress().value());

  // Now delete both the neighbor entries from the system
  folly::Baton neighborBaton;
  int expectedNeighbors{0};
  myHandler->replaceEventFunc([&]() {
    if (expectedNeighbors == myHandler->neighbors.size()) {
      neighborBaton.post();
    }
  });

  expectedNeighbors = 1;
  deleteTestNeighborEntry(kVethNameX, kNextHopIp1, kLinkAddr1);
  neighborBaton.wait();
  neighborBaton.reset();

  expectedNeighbors = 0;
  deleteTestNeighborEntry(kVethNameY, kNextHopIp2, kLinkAddr2);
  neighborBaton.wait();
  neighborBaton.reset();

  // Now verify our events
  // 4 neighbor events
  // But neighbors should be down now..
  EXPECT_EQ(0, myHandler->neighbors.size());
  EXPECT_LE(2, myHandler->neighborDelEventCount);

  // No link events
  EXPECT_EQ(0, myHandler->links.size());
  EXPECT_EQ(0, myHandler->linkAddEventCount);
  EXPECT_EQ(0, myHandler->linkDelEventCount);

  // Read again and verify get* methods
  links = netlinkSocket.getAllLinks().get();
  neighbors = netlinkSocket.getAllReachableNeighbors().get();

  // no new events
  EXPECT_EQ(0, myHandler->neighbors.size());
  EXPECT_LE(2, myHandler->neighborAddEventCount);
  EXPECT_LE(2, myHandler->neighborDelEventCount);

  // No link events
  EXPECT_EQ(0, myHandler->links.size());
  EXPECT_EQ(0, myHandler->linkAddEventCount);
  EXPECT_EQ(0, myHandler->linkDelEventCount);

  // Verify we have the link state
  EXPECT_EQ(1, links.count(kVethNameX));
  EXPECT_EQ(1, links.count(kVethNameY));

  // Verify links are not up
  EXPECT_FALSE(links.at(kVethNameX).isUp);
  EXPECT_FALSE(links.at(kVethNameY).isUp);

  // Verify neighbor entry we deleted
  EXPECT_EQ(0, neighbors.count(neighborKey1));
  EXPECT_EQ(0, neighbors.count(neighborKey2));

  zmqLoop.stop();
  eventThread.join();
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
