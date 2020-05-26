/**
 * Copyright (c) 2014-present, Facebook, Inc.
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

Route
buildRoute(
    int ifIndex,
    int protocolId,
    const std::vector<folly::IPAddress>& nexthops,
    const folly::CIDRNetwork& dest) {
  fbnl::RouteBuilder rtBuilder;
  auto route = rtBuilder.setDestination(dest).setProtocolId(protocolId);
  fbnl::NextHopBuilder nhBuilder;
  for (const auto& nh : nexthops) {
    nhBuilder.setIfIndex(ifIndex).setGateway(nh);
    rtBuilder.addNextHop(nhBuilder.build());
    nhBuilder.reset();
  }
  return rtBuilder.build();
}

bool
CompareNextHops(std::vector<folly::IPAddress>& nexthops, const Route& route) {
  std::vector<folly::IPAddress> actual;
  for (const auto& nh : route.getNextHops()) {
    if (!nh.getGateway().has_value()) {
      return false;
    }
    actual.push_back(nh.getGateway().value());
  }
  sort(nexthops.begin(), nexthops.end());
  sort(actual.begin(), actual.end());
  return nexthops == actual;
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

    nlProtocolSocket =
        std::make_unique<openr::fbnl::NetlinkProtocolSocket>(&nlEvb);
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
  linkEventFunc(
      const std::string&,
      const openr::fbnl::Link& linkEntry) noexcept override {
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

// By default our test veth links should be down
// and there should be no neighbor entries
// We also do not start our zmq event loop, so there should be
// no events delivered
TEST_F(NetlinkSocketSubscribeFixture, DefaultStateTest) {
  ZmqEventLoop zmqLoop;

  auto myHandler = std::make_shared<MyNetlinkHandler>();
  NetlinkSocket netlinkSocket(
      &zmqLoop, myHandler.get(), std::move(nlProtocolSocket));
  netlinkSocket.subscribeAllEvents();
  myHandler->setNetlinkSocket(&netlinkSocket);

  auto links = netlinkSocket.getAllLinks().get();
  auto neighbors = netlinkSocket.getAllReachableNeighbors().get();

  // Verify we have the link state
  EXPECT_EQ(1, links.count(kVethNameX));
  EXPECT_EQ(1, links.count(kVethNameY));

  // Verify link is down
  EXPECT_FALSE(links.at(kVethNameX).isUp);
  EXPECT_FALSE(links.at(kVethNameY).isUp);

  // Verify that links have no IPs
  EXPECT_TRUE(links.at(kVethNameX).networks.empty());
  EXPECT_TRUE(links.at(kVethNameY).networks.empty());

  // Verify neighbor entries for our test links are only for mcast
  // These were created by system when link came up
  for (const auto& kv : neighbors) {
    EXPECT_NE(kVethNameX, std::get<0>(kv.first));
    EXPECT_NE(kVethNameY, std::get<0>(kv.first));
  }

  // Verify no events received
  EXPECT_TRUE(myHandler->links.empty());
  EXPECT_TRUE(myHandler->neighbors.empty());
  EXPECT_EQ(0, myHandler->linkAddEventCount);
  EXPECT_EQ(0, myHandler->linkDelEventCount);
  EXPECT_EQ(0, myHandler->neighborAddEventCount);
  EXPECT_EQ(0, myHandler->neighborDelEventCount);
  EXPECT_EQ(0, myHandler->addrAddEventCount);
  EXPECT_EQ(0, myHandler->addrDelEventCount);
}

// Flap a link and test for events
// Also get and verify links states in main thread
TEST_F(NetlinkSocketSubscribeFixture, LinkFlapTest) {
  ZmqEventLoop zmqLoop;

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
    VLOG(3) << "Timeout waiting for events... ";
    zmqLoop.stop();
  });

  // We expect both links to be up after which we stop the zmq loop
  // This func helps stop zmq loop when expected events are processed
  std::shared_ptr<MyNetlinkHandler> myHandler =
      std::make_shared<MyNetlinkHandler>(
          [&]() noexcept {
            VLOG(3) << "Received event from netlink";
            if (zmqLoop.isRunning() && myHandler->links.count(kVethNameX) &&
                myHandler->links.count(kVethNameY) &&
                myHandler->links.at(kVethNameX).isUp &&
                myHandler->links.at(kVethNameY).isUp &&
                myHandler->links.at(kVethNameX).networks.size() == 1 &&
                myHandler->links.at(kVethNameY).networks.size() == 1) {
              LOG(INFO) << "Expected events received. Stopping zmq event loop";
              zmqLoop.stop();
            }
          },
          "vethTest" /* Filter on test links only */);

  NetlinkSocket netlinkSocket(
      &zmqLoop, myHandler.get(), std::move(nlProtocolSocket));
  myHandler->setNetlinkSocket(&netlinkSocket);
  netlinkSocket.subscribeAllEvents();

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() {
    zmqLoop.run();
    zmqLoop.waitUntilStopped();
  });

  zmqLoop.waitUntilRunning();

  // Now emulate the links going up
  // We deliberately choose system calls here to completely
  // decouple from netlink socket behavior being tested
  auto cmd = "ip link set dev {} up"_shellify(kVethNameX.c_str());
  folly::Subprocess proc(std::move(cmd));
  EXPECT_EQ(0, proc.wait().exitStatus());

  cmd = "ip link set dev {} up"_shellify(kVethNameY.c_str());
  folly::Subprocess proc1(std::move(cmd));
  EXPECT_EQ(0, proc1.wait().exitStatus());

  zmqLoop.waitUntilStopped();

  // Verify the get* methods
  auto links = netlinkSocket.getAllLinks().get();
  auto neighbors = netlinkSocket.getAllReachableNeighbors().get();

  // Verify we have the link state
  EXPECT_EQ(1, links.count(kVethNameX));
  EXPECT_EQ(1, links.count(kVethNameY));

  // Verify links are up
  EXPECT_TRUE(links.at(kVethNameX).isUp);
  EXPECT_TRUE(links.at(kVethNameY).isUp);

  // Verify links have link-local addresses
  EXPECT_EQ(1, links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, links.at(kVethNameY).networks.size());

  // Now verify our events we received via callback

  // >= 2 link events and both links are up
  // (veths report link down and then up)
  EXPECT_EQ(2, myHandler->links.size());
  EXPECT_LE(2, myHandler->linkAddEventCount);
  EXPECT_EQ(1, myHandler->links.count(kVethNameX));
  EXPECT_EQ(1, myHandler->links.count(kVethNameY));
  EXPECT_TRUE(myHandler->links.at(kVethNameX).isUp);
  EXPECT_TRUE(myHandler->links.at(kVethNameY).isUp);
  EXPECT_EQ(2, myHandler->addrAddEventCount);
  EXPECT_EQ(1, myHandler->links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, myHandler->links.at(kVethNameY).networks.size());

  eventThread.join();
}

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

// Flap link and check that link-local IPs appear and disappear
TEST_F(NetlinkSocketSubscribeFixture, AddrLinkFlapTest) {
  ZmqEventLoop zmqLoop;

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  auto setTimeout = [&] {
    return zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
      VLOG(3) << "Timeout waiting for events... ";
      zmqLoop.stop();
    });
  };
  int timeout = setTimeout();

  bool allEventsReceived = false;

  // We expect both links to be up after which we stop the zmq loop
  // This func helps stop zmq loop when expected events are processed
  std::shared_ptr<MyNetlinkHandler> myHandler =
      std::make_shared<MyNetlinkHandler>(
          [&]() noexcept {
            VLOG(3) << "Received event from netlink";
            if (zmqLoop.isRunning() && myHandler->links.count(kVethNameX) &&
                myHandler->links.count(kVethNameY) &&
                myHandler->links.at(kVethNameX).isUp &&
                myHandler->links.at(kVethNameY).isUp &&
                myHandler->addrAddEventCount == 2) {
              VLOG(3) << "Expected events received. Stopping zmq event loop";
              allEventsReceived = true;
              zmqLoop.stop();
            }
          },
          "vethTest" /* Filter on test links only */);

  NetlinkSocket netlinkSocket(
      &zmqLoop, myHandler.get(), std::move(nlProtocolSocket));
  myHandler->setNetlinkSocket(&netlinkSocket);
  netlinkSocket.subscribeAllEvents();

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() { zmqLoop.run(); });

  zmqLoop.waitUntilRunning();

  // Now emulate the links going up. This will generate link-local addresses
  // We deliberately choose system calls here to completely
  // decouple from netlink socket behavior being tested
  auto cmd = "ip link set dev {} up"_shellify(kVethNameX.c_str());
  folly::Subprocess proc(std::move(cmd));
  EXPECT_EQ(0, proc.wait().exitStatus());

  cmd = "ip link set dev {} up"_shellify(kVethNameY.c_str());
  folly::Subprocess proc1(std::move(cmd));
  EXPECT_EQ(0, proc1.wait().exitStatus());

  eventThread.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  // Verify the get method
  auto links = netlinkSocket.getAllLinks().get();

  EXPECT_EQ(1, links.count(kVethNameX));
  EXPECT_EQ(1, links.count(kVethNameY));
  EXPECT_EQ(1, links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, links.at(kVethNameY).networks.size());
  EXPECT_TRUE(allEventsReceived);
  EXPECT_EQ(1, myHandler->links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, myHandler->links.at(kVethNameY).networks.size());

  // Now bring the links down
  myHandler->addrDelEventCount = 0;
  allEventsReceived = false;
  myHandler->replaceEventFunc([&] {
    VLOG(3) << "Received event from netlink";
    if (zmqLoop.isRunning() && myHandler->links.count(kVethNameX) &&
        myHandler->links.count(kVethNameY) &&
        // We check for !isUp here, the rest is the same
        !myHandler->links.at(kVethNameX).isUp &&
        !myHandler->links.at(kVethNameY).isUp &&
        myHandler->addrDelEventCount == 2) {
      VLOG(3) << "Expected events received. Stopping zmq event loop";
      allEventsReceived = true;
      zmqLoop.stop();
    }
  });

  timeout = setTimeout();
  std::thread eventThread4([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  cmd = "ip link set dev {} down"_shellify(kVethNameX.c_str());
  folly::Subprocess proc2(std::move(cmd));
  EXPECT_EQ(0, proc2.wait().exitStatus());
  cmd = "ip link set dev {} down"_shellify(kVethNameY.c_str());
  folly::Subprocess proc3(std::move(cmd));
  EXPECT_EQ(0, proc3.wait().exitStatus());

  eventThread4.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  // Verify that the addresses were deleted and we got the events
  links = netlinkSocket.getAllLinks().get();
  EXPECT_EQ(0, links.at(kVethNameX).networks.size());
  EXPECT_EQ(0, links.at(kVethNameY).networks.size());
  EXPECT_EQ(0, myHandler->links.at(kVethNameX).networks.size());
  EXPECT_EQ(0, myHandler->links.at(kVethNameY).networks.size());
  EXPECT_TRUE(allEventsReceived);
}

// Add and remove IPs
TEST_F(NetlinkSocketSubscribeFixture, AddrAddRemoveTest) {
  ZmqEventLoop zmqLoop;

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  auto setTimeout = [&] {
    return zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
      VLOG(3) << "Timeout waiting for events... ";
      zmqLoop.stop();
    });
  };
  int timeout = setTimeout();

  bool allEventsReceived = false;

  // We expect both links to be up after which we stop the zmq loop
  // This func helps stop zmq loop when expected events are processed
  std::shared_ptr<MyNetlinkHandler> myHandler =
      std::make_shared<MyNetlinkHandler>(
          [&]() noexcept {
            VLOG(3) << "Received event from netlink";
            if (zmqLoop.isRunning() && myHandler->links.count(kVethNameX) &&
                myHandler->links.count(kVethNameY) &&
                myHandler->links.at(kVethNameX).isUp &&
                myHandler->links.at(kVethNameY).isUp &&
                myHandler->addrAddEventCount == 6) {
              VLOG(3) << "Expected events received. Stopping zmq event loop";
              allEventsReceived = true;
              zmqLoop.stop();
            }
          },
          "vethTest" /* Filter on test links only */);

  NetlinkSocket netlinkSocket(
      &zmqLoop, myHandler.get(), std::move(nlProtocolSocket));
  myHandler->setNetlinkSocket(&netlinkSocket);
  netlinkSocket.subscribeAllEvents();

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  // Now emulate the links going up. This will generate link-local addresses.
  // Also add manual IPs
  // We deliberately choose system calls here to completely
  // decouple from netlink socket behavior being tested
  auto cmd = "ip link set dev {} up"_shellify(kVethNameX.c_str());
  folly::Subprocess proc(std::move(cmd));
  EXPECT_EQ(0, proc.wait().exitStatus());

  cmd = "ip link set dev {} up"_shellify(kVethNameY.c_str());
  folly::Subprocess proc1(std::move(cmd));
  EXPECT_EQ(0, proc1.wait().exitStatus());

  cmd = "ip addr add {} dev {}"_shellify(
      folly::IPAddress::networkToString(kIpAddr1).c_str(), kVethNameX.c_str());
  folly::Subprocess proc2(std::move(cmd));
  EXPECT_EQ(0, proc2.wait().exitStatus());

  cmd = "ip addr add {} dev {}"_shellify(
      folly::IPAddress::networkToString(kIpAddr2).c_str(), kVethNameY.c_str());
  folly::Subprocess proc3(std::move(cmd));
  EXPECT_EQ(0, proc3.wait().exitStatus());

  cmd = "ip addr add {} dev {}"_shellify(
      folly::IPAddress::networkToString(kIpAddr3).c_str(), kVethNameX.c_str());
  folly::Subprocess proc4(std::move(cmd));
  EXPECT_EQ(0, proc4.wait().exitStatus());

  cmd = "ip addr add {} dev {}"_shellify(
      folly::IPAddress::networkToString(kIpAddr4).c_str(), kVethNameY.c_str());
  folly::Subprocess proc5(std::move(cmd));
  EXPECT_EQ(0, proc5.wait().exitStatus());

  eventThread.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  // Verify the get method
  auto links = netlinkSocket.getAllLinks().get();

  EXPECT_EQ(1, links.count(kVethNameX));
  EXPECT_EQ(1, links.count(kVethNameY));
  EXPECT_EQ(3, links.at(kVethNameX).networks.size());
  EXPECT_EQ(3, links.at(kVethNameY).networks.size());
  EXPECT_TRUE(allEventsReceived);
  EXPECT_EQ(3, myHandler->links.at(kVethNameX).networks.size());
  EXPECT_EQ(3, myHandler->links.at(kVethNameY).networks.size());

  // Now remove the addresses
  myHandler->addrAddEventCount = 0;
  allEventsReceived = false;
  myHandler->replaceEventFunc([&] {
    VLOG(3) << "Received event from netlink";
    if (zmqLoop.isRunning() && myHandler->links.count(kVethNameX) &&
        myHandler->links.count(kVethNameY) &&
        myHandler->links.at(kVethNameX).isUp &&
        myHandler->links.at(kVethNameY).isUp &&
        // This is the ony change - wait for 4 addr delete events
        myHandler->addrDelEventCount == 4) {
      VLOG(3) << "Expected events received. Stopping zmq event loop";
      allEventsReceived = true;
      zmqLoop.stop();
    }
  });

  timeout = setTimeout();
  std::thread eventThread2([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  cmd = "ip addr del {} dev {}"_shellify(
      folly::IPAddress::networkToString(kIpAddr1).c_str(), kVethNameX.c_str());
  folly::Subprocess proc6(std::move(cmd));
  EXPECT_EQ(0, proc6.wait().exitStatus());

  cmd = "ip addr del {} dev {}"_shellify(
      folly::IPAddress::networkToString(kIpAddr2).c_str(), kVethNameY.c_str());
  folly::Subprocess proc7(std::move(cmd));
  EXPECT_EQ(0, proc7.wait().exitStatus());

  cmd = "ip addr del {} dev {}"_shellify(
      folly::IPAddress::networkToString(kIpAddr3).c_str(), kVethNameX.c_str());
  folly::Subprocess proc8(std::move(cmd));
  EXPECT_EQ(0, proc8.wait().exitStatus());

  cmd = "ip addr del {} dev {}"_shellify(
      folly::IPAddress::networkToString(kIpAddr4).c_str(), kVethNameY.c_str());
  folly::Subprocess proc9(std::move(cmd));
  EXPECT_EQ(0, proc9.wait().exitStatus());

  eventThread2.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  // Verify that the addresses disappeared (ipv6 link local addresses remains)
  links = netlinkSocket.getAllLinks().get();
  EXPECT_EQ(1, links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, links.at(kVethNameY).networks.size());
  EXPECT_EQ(1, myHandler->links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, myHandler->links.at(kVethNameY).networks.size());
  EXPECT_TRUE(allEventsReceived);
}

// Add and remove IPs using NetlinkSocket APIs and check if events are generated
TEST_F(NetlinkSocketSubscribeFixture, AddrAddRemoveTestNetlink) {
  ZmqEventLoop zmqLoop;

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  auto setTimeout = [&] {
    return zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
      VLOG(3) << "Timeout waiting for events... ";
      zmqLoop.stop();
    });
  };
  int timeout = setTimeout();

  bool allEventsReceived = false;

  // We expect both links to be up after which we stop the zmq loop
  // This func helps stop zmq loop when expected events are processed.
  // We should receive 4 address events -
  // 2 for the link-local addresses of vethTestX and vethTestY and
  // 2 for the IPv4 and IPv6 address we add on vethTestX using NetlinkSocket
  std::shared_ptr<MyNetlinkHandler> myHandler =
      std::make_shared<MyNetlinkHandler>(
          [&]() noexcept {
            VLOG(3) << "Received event from netlink";
            if (zmqLoop.isRunning() && myHandler->links.count(kVethNameX) &&
                myHandler->links.count(kVethNameY) &&
                myHandler->links.at(kVethNameX).isUp &&
                myHandler->links.at(kVethNameY).isUp &&
                myHandler->addrAddEventCount == 4) {
              VLOG(3) << "Expected events received. Stopping zmq event loop";
              allEventsReceived = true;
              zmqLoop.stop();
            }
          },
          "vethTest" /* Filter on test links only */);

  NetlinkSocket netlinkSocket(
      &zmqLoop, myHandler.get(), std::move(nlProtocolSocket));
  myHandler->setNetlinkSocket(&netlinkSocket);
  netlinkSocket.subscribeAllEvents();

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  // Now emulate the links going up. This will generate link-local addresses.
  // We deliberately choose system calls here to completely
  // decouple from netlink socket behavior being tested
  auto cmd = "ip link set dev {} up"_shellify(kVethNameX.c_str());
  folly::Subprocess proc(std::move(cmd));
  EXPECT_EQ(0, proc.wait().exitStatus());

  cmd = "ip link set dev {} up"_shellify(kVethNameY.c_str());
  folly::Subprocess proc1(std::move(cmd));
  EXPECT_EQ(0, proc1.wait().exitStatus());

  {
    // Now add interface addresses using NetlinkSocket and check if events
    // are created
    int ifIndexX = netlinkSocket.getIfIndex(kVethNameX).get();
    fbnl::IfAddressBuilder ifBuilder;
    auto ifAddr = ifBuilder.setPrefix(kIpAddr1)
                      .setIfIndex(ifIndexX)
                      .setScope(RT_SCOPE_UNIVERSE)
                      .build();
    netlinkSocket.addIfAddress(std::move(ifAddr));

    fbnl::IfAddressBuilder ifBuilderV4;
    auto ifAddrV4 = ifBuilderV4.setPrefix(kIpAddr3)
                        .setIfIndex(ifIndexX)
                        .setScope(RT_SCOPE_UNIVERSE)
                        .build();
    netlinkSocket.addIfAddress(std::move(ifAddrV4));
  }

  eventThread.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  // Verify the get method
  auto links = netlinkSocket.getAllLinks().get();

  EXPECT_EQ(1, links.count(kVethNameX));
  EXPECT_EQ(1, links.count(kVethNameY));
  EXPECT_EQ(3, links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, links.at(kVethNameY).networks.size());
  EXPECT_TRUE(allEventsReceived);
  EXPECT_EQ(3, myHandler->links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, myHandler->links.at(kVethNameY).networks.size());

  // Now remove the addresses
  myHandler->addrAddEventCount = 0;
  allEventsReceived = false;
  myHandler->replaceEventFunc([&] {
    VLOG(3) << "Received event from netlink";
    if (zmqLoop.isRunning() && myHandler->links.count(kVethNameX) &&
        myHandler->links.count(kVethNameY) &&
        myHandler->links.at(kVethNameX).isUp &&
        myHandler->links.at(kVethNameY).isUp &&
        // This is the ony change - wait for 1 addr delete events
        myHandler->addrDelEventCount == 2) {
      VLOG(3) << "Expected events received. Stopping zmq event loop";
      allEventsReceived = true;
      zmqLoop.stop();
    }
  });

  timeout = setTimeout();
  std::thread eventThread2([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  {
    // Now delete interface addresses using NetlinkSocket and check if events
    // are created
    int ifIndexX = netlinkSocket.getIfIndex(kVethNameX).get();
    fbnl::IfAddressBuilder ifBuilder;
    auto ifAddr = ifBuilder.setPrefix(kIpAddr1)
                      .setIfIndex(ifIndexX)
                      .setScope(RT_SCOPE_UNIVERSE)
                      .build();
    netlinkSocket.delIfAddress(std::move(ifAddr));

    fbnl::IfAddressBuilder ifBuilderV4;
    auto ifAddrV4 = ifBuilderV4.setPrefix(kIpAddr3)
                        .setIfIndex(ifIndexX)
                        .setScope(RT_SCOPE_UNIVERSE)
                        .build();
    netlinkSocket.delIfAddress(std::move(ifAddrV4));
  }

  eventThread2.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  // Verify that the addresses disappeared (ipv6 link local addresses remains)
  links = netlinkSocket.getAllLinks().get();
  EXPECT_EQ(1, links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, links.at(kVethNameY).networks.size());
  EXPECT_EQ(1, myHandler->links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, myHandler->links.at(kVethNameY).networks.size());
  EXPECT_TRUE(allEventsReceived);
}

// Link event flag test
// Subscribe/Unsubscribe link event
TEST_F(NetlinkSocketSubscribeFixture, LinkEventFlagTest) {
  ZmqEventLoop zmqLoop;

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  auto setTimeout = [&] {
    return zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
      VLOG(3) << "Timeout waiting for events... ";
      zmqLoop.stop();
    });
  };
  int timeout = setTimeout();

  bool allEventsReceived = false;

  // We expect both links to be up after which we stop the zmq loop
  // This func helps stop zmq loop when expected events are processed
  std::shared_ptr<MyNetlinkHandler> myHandler =
      std::make_shared<MyNetlinkHandler>(
          [&]() noexcept {
            VLOG(3) << "Received event from netlink";
            if (zmqLoop.isRunning() && myHandler->links.count(kVethNameX) &&
                myHandler->links.count(kVethNameY) &&
                myHandler->links.at(kVethNameX).isUp &&
                myHandler->links.at(kVethNameY).isUp &&
                myHandler->addrAddEventCount == 0 &&
                myHandler->addrDelEventCount == 0) {
              VLOG(3) << "Expected events received. Stopping zmq event loop";
              allEventsReceived = true;
              zmqLoop.stop();
            }
          },
          "vethTest" /* Filter on test links only */);

  // Only enable link event
  NetlinkSocket netlinkSocket(
      &zmqLoop, myHandler.get(), std::move(nlProtocolSocket));
  myHandler->setNetlinkSocket(&netlinkSocket);
  netlinkSocket.unsubscribeAllEvents();
  netlinkSocket.subscribeEvent(fbnl::LINK_EVENT);

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();
  VLOG(3) << "Hello";

  // Now emulate the links going up. This will generate link-local addresses
  // We deliberately choose system calls here to completely
  // decouple from netlink socket behavior being tested
  {
    auto cmd = "ip link set dev {} up"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
    cmd = "ip link set dev {} up"_shellify(kVethNameY.c_str());
    folly::Subprocess proc1(std::move(cmd));
    EXPECT_EQ(0, proc1.wait().exitStatus());
  }

  eventThread.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  EXPECT_TRUE(allEventsReceived);
  EXPECT_LE(2, myHandler->linkAddEventCount);
  EXPECT_LE(0, myHandler->linkDelEventCount);
  EXPECT_EQ(0, myHandler->addrAddEventCount);
  EXPECT_EQ(0, myHandler->addrDelEventCount);
  EXPECT_EQ(0, myHandler->neighborAddEventCount);
  EXPECT_EQ(0, myHandler->neighborDelEventCount);

  // Now bring the links down and disable link event handler
  myHandler->linkAddEventCount = 0;
  myHandler->linkDelEventCount = 0;
  netlinkSocket.unsubscribeEvent(fbnl::LINK_EVENT);

  timeout = setTimeout();
  std::thread eventThread2([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  // Shutdown links
  {
    auto cmd = "ip link set dev {} down"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
    cmd = "ip link set dev {} down"_shellify(kVethNameY.c_str());
    folly::Subprocess proc1(std::move(cmd));
    EXPECT_EQ(0, proc1.wait().exitStatus());
  }

  eventThread2.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  EXPECT_EQ(0, myHandler->linkAddEventCount);
  EXPECT_EQ(0, myHandler->linkDelEventCount);
  EXPECT_EQ(0, myHandler->addrAddEventCount);
  EXPECT_EQ(0, myHandler->addrDelEventCount);
  EXPECT_EQ(0, myHandler->neighborAddEventCount);
  EXPECT_EQ(0, myHandler->neighborDelEventCount);
}

// Subscribe/Unsubscribe neighbor event
TEST_F(NetlinkSocketSubscribeFixture, NeighEventFlagTest) {
  ZmqEventLoop zmqLoop;

  auto neighborKey1 = std::make_pair(kVethNameX, kNextHopIp1);
  auto neighborKey2 = std::make_pair(kVethNameY, kNextHopIp2);

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  auto setTimeout = [&] {
    return zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
      VLOG(3) << "Timeout waiting for events... ";
      zmqLoop.stop();
    });
  };
  int timeout = setTimeout();

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
    VLOG(3) << "Timeout waiting for events... ";
    zmqLoop.stop();
  });

  // We expect link and neighbor events to be delivered
  // 4 neighbor event (create and delete)
  // This func helps stop zmq loop when expected events are processed
  std::shared_ptr<MyNetlinkHandler> myHandler =
      std::make_shared<MyNetlinkHandler>(
          [&]() noexcept {
            VLOG(3) << "Received event from netlink";
            if (zmqLoop.isRunning() &&
                myHandler->neighbors.count(neighborKey1) == 1 &&
                myHandler->neighborAddEventCount >= 1) {
              VLOG(3) << "Expected events received. Stopping zmq event loop";
              zmqLoop.stop();
            }
          },
          "vethTest" /* Filter on test links only */);

  NetlinkSocket netlinkSocket(
      &zmqLoop, myHandler.get(), std::move(nlProtocolSocket));
  myHandler->setNetlinkSocket(&netlinkSocket);
  // Only enable neighbor event
  netlinkSocket.unsubscribeAllEvents();
  netlinkSocket.subscribeEvent(fbnl::NEIGH_EVENT);

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

  eventThread.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  // Now verify our events
  // 1+ neighbor events
  EXPECT_LE(1, myHandler->neighborAddEventCount);
  EXPECT_LE(0, myHandler->neighborDelEventCount);
  EXPECT_EQ(0, myHandler->linkAddEventCount);
  EXPECT_EQ(0, myHandler->linkDelEventCount);
  EXPECT_EQ(0, myHandler->addrAddEventCount);
  EXPECT_EQ(0, myHandler->addrDelEventCount);

  // Unsubscribe neighbor event
  netlinkSocket.unsubscribeEvent(fbnl::NEIGH_EVENT);
  myHandler->neighborAddEventCount = 0;

  // Now delete both the neighbor entries from the system
  timeout = setTimeout();
  std::thread eventThread2([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  deleteTestNeighborEntry(kVethNameX, kNextHopIp1, kLinkAddr1);

  eventThread2.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  EXPECT_LE(0, myHandler->neighborAddEventCount);
  EXPECT_LE(0, myHandler->neighborDelEventCount);
  EXPECT_EQ(0, myHandler->linkAddEventCount);
  EXPECT_EQ(0, myHandler->linkDelEventCount);
  EXPECT_EQ(0, myHandler->addrAddEventCount);
  EXPECT_EQ(0, myHandler->addrDelEventCount);
}

// Subscribe/Unsubscribe addr event
TEST_F(NetlinkSocketSubscribeFixture, AddrEventFlagTest) {
  ZmqEventLoop zmqLoop;

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  auto setTimeout = [&] {
    return zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
      VLOG(3) << "Timeout waiting for events... ";
      zmqLoop.stop();
    });
  };
  int timeout = setTimeout();
  bool allEventsReceived = false;

  // We expect both links to be up after which we stop the zmq loop
  // This func helps stop zmq loop when expected events are processed
  std::shared_ptr<MyNetlinkHandler> myHandler =
      std::make_shared<MyNetlinkHandler>(
          [&]() noexcept {
            VLOG(3) << "Received event from netlink";
            if (zmqLoop.isRunning() && myHandler->addrAddEventCount == 4) {
              VLOG(3) << "Expected events received. Stopping zmq event loop";
              allEventsReceived = true;
              zmqLoop.stop();
            }
          },
          "vethTest" /* Filter on test links only */);

  NetlinkSocket netlinkSocket(
      &zmqLoop, myHandler.get(), std::move(nlProtocolSocket));
  myHandler->setNetlinkSocket(&netlinkSocket);
  // Only subscribe addr event
  netlinkSocket.unsubscribeAllEvents();
  netlinkSocket.subscribeEvent(fbnl::ADDR_EVENT);

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  // Now emulate the links going up. This will generate link-local addresses.
  // Also add manual IPs
  // We deliberately choose system calls here to completely
  // decouple from netlink socket behavior being tested
  {
    auto cmd = "ip link set dev {} up"_shellify(kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());

    cmd = "ip link set dev {} up"_shellify(kVethNameY.c_str());
    folly::Subprocess proc1(std::move(cmd));
    EXPECT_EQ(0, proc1.wait().exitStatus());

    cmd = "ip addr add {} dev {}"_shellify(
        folly::IPAddress::networkToString(kIpAddr1).c_str(),
        kVethNameX.c_str());
    folly::Subprocess proc2(std::move(cmd));
    EXPECT_EQ(0, proc2.wait().exitStatus());

    cmd = "ip addr add {} dev {}"_shellify(
        folly::IPAddress::networkToString(kIpAddr2).c_str(),
        kVethNameY.c_str());
    folly::Subprocess proc3(std::move(cmd));
    EXPECT_EQ(0, proc3.wait().exitStatus());
  }
  eventThread.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  EXPECT_TRUE(allEventsReceived);
  EXPECT_LE(4, myHandler->addrAddEventCount);
  EXPECT_LE(0, myHandler->addrDelEventCount);
  EXPECT_EQ(0, myHandler->neighborAddEventCount);
  EXPECT_EQ(0, myHandler->neighborDelEventCount);
  EXPECT_EQ(0, myHandler->linkAddEventCount);
  EXPECT_EQ(0, myHandler->linkDelEventCount);

  // Now remove the addresses
  myHandler->addrAddEventCount = 0;
  // Unsubscribe addr event
  netlinkSocket.unsubscribeEvent(fbnl::ADDR_EVENT);

  timeout = setTimeout();
  std::thread eventThread2([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  {
    auto cmd = "ip addr del {} dev {}"_shellify(
        folly::IPAddress::networkToString(kIpAddr1).c_str(),
        kVethNameX.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());

    cmd = "ip addr del {} dev {}"_shellify(
        folly::IPAddress::networkToString(kIpAddr2).c_str(),
        kVethNameY.c_str());
    folly::Subprocess proc1(std::move(cmd));
    EXPECT_EQ(0, proc1.wait().exitStatus());
  }
  eventThread2.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  EXPECT_LE(0, myHandler->addrAddEventCount);
  EXPECT_LE(0, myHandler->addrDelEventCount);
  EXPECT_EQ(0, myHandler->neighborAddEventCount);
  EXPECT_EQ(0, myHandler->neighborDelEventCount);
  EXPECT_EQ(0, myHandler->linkAddEventCount);
  EXPECT_EQ(0, myHandler->linkDelEventCount);
}

// Flap multiple links up and down and stress test link events
TEST_F(NetlinkSocketSubscribeFixture, MultipleLinkFlapTest) {
  ZmqEventLoop zmqLoop;

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  zmqLoop.scheduleTimeout(kStressTestEventLoopTimeout, [&]() noexcept {
    VLOG(3) << "Timeout waiting for events... ";
    zmqLoop.stop();
  });

  // Create large number of links to stress test link flapping
  const int linkCount{100};
  const int flapCount{10};
  for (int i = 0; i < linkCount; i++) {
    std::string vethNameA{"vethTestA" + std::to_string(i)};
    std::string vethNameB{"vethTestB" + std::to_string(i)};
    auto cmd = "ip link add {} type veth peer name {}"_shellify(
        vethNameA.c_str(), vethNameB.c_str());
    folly::Subprocess proc(std::move(cmd));
    EXPECT_EQ(0, proc.wait().exitStatus());
  }

  // After we flap a large number of links,
  // we will bring vethTestX and vethtestY up, after which we stop the zmq loop
  // This func helps stop zmq loop when expected events are processed
  std::shared_ptr<MyNetlinkHandler> myHandler =
      std::make_shared<MyNetlinkHandler>(
          [&]() noexcept {
            VLOG(3) << "Received event from netlink";
            if (zmqLoop.isRunning() && myHandler->links.count(kVethNameX) &&
                myHandler->links.count(kVethNameY) &&
                myHandler->links.at(kVethNameX).isUp &&
                myHandler->links.at(kVethNameY).isUp &&
                myHandler->links.at(kVethNameX).networks.size() == 1 &&
                myHandler->links.at(kVethNameY).networks.size() == 1) {
              LOG(INFO) << "Expected events received. Stopping zmq event loop";
              zmqLoop.stop();
            }
          },
          "vethTest" /* Filter on test links only */);

  NetlinkSocket netlinkSocket(
      &zmqLoop, myHandler.get(), std::move(nlProtocolSocket));
  myHandler->setNetlinkSocket(&netlinkSocket);
  netlinkSocket.subscribeAllEvents();

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() {
    zmqLoop.run();
    zmqLoop.waitUntilStopped();
  });

  zmqLoop.waitUntilRunning();

  // We emulate 100 links going up and down several times
  for (int flap = 0; flap < flapCount; flap++) {
    for (int i = 0; i < linkCount; i++) {
      std::string vethNameA{"vethTestA" + std::to_string(i)};
      auto cmdA = "ip link set dev {} up"_shellify(vethNameA.c_str());
      folly::Subprocess procA(std::move(cmdA));
      EXPECT_EQ(0, procA.wait().exitStatus());

      std::string vethNameB{"vethTestB" + std::to_string(i)};
      auto cmdB = "ip link set dev {} up"_shellify(vethNameB.c_str());
      folly::Subprocess procB(std::move(cmdB));
      EXPECT_EQ(0, procB.wait().exitStatus());
    }

    for (int i = 0; i < linkCount; i++) {
      std::string vethNameA{"vethTestA" + std::to_string(i)};
      auto cmdA = "ip link set dev {} down"_shellify(vethNameA.c_str());
      folly::Subprocess procA(std::move(cmdA));
      EXPECT_EQ(0, procA.wait().exitStatus());

      std::string vethNameB{"vethTestB" + std::to_string(i)};
      auto cmdB = "ip link set dev {} down"_shellify(vethNameB.c_str());
      folly::Subprocess procB(std::move(cmdB));
      EXPECT_EQ(0, procB.wait().exitStatus());
    }
  }

  // Now bring vethTestX and vethTestY up to end the test
  auto cmd = "ip link set dev {} up"_shellify(kVethNameX.c_str());
  folly::Subprocess proc(std::move(cmd));
  EXPECT_EQ(0, proc.wait().exitStatus());

  cmd = "ip link set dev {} up"_shellify(kVethNameY.c_str());
  folly::Subprocess proc1(std::move(cmd));
  EXPECT_EQ(0, proc1.wait().exitStatus());

  zmqLoop.waitUntilStopped();
  eventThread.join();

  // Verify our link flap events are received via callback
  EXPECT_LE(2 * flapCount * linkCount, myHandler->linkAddEventCount);
  EXPECT_LE(2 * flapCount * linkCount, myHandler->linkDelEventCount);

  for (int i = 0; i < linkCount; i++) {
    std::string vethNameA{"vethTestA" + std::to_string(i)};
    std::string vethNameB{"vethTestB" + std::to_string(i)};
    auto cmdA = "ip link delete {}"_shellify(vethNameA.c_str());
    folly::Subprocess procA(std::move(cmdA));
    EXPECT_EQ(0, procA.wait().exitStatus());
  }
}

// Add and remove 250 IPv4 and IPv6 addresses (total 500)
TEST_F(NetlinkSocketSubscribeFixture, AddrScaleTest) {
  ZmqEventLoop zmqLoop;

  const int addrCount{250};
  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  auto setTimeout = [&] {
    return zmqLoop.scheduleTimeout(kStressTestEventLoopTimeout, [&]() noexcept {
      VLOG(3) << "Timeout waiting for events... ";
      zmqLoop.stop();
    });
  };
  int timeout = setTimeout();

  bool allEventsReceived = false;

  // This func helps stop zmq loop when expected events are processed
  std::shared_ptr<MyNetlinkHandler> myHandler =
      std::make_shared<MyNetlinkHandler>(
          [&]() noexcept {
            VLOG(3) << "Received event from netlink";
            if (zmqLoop.isRunning() &&
                myHandler->addrAddEventCount == 2 + 4 * addrCount) {
              VLOG(3) << "Expected events received. Stopping zmq event loop";
              allEventsReceived = true;
              zmqLoop.stop();
            }
          },
          "vethTest" /* Filter on test links only */);

  NetlinkSocket netlinkSocket(
      &zmqLoop, myHandler.get(), std::move(nlProtocolSocket));
  myHandler->setNetlinkSocket(&netlinkSocket);
  netlinkSocket.subscribeEvent(fbnl::ADDR_EVENT);

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  // Now emulate the links going up. This will generate link-local addresses.
  // Also add manual IPs
  // We deliberately choose system calls here to completely
  // decouple from netlink socket behavior being tested
  auto cmd = "ip link set dev {} up"_shellify(kVethNameX.c_str());
  folly::Subprocess proc(std::move(cmd));
  EXPECT_EQ(0, proc.wait().exitStatus());

  cmd = "ip link set dev {} up"_shellify(kVethNameY.c_str());
  folly::Subprocess proc1(std::move(cmd));
  EXPECT_EQ(0, proc1.wait().exitStatus());

  for (int i = 0; i < addrCount; i++) {
    std::string ipAddr1 = "face:d00d::" + std::to_string(i) + "/128";
    cmd = "ip addr add {} dev {}"_shellify(ipAddr1, kVethNameX.c_str());
    folly::Subprocess proc2(std::move(cmd));
    EXPECT_EQ(0, proc2.wait().exitStatus());

    std::string ipAddr2 = "face:1001::" + std::to_string(i) + "/128";
    cmd = "ip addr add {} dev {}"_shellify(ipAddr2, kVethNameY.c_str());
    folly::Subprocess proc3(std::move(cmd));
    EXPECT_EQ(0, proc3.wait().exitStatus());

    std::string ipAddr3 = "10.0." + std::to_string(i) + ".0/8";
    cmd = "ip addr add {} dev {}"_shellify(ipAddr3, kVethNameX.c_str());
    folly::Subprocess proc4(std::move(cmd));
    EXPECT_EQ(0, proc4.wait().exitStatus());

    std::string ipAddr4 = "10.1." + std::to_string(i) + ".0/8";
    cmd = "ip addr add {} dev {}"_shellify(ipAddr4, kVethNameY.c_str());
    folly::Subprocess proc5(std::move(cmd));
    EXPECT_EQ(0, proc5.wait().exitStatus());
  }

  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);
  eventThread.join();

  // Verify the get method
  auto links = netlinkSocket.getAllLinks().get();

  EXPECT_EQ(1, links.count(kVethNameX));
  EXPECT_EQ(1, links.count(kVethNameY));
  EXPECT_EQ(1 + 2 * addrCount, links.at(kVethNameX).networks.size());
  EXPECT_EQ(1 + 2 * addrCount, links.at(kVethNameY).networks.size());
  // 2 link-local, and rest are our IPs on VethTestX and Y
  EXPECT_EQ(2 + 4 * addrCount, myHandler->addrAddEventCount);
  EXPECT_TRUE(allEventsReceived);

  // Now remove the addresses
  myHandler->addrAddEventCount = 0;
  allEventsReceived = false;
  myHandler->replaceEventFunc([&] {
    VLOG(3) << "Received event from netlink";
    if (zmqLoop.isRunning() && myHandler->addrDelEventCount == 4 * addrCount) {
      VLOG(3) << "Expected events received. Stopping zmq event loop";
      allEventsReceived = true;
      zmqLoop.stop();
    }
  });

  timeout = setTimeout();
  std::thread eventThread2([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  for (int i = 0; i < addrCount; i++) {
    std::string ipAddr1 = "face:d00d::" + std::to_string(i) + "/128";
    cmd = "ip addr del {} dev {}"_shellify(ipAddr1, kVethNameX.c_str());
    folly::Subprocess proc2(std::move(cmd));
    EXPECT_EQ(0, proc2.wait().exitStatus());

    std::string ipAddr2 = "face:1001::" + std::to_string(i) + "/128";
    cmd = "ip addr del {} dev {}"_shellify(ipAddr2, kVethNameY.c_str());
    folly::Subprocess proc3(std::move(cmd));
    EXPECT_EQ(0, proc3.wait().exitStatus());

    std::string ipAddr3 = "10.0." + std::to_string(i) + ".0/8";
    cmd = "ip addr del {} dev {}"_shellify(ipAddr3, kVethNameX.c_str());
    folly::Subprocess proc4(std::move(cmd));
    EXPECT_EQ(0, proc4.wait().exitStatus());

    std::string ipAddr4 = "10.1." + std::to_string(i) + ".0/8";
    cmd = "ip addr del {} dev {}"_shellify(ipAddr4, kVethNameY.c_str());
    folly::Subprocess proc5(std::move(cmd));
    EXPECT_EQ(0, proc5.wait().exitStatus());
  }

  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);
  eventThread2.join();

  // Verify that the addresses disappeared (ipv6 link local addresses remains)
  links = netlinkSocket.getAllLinks().get();
  EXPECT_EQ(1, links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, links.at(kVethNameY).networks.size());
  EXPECT_EQ(1, myHandler->links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, myHandler->links.at(kVethNameY).networks.size());
  EXPECT_EQ(4 * addrCount, myHandler->addrDelEventCount);
  EXPECT_TRUE(allEventsReceived);
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
