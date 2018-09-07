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
#include <folly/system/Shell.h>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

extern "C" {
#include <netlink/errno.h>
#include <netlink/netlink.h>
#include <netlink/route/link.h>
#include <netlink/route/link/veth.h>
#include <netlink/socket.h>
}

#include <openr/nl/NetlinkSubscriber.h>

using namespace openr;
using namespace fbzmq;
using namespace folly::literals::shell_literals;

namespace {
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");
const folly::IPAddress kNextHopIp1("fe80::1");
const folly::IPAddress kNextHopIp2("fe80::2");
const folly::IPAddress kNextHopIp3("fe80::3");
const folly::MacAddress kLinkAddr1("01:02:03:04:05:06");
const folly::MacAddress kLinkAddr2("01:02:03:04:05:07");
const folly::MacAddress kDefaultLinkAddr;
const folly::CIDRNetwork kIpAddr1{folly::IPAddress("face:b00c::1"), 128};
const folly::CIDRNetwork kIpAddr2{folly::IPAddress("face:b00c::2"), 128};
const folly::CIDRNetwork kIpAddr3{folly::IPAddress("10.0.0.1"), 32};
const folly::CIDRNetwork kIpAddr4{folly::IPAddress("10.0.0.2"), 32};
const folly::CIDRNetwork kPrefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
const folly::CIDRNetwork kPrefix2{folly::IPAddress("fc00:cafe:3::4"), 128};
const folly::CIDRNetwork kPrefix3{folly::IPAddress("fc00:cafe:3::5"), 128};
const std::chrono::milliseconds kEventLoopTimeout(5000);

void
addTestNeighborEntry(
    const std::string& ifName,
    const folly::IPAddress& nextHopIp,
    const folly::MacAddress& linkAddr) {
  std::system(folly::sformat(
                  "ip -6 neigh add {} lladdr {} nud reachable dev {}",
                  nextHopIp.str(),
                  linkAddr.toString(),
                  ifName)
                  .c_str());
  // A simple read to flush updates
  std::system(
      folly::sformat("ip -6 neigh ls dev {} > /dev/null 2>&1", ifName).c_str());
}

void
deleteTestNeighborEntry(
    const std::string& ifName,
    const folly::IPAddress& nextHopIp,
    const folly::MacAddress& linkAddr) {
  // Now delete the neighbor entry from the system
  std::system(folly::sformat(
                  "ip -6 neigh del {} lladdr {} nud reachable dev {}",
                  nextHopIp.str(),
                  linkAddr.toString(),
                  ifName)
                  .c_str());
  // A simple read to flush updates
  std::system(
      folly::sformat("ip -6 neigh ls dev {} > /dev/null 2>&1", ifName).c_str());
}

} // namespace

// This fixture creates virtual links (veths)
// which the UT can use
class NetlinkSubscriberFixture : public testing::Test {
 public:
  NetlinkSubscriberFixture() = default;
  ~NetlinkSubscriberFixture() override = default;

  void
  SetUp() override {
    // cleanup old interfaces in any
    std::system(folly::sformat("ip link del {}", kVethNameX).c_str());

    // Not handling errors here ...
    link_ = rtnl_link_veth_alloc();
    socket_ = nl_socket_alloc();
    nl_connect(socket_, NETLINK_ROUTE);
    rtnl_link_set_name(link_, kVethNameX.c_str());
    rtnl_link_set_name(rtnl_link_veth_get_peer(link_), kVethNameY.c_str());
    int err = rtnl_link_add(socket_, link_, NLM_F_CREATE);
    if (err != 0) {
      LOG(ERROR) << "Failed to add veth link: " << nl_geterror(err);
    }
  }

  void
  TearDown() override {
    rtnl_link_delete(socket_, link_);
    nl_socket_free(socket_);
    rtnl_link_veth_release(link_);
  }

 private:
  struct rtnl_link* link_{nullptr};
  struct nl_sock* socket_{nullptr};
};

// EventFunc is to let a UT do whatever it wants for the event
// (like keep track of certain number of events once event loop starts)
// ifNamePrefix is to filter events for certain links only
class MyNetlinkHandler final : public NetlinkSubscriber::Handler {
 public:
  MyNetlinkHandler(
      std::function<void()> eventFunc, const std::string& ifNamePrefix)
      : eventFunc(eventFunc), ifNamePrefix(ifNamePrefix){};
  MyNetlinkHandler() = default;
  ~MyNetlinkHandler() override = default;

  void
  replaceEventFunc(std::function<void()> eventFunc) {
    this->eventFunc = std::move(eventFunc);
  }

  void
  linkEventFunc(const LinkEntry& linkEntry) override {
    VLOG(3) << "**Link : " << linkEntry.ifName
            << (linkEntry.isUp ? " UP" : " DOWN");
    if (linkEntry.ifName.find(ifNamePrefix) == std::string::npos) {
      return;
    }
    links[linkEntry.ifName].isUp = linkEntry.isUp;
    ++linkEventCount;
    if (eventFunc) {
      eventFunc();
    }
  }

  void
  addrEventFunc(const AddrEntry& addrEntry) override {
    VLOG(3) << "**Address : "
            << folly::IPAddress::networkToString(addrEntry.network) << "@"
            << addrEntry.ifName << (addrEntry.isValid ? " ADDED" : " DELETED");
    if (addrEntry.ifName.find(ifNamePrefix) == std::string::npos) {
      return;
    }
    if (addrEntry.isValid) {
      links[addrEntry.ifName].networks.insert(addrEntry.network);
    } else {
      links[addrEntry.ifName].networks.erase(addrEntry.network);
    }
    ++addrEventCount;
    if (eventFunc) {
      eventFunc();
    }
  }

  void
  neighborEventFunc(const NeighborEntry& neighborEntry) override {
    VLOG(3) << "** Neighbor entry: " << neighborEntry.ifName << " : "
            << neighborEntry.destination.str() << " -> "
            << neighborEntry.linkAddress.toString()
            << (neighborEntry.isReachable ? " : Reachable" : " : Unreachable");

    // Ignore entries on unknown interfaces
    if (neighborEntry.ifName.find(ifNamePrefix) == std::string::npos) {
      return;
    }

    auto neighborKey = std::make_pair(
        std::move(neighborEntry.ifName), std::move(neighborEntry.destination));
    if (neighborEntry.isReachable) {
      neighbors.emplace(neighborKey, std::move(neighborEntry.linkAddress));
    } else {
      neighbors.erase(neighborKey);
    }
    ++neighborEventCount;
    if (eventFunc) {
      eventFunc();
    }
  }

  void routeEventFunc(
      const openr::RouteEntry& routeEntry) override {
    VLOG(3) << "** Route entry: " << "Dest : "
            << folly::IPAddress::networkToString(routeEntry.prefix)
            << (routeEntry.isDeleted ? " Deleted " : " Added");

    if (routeEntry.isDeleted) {
      routeDelEventCount++;
      routes.erase(routeEntry.prefix);
    } else {
      routeAddEventCount++;
      routes.erase(routeEntry.prefix);
      routes.emplace(routeEntry.prefix, routeEntry);
    }
    if (eventFunc) {
      eventFunc();
    }
  }

  // Making vars public so UT can easily check these
  int linkEventCount{0};
  int neighborEventCount{0};
  int addrEventCount{0};
  int routeDelEventCount{0};
  int routeAddEventCount{0};
  Links links;
  Neighbors neighbors;
  std::unordered_map<folly::CIDRNetwork, RouteEntry> routes;
  std::function<void()> eventFunc{nullptr};
  std::string ifNamePrefix{"vethTest"};

 private:
  MyNetlinkHandler(const MyNetlinkHandler&) = delete;
  MyNetlinkHandler& operator=(const MyNetlinkHandler&) = delete;
};

// By default our test veth links should be down
// and there should be no neighbor entries
// We also do not start our zmq event loop, so there should be
// no events delivered
TEST_F(NetlinkSubscriberFixture, DefaultStateTest) {
  ZmqEventLoop zmqLoop;
  MyNetlinkHandler myHandler;

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);

  auto links = netlinkSubscriber.getAllLinks();
  auto neighbors = netlinkSubscriber.getAllReachableNeighbors();

  // Verify we have the link state
  ASSERT_EQ(1, links.count(kVethNameX));
  ASSERT_EQ(1, links.count(kVethNameY));

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
  EXPECT_TRUE(myHandler.links.empty());
  EXPECT_TRUE(myHandler.neighbors.empty());
  EXPECT_EQ(0, myHandler.linkEventCount);
  EXPECT_EQ(0, myHandler.neighborEventCount);
  EXPECT_EQ(0, myHandler.addrEventCount);
  EXPECT_EQ(0, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);
}

// Flap a link and test for events
// Also get and verify links states in main thread
TEST_F(NetlinkSubscriberFixture, LinkFlapTest) {
  ZmqEventLoop zmqLoop;

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
    VLOG(3) << "Timeout waiting for events... ";
    zmqLoop.stop();
  });

  // We expect both links to be up after which we stop the zmq loop
  // This func helps stop zmq loop when expected events are processed
  MyNetlinkHandler myHandler(
      [&]() noexcept {
        VLOG(3) << "Received event from netlink";
        if (zmqLoop.isRunning() &&
            myHandler.links.count(kVethNameX) &&
            myHandler.links.count(kVethNameY) &&
            myHandler.links.at(kVethNameX).isUp &&
            myHandler.links.at(kVethNameY).isUp &&
            myHandler.links.at(kVethNameX).networks.size() == 1 &&
            myHandler.links.at(kVethNameY).networks.size() == 1) {
          LOG(INFO) << "Expected events received. Stopping zmq event loop";
          zmqLoop.stop();
        }
      },
      "vethTest" /* Filter on test links only */);

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);

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
  std::system(
      folly::to<std::string>("ip link set dev ", kVethNameX, " up").c_str());
  std::system(
      folly::to<std::string>("ip link set dev ", kVethNameY, " up").c_str());

  zmqLoop.waitUntilStopped();

  // Verify the get* methods
  auto links = netlinkSubscriber.getAllLinks();
  auto neighbors = netlinkSubscriber.getAllReachableNeighbors();

  // Verify we have the link state
  ASSERT_EQ(1, links.count(kVethNameX));
  ASSERT_EQ(1, links.count(kVethNameY));

  // Verify links are up
  EXPECT_TRUE(links.at(kVethNameX).isUp);
  EXPECT_TRUE(links.at(kVethNameY).isUp);

  // Verify links have link-local addresses
  EXPECT_EQ(1, links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, links.at(kVethNameY).networks.size());

  // Now verify our events we received via callback

  // >= 2 link events and both links are up
  // (veths report link down and then up)
  EXPECT_EQ(2, myHandler.links.size());
  EXPECT_LE(2, myHandler.linkEventCount);
  ASSERT_EQ(1, myHandler.links.count(kVethNameX));
  ASSERT_EQ(1, myHandler.links.count(kVethNameY));
  EXPECT_TRUE(myHandler.links.at(kVethNameX).isUp);
  EXPECT_TRUE(myHandler.links.at(kVethNameY).isUp);
  EXPECT_EQ(2, myHandler.addrEventCount);
  EXPECT_EQ(1, myHandler.links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, myHandler.links.at(kVethNameY).networks.size());

  EXPECT_EQ(0, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);
  eventThread.join();
}

// Create a dummy link
// Bring it up
// Add a neighbor entry via this link
// Now delete link
// Verify neighbor entry is unreachable and link is deleted
// Also get and verify links and neighbor states in main thread
TEST(NetlinkSubscriberTest, LinkDeleteTest) {
  ZmqEventLoop zmqLoop;

  // override veth names to dummy as we are not using the fixture
  const std::string kVethNameX("veth-dummy-a");
  const std::string kVethNameY("veth-dummy-b");
  auto neighborKey = std::make_pair(kVethNameX, kNextHopIp1);

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
    VLOG(3) << "Timeout waiting for events... ";
    zmqLoop.stop();
  });

  // We expect link and neighbor events to be delivered
  // 4 link events (2 for each; create and delete)
  // 1+ neighbor event (create and delete (although state is stale))
  // This func helps stop zmq loop when expected events are processed
  MyNetlinkHandler myHandler(
      [&]() noexcept {
        VLOG(3) << "Received event from netlink";
        if (zmqLoop.isRunning() && myHandler.links.count(kVethNameX) &&
            myHandler.links.count(kVethNameY) &&
            myHandler.neighbors.count(neighborKey) == 0 &&
            myHandler.linkEventCount == 4 &&
            myHandler.neighborEventCount >= 1) {
          VLOG(3) << "Expected events received. Stopping zmq event loop";
          zmqLoop.stop();
        }
      },
      "veth-dummy-" /* Filter on test links only */);

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() {
    zmqLoop.run();
    zmqLoop.waitUntilStopped();
  });

  zmqLoop.waitUntilRunning();

  // Now add a dummy interface which we will later remove to test deletion
  std::system(
      folly::sformat(
          "ip link add name {} type veth peer name {}", kVethNameX, kVethNameY)
          .c_str());

  SCOPE_FAIL {
    std::system(folly::sformat("ip link del dev {}", kVethNameX).c_str());
  };

  // Now add a neighbor entry in the system
  addTestNeighborEntry(kVethNameX, kNextHopIp1, kLinkAddr1);

  // Verify the get* methods
  auto links = netlinkSubscriber.getAllLinks();
  auto neighbors = netlinkSubscriber.getAllReachableNeighbors();

  // Verify we have the link state
  ASSERT_EQ(1, links.count(kVethNameX));
  ASSERT_EQ(1, links.count(kVethNameY));

  // Verify links are not up
  EXPECT_FALSE(links.at(kVethNameX).isUp);
  EXPECT_FALSE(links.at(kVethNameY).isUp);

  // Verify neighbor entry we added
  ASSERT_EQ(1, neighbors.count(neighborKey));
  EXPECT_EQ(kLinkAddr1, neighbors.at(neighborKey));

  // Now verify our events
  // 1+ neighbor events
  EXPECT_EQ(1, myHandler.neighbors.size());
  EXPECT_LE(1, myHandler.neighborEventCount);
  ASSERT_EQ(1, myHandler.neighbors.count(neighborKey));
  EXPECT_EQ(kLinkAddr1, myHandler.neighbors.at(neighborKey));

  // 2 link events (creation) and both links are down
  EXPECT_EQ(2, myHandler.links.size());
  EXPECT_EQ(2, myHandler.linkEventCount);
  ASSERT_EQ(1, myHandler.links.count(kVethNameX));
  ASSERT_EQ(1, myHandler.links.count(kVethNameY));
  EXPECT_FALSE(myHandler.links.at(kVethNameX).isUp);
  EXPECT_FALSE(myHandler.links.at(kVethNameY).isUp);

  // Now delete the dummy interface which we test for in our handler
  std::system(folly::sformat("ip link del dev {}", kVethNameX).c_str());

  // Again Verify the get* methods
  links = netlinkSubscriber.getAllLinks();
  neighbors = netlinkSubscriber.getAllReachableNeighbors();

  // Verify we have the link state
  ASSERT_EQ(1, links.count(kVethNameX));
  ASSERT_EQ(1, links.count(kVethNameY));

  // Verify links are not up
  EXPECT_FALSE(links.at(kVethNameX).isUp);
  EXPECT_FALSE(links.at(kVethNameY).isUp);

  // Verify neighbor entry is deleted
  ASSERT_EQ(0, neighbors.count(neighborKey));

  // Now verify our events
  // 1+ neighbor events
  EXPECT_EQ(0, myHandler.neighbors.size());
  EXPECT_LE(1, myHandler.neighborEventCount);
  ASSERT_EQ(0, myHandler.neighbors.count(neighborKey));

  // >= 2 link events and both links are up
  // (veths report link down and then up)
  EXPECT_EQ(2, myHandler.links.size());
  EXPECT_EQ(4, myHandler.linkEventCount);
  ASSERT_EQ(1, myHandler.links.count(kVethNameX));
  ASSERT_EQ(1, myHandler.links.count(kVethNameY));
  EXPECT_FALSE(myHandler.links.at(kVethNameX).isUp);
  EXPECT_FALSE(myHandler.links.at(kVethNameY).isUp);

  EXPECT_EQ(0, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);
  eventThread.join();
}

// Add 2 neighbor entries via test link
// Verify events are received
// Now delete the entries one by one
// Verify neighbor entry is reachable
// Also get and verify links and neighbor states in main thread
TEST_F(NetlinkSubscriberFixture, NeighborMultipleEventTest) {
  ZmqEventLoop zmqLoop;
  auto neighborKey1 = std::make_pair(kVethNameX, kNextHopIp1);
  auto neighborKey2 = std::make_pair(kVethNameY, kNextHopIp2);

  // A timeout to stop the UT in case we never received expected events
  // UT will mostly likely fail as our checks at the end will fail
  zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
    VLOG(3) << "Timeout waiting for events... ";
    zmqLoop.stop();
  });

  // We expect link and neighbor events to be delivered
  // 4 neighbor event (create and delete)
  // This func helps stop zmq loop when expected events are processed
  MyNetlinkHandler myHandler(
      [&]() noexcept {
        VLOG(3) << "Received event from netlink";
        if (zmqLoop.isRunning() &&
            myHandler.neighbors.count(neighborKey1) == 0 &&
            myHandler.neighbors.count(neighborKey2) == 0 &&
            myHandler.neighborEventCount >= 4) {
          VLOG(3) << "Expected events received. Stopping zmq event loop";
          zmqLoop.stop();
        }
      },
      "vethTest" /* Filter on test links only */);

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);

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
  auto links = netlinkSubscriber.getAllLinks();
  auto neighbors = netlinkSubscriber.getAllReachableNeighbors();

  // Verify we have the link state
  ASSERT_EQ(1, links.count(kVethNameX));
  ASSERT_EQ(1, links.count(kVethNameY));

  // Verify links are not up
  EXPECT_FALSE(links.at(kVethNameX).isUp);
  EXPECT_FALSE(links.at(kVethNameY).isUp);

  // Verify neighbor entry we added
  ASSERT_EQ(1, neighbors.count(neighborKey1));
  EXPECT_EQ(kLinkAddr1, neighbors.at(neighborKey1));

  // Now verify our events
  // 1+ neighbor events
  EXPECT_EQ(1, myHandler.neighbors.size());
  EXPECT_LE(1, myHandler.neighborEventCount);
  ASSERT_EQ(1, myHandler.neighbors.count(neighborKey1));
  EXPECT_EQ(kLinkAddr1, myHandler.neighbors.at(neighborKey1));

  // No link events
  EXPECT_EQ(0, myHandler.links.size());
  EXPECT_EQ(0, myHandler.linkEventCount);

  // Now add another neighbor entry in the system
  addTestNeighborEntry(kVethNameY, kNextHopIp2, kLinkAddr2);

  // Again Verify the get* methods
  links = netlinkSubscriber.getAllLinks();
  neighbors = netlinkSubscriber.getAllReachableNeighbors();

  // Verify we have the link state
  ASSERT_EQ(1, links.count(kVethNameX));
  ASSERT_EQ(1, links.count(kVethNameY));

  // Verify neighbor entry we added
  ASSERT_EQ(1, neighbors.count(neighborKey2));
  EXPECT_EQ(kLinkAddr2, neighbors.at(neighborKey2));

  // Now verify our events
  // 2+ neighbor events
  EXPECT_EQ(2, myHandler.neighbors.size());
  EXPECT_LE(2, myHandler.neighborEventCount);
  ASSERT_EQ(1, myHandler.neighbors.count(neighborKey2));
  EXPECT_EQ(kLinkAddr2, myHandler.neighbors.at(neighborKey2));

  // Now delete both the neighbor entries from the system
  deleteTestNeighborEntry(kVethNameX, kNextHopIp1, kLinkAddr1);
  deleteTestNeighborEntry(kVethNameY, kNextHopIp2, kLinkAddr2);

  // Now verify our events
  // 4 neighbor events
  // But neighbors should be down now..
  EXPECT_EQ(0, myHandler.neighbors.size());
  EXPECT_LE(4, myHandler.neighborEventCount);

  // No link events
  EXPECT_EQ(0, myHandler.links.size());
  EXPECT_EQ(0, myHandler.linkEventCount);

  // Read again and verify get* methods
  links = netlinkSubscriber.getAllLinks();
  neighbors = netlinkSubscriber.getAllReachableNeighbors();

  // no new events
  EXPECT_EQ(0, myHandler.neighbors.size());
  EXPECT_LE(4, myHandler.neighborEventCount);

  // No link events
  EXPECT_EQ(0, myHandler.links.size());
  EXPECT_EQ(0, myHandler.linkEventCount);

  // Verify we have the link state
  ASSERT_EQ(1, links.count(kVethNameX));
  ASSERT_EQ(1, links.count(kVethNameY));

  // Verify links are not up
  EXPECT_FALSE(links.at(kVethNameX).isUp);
  EXPECT_FALSE(links.at(kVethNameY).isUp);

  // Verify neighbor entry we deleted
  ASSERT_EQ(0, neighbors.count(neighborKey1));
  ASSERT_EQ(0, neighbors.count(neighborKey2));

  EXPECT_EQ(0, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);
  eventThread.join();
}

// Flap link and check that link-local IPs appear and disappear
TEST_F(NetlinkSubscriberFixture, AddrLinkFlapTest) {
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
  MyNetlinkHandler myHandler(
      [&]() noexcept {
        VLOG(3) << "Received event from netlink";
        if (zmqLoop.isRunning() && myHandler.links.count(kVethNameX) &&
            myHandler.links.count(kVethNameY) &&
            myHandler.links.at(kVethNameX).isUp &&
            myHandler.links.at(kVethNameY).isUp &&
            myHandler.addrEventCount == 2) {
          VLOG(3) << "Expected events received. Stopping zmq event loop";
          allEventsReceived = true;
          zmqLoop.stop();
        }
      },
      "vethTest" /* Filter on test links only */);

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() { zmqLoop.run(); });

  zmqLoop.waitUntilRunning();

  // Now emulate the links going up. This will generate link-local addresses
  // We deliberately choose system calls here to completely
  // decouple from netlink socket behavior being tested
  std::system(
      folly::to<std::string>("ip link set dev ", kVethNameX, " up").c_str());
  std::system(
      folly::to<std::string>("ip link set dev ", kVethNameY, " up").c_str());

  eventThread.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  // Verify the get method
  auto links = netlinkSubscriber.getAllLinks();

  EXPECT_EQ(1, links.count(kVethNameX));
  EXPECT_EQ(1, links.count(kVethNameY));
  EXPECT_EQ(1, links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, links.at(kVethNameY).networks.size());
  EXPECT_TRUE(allEventsReceived);
  EXPECT_EQ(1, myHandler.links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, myHandler.links.at(kVethNameY).networks.size());

  // Now bring the links down
  myHandler.addrEventCount = 0;
  allEventsReceived = false;
  myHandler.replaceEventFunc([&] {
    VLOG(3) << "Received event from netlink";
    if (zmqLoop.isRunning() && myHandler.links.count(kVethNameX) &&
        myHandler.links.count(kVethNameY) &&
        // We check for !isUp here, the rest is the same
        !myHandler.links.at(kVethNameX).isUp &&
        !myHandler.links.at(kVethNameY).isUp && myHandler.addrEventCount == 2) {
      VLOG(3) << "Expected events received. Stopping zmq event loop";
      allEventsReceived = true;
      zmqLoop.stop();
    }
  });

  timeout = setTimeout();
  std::thread eventThread4([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  std::system(folly::sformat("ip link set dev {} down", kVethNameX).c_str());
  std::system(folly::sformat("ip link set dev {} down", kVethNameY).c_str());

  eventThread4.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  // Verify that the addresses were deleted and we got the events
  links = netlinkSubscriber.getAllLinks();
  EXPECT_EQ(0, links.at(kVethNameX).networks.size());
  EXPECT_EQ(0, links.at(kVethNameY).networks.size());
  EXPECT_EQ(0, myHandler.links.at(kVethNameX).networks.size());
  EXPECT_EQ(0, myHandler.links.at(kVethNameY).networks.size());
  EXPECT_TRUE(allEventsReceived);

  EXPECT_EQ(0, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);
}

// Add and remove IPs
TEST_F(NetlinkSubscriberFixture, AddrAddRemoveTest) {
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
  MyNetlinkHandler myHandler(
      [&]() noexcept {
        VLOG(3) << "Received event from netlink";
        if (zmqLoop.isRunning() && myHandler.links.count(kVethNameX) &&
            myHandler.links.count(kVethNameY) &&
            myHandler.links.at(kVethNameX).isUp &&
            myHandler.links.at(kVethNameY).isUp &&
            myHandler.addrEventCount == 6) {
          VLOG(3) << "Expected events received. Stopping zmq event loop";
          allEventsReceived = true;
          zmqLoop.stop();
        }
      },
      "vethTest" /* Filter on test links only */);

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);

  // Run the zmq event loop in its own thread
  // We will either timeout if expected events are not received
  // or stop after we receive expected events
  std::thread eventThread([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  // Now emulate the links going up. This will generate link-local addresses.
  // Also add manual IPs
  // We deliberately choose system calls here to completely
  // decouple from netlink socket behavior being tested
  std::system(
      folly::to<std::string>("ip link set dev ", kVethNameX, " up").c_str());
  std::system(
      folly::to<std::string>("ip link set dev ", kVethNameY, " up").c_str());
  std::system(folly::sformat(
                  "ip addr add {} dev {}",
                  folly::IPAddress::networkToString(kIpAddr1),
                  kVethNameX)
                  .c_str());
  std::system(folly::sformat(
                  "ip addr add {} dev {}",
                  folly::IPAddress::networkToString(kIpAddr2),
                  kVethNameY)
                  .c_str());

  // Add v4 addresses
  std::system(folly::sformat(
                  "ip addr add {} dev {}",
                  folly::IPAddress::networkToString(kIpAddr3),
                  kVethNameX)
                  .c_str());
  std::system(folly::sformat(
                  "ip addr add {} dev {}",
                  folly::IPAddress::networkToString(kIpAddr4),
                  kVethNameY)
                  .c_str());

  eventThread.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  // Verify the get method
  auto links = netlinkSubscriber.getAllLinks();

  EXPECT_EQ(1, links.count(kVethNameX));
  EXPECT_EQ(1, links.count(kVethNameY));
  EXPECT_EQ(3, links.at(kVethNameX).networks.size());
  EXPECT_EQ(3, links.at(kVethNameY).networks.size());
  EXPECT_TRUE(allEventsReceived);
  EXPECT_EQ(3, myHandler.links.at(kVethNameX).networks.size());
  EXPECT_EQ(3, myHandler.links.at(kVethNameY).networks.size());

  // Now remove the addresses
  myHandler.addrEventCount = 0;
  allEventsReceived = false;
  myHandler.replaceEventFunc([&] {
    VLOG(3) << "Received event from netlink";
    if (zmqLoop.isRunning() && myHandler.links.count(kVethNameX) &&
        myHandler.links.count(kVethNameY) &&
        myHandler.links.at(kVethNameX).isUp &&
        myHandler.links.at(kVethNameY).isUp &&
        // This is the ony change - wait for 4 addr delete events
        myHandler.addrEventCount == 4) {
      VLOG(3) << "Expected events received. Stopping zmq event loop";
      allEventsReceived = true;
      zmqLoop.stop();
    }
  });

  timeout = setTimeout();
  std::thread eventThread2([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  std::system(folly::sformat(
                  "ip addr del {} dev {}",
                  folly::IPAddress::networkToString(kIpAddr1),
                  kVethNameX)
                  .c_str());
  std::system(folly::sformat(
                  "ip addr del {} dev {}",
                  folly::IPAddress::networkToString(kIpAddr2),
                  kVethNameY)
                  .c_str());

  std::system(folly::sformat(
                  "ip addr del {} dev {}",
                  folly::IPAddress::networkToString(kIpAddr3),
                  kVethNameX)
                  .c_str());
  std::system(folly::sformat(
                  "ip addr del {} dev {}",
                  folly::IPAddress::networkToString(kIpAddr4),
                  kVethNameY)
                  .c_str());

  eventThread2.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  // Verify that the addresses disappeared (ipv6 link local addresses remains)
  links = netlinkSubscriber.getAllLinks();
  EXPECT_EQ(1, links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, links.at(kVethNameY).networks.size());
  EXPECT_EQ(1, myHandler.links.at(kVethNameX).networks.size());
  EXPECT_EQ(1, myHandler.links.at(kVethNameY).networks.size());
  EXPECT_TRUE(allEventsReceived);

  EXPECT_EQ(0, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);
}

// Link event flag test
// Subscribe/Unsubscribe link event
TEST_F(NetlinkSubscriberFixture, LinkEventFlagTest) {
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
  MyNetlinkHandler myHandler(
      [&]() noexcept {
        VLOG(3) << "Received event from netlink";
        if (zmqLoop.isRunning() && myHandler.links.count(kVethNameX) &&
            myHandler.links.count(kVethNameY) &&
            myHandler.links.at(kVethNameX).isUp &&
            myHandler.links.at(kVethNameY).isUp &&
            myHandler.addrEventCount == 0) {
          VLOG(3) << "Expected events received. Stopping zmq event loop";
          allEventsReceived = true;
          zmqLoop.stop();
        }
      },
      "vethTest" /* Filter on test links only */);

  // Only enable link event
  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);
  netlinkSubscriber.unsubscribeAllEvents();
  netlinkSubscriber.subscribeEvent(fbnl::LINK_EVENT);

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
  EXPECT_LE(2, myHandler.linkEventCount);
  EXPECT_EQ(0, myHandler.addrEventCount);
  EXPECT_EQ(0, myHandler.neighborEventCount);

  // Now bring the links down and disable link event handler
  myHandler.linkEventCount = 0;
  netlinkSubscriber.unsubscribeEvent(fbnl::LINK_EVENT);

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

  EXPECT_EQ(0, myHandler.linkEventCount);
  EXPECT_EQ(0, myHandler.addrEventCount);
  EXPECT_EQ(0, myHandler.neighborEventCount);
  EXPECT_EQ(0, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);
}

// Subscribe/Unsubscribe neighbor event
TEST_F(NetlinkSubscriberFixture, NeighEventFlagTest) {
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
  MyNetlinkHandler myHandler(
      [&]() noexcept {
        VLOG(3) << "Received event from netlink";
        if (zmqLoop.isRunning() &&
            myHandler.neighbors.count(neighborKey1) == 1 &&
            myHandler.neighborEventCount >= 1) {
          VLOG(3) << "Expected events received. Stopping zmq event loop";
          zmqLoop.stop();
        }
      },
      "vethTest" /* Filter on test links only */);

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);
  // Only enable neighbor event
  netlinkSubscriber.unsubscribeAllEvents();
  netlinkSubscriber.subscribeEvent(fbnl::NEIGH_EVENT);

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
  EXPECT_LE(1, myHandler.neighborEventCount);
  EXPECT_EQ(0, myHandler.linkEventCount);
  EXPECT_EQ(0, myHandler.addrEventCount);
  EXPECT_EQ(0, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);


  // Unsubscribe neighbor event
  netlinkSubscriber.unsubscribeEvent(fbnl::NEIGH_EVENT);
  myHandler.neighborEventCount = 0;

  // Now delete both the neighbor entries from the system
  timeout = setTimeout();
  std::thread eventThread2([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  deleteTestNeighborEntry(kVethNameX, kNextHopIp1, kLinkAddr1);

  eventThread2.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  EXPECT_LE(0, myHandler.neighborEventCount);
  EXPECT_EQ(0, myHandler.linkEventCount);
  EXPECT_EQ(0, myHandler.addrEventCount);
  EXPECT_EQ(0, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);
}

// Subscribe/Unsubscribe addr event
TEST_F(NetlinkSubscriberFixture, AddrEventFlagTest) {
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
  MyNetlinkHandler myHandler(
      [&]() noexcept {
        VLOG(3) << "Received event from netlink";
        if (zmqLoop.isRunning() &&
            myHandler.addrEventCount == 4) {
          VLOG(3) << "Expected events received. Stopping zmq event loop";
          allEventsReceived = true;
          zmqLoop.stop();
        }
      },
      "vethTest" /* Filter on test links only */);

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);
  // Only subscribe addr event
  netlinkSubscriber.unsubscribeAllEvents();
  netlinkSubscriber.subscribeEvent(fbnl::ADDR_EVENT);

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
      folly::IPAddress::networkToString(kIpAddr1).c_str(), kVethNameX.c_str());
    folly::Subprocess proc2(std::move(cmd));
    EXPECT_EQ(0, proc2.wait().exitStatus());

    cmd = "ip addr add {} dev {}"_shellify(
      folly::IPAddress::networkToString(kIpAddr2).c_str(), kVethNameY.c_str());
    folly::Subprocess proc3(std::move(cmd));
    EXPECT_EQ(0, proc3.wait().exitStatus());
  }
  eventThread.join();
  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  EXPECT_TRUE(allEventsReceived);
  EXPECT_LE(4, myHandler.addrEventCount);
  EXPECT_EQ(0, myHandler.neighborEventCount);
  EXPECT_EQ(0, myHandler.linkEventCount);
  EXPECT_EQ(0, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);

  // Now remove the addresses
  myHandler.addrEventCount = 0;
  // Unsubscribe addr event
  netlinkSubscriber.unsubscribeEvent(fbnl::ADDR_EVENT);

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

  EXPECT_LE(0, myHandler.addrEventCount);
  EXPECT_EQ(0, myHandler.neighborEventCount);
  EXPECT_EQ(0, myHandler.linkEventCount);
  EXPECT_EQ(0, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);
}

// Add/Del route, test route events
TEST_F(NetlinkSubscriberFixture, RouteTest) {
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

  // We expect both links to be up after which we stop the zmq loop
  // This func helps stop zmq loop when expected events are processed
  MyNetlinkHandler myHandler(
      [&]() noexcept {
        VLOG(3) << "Received event from netlink";
        if (zmqLoop.isRunning() &&
            myHandler.routes.count(kPrefix1) &&
            myHandler.routes.count(kPrefix2) &&
            myHandler.routes.count(kPrefix3)) {
          LOG(INFO) << "Expected events received. Stopping zmq event loop";
          zmqLoop.stop();
        }
      },
      "vethTest" /* Filter on test links only */);

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);
  netlinkSubscriber.subscribeAllEvents();

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

  cmd = "ip addr add {} dev {}"_shellify(
    folly::IPAddress::networkToString(kIpAddr1).c_str(), kVethNameX.c_str());
  folly::Subprocess proc2(std::move(cmd));
  EXPECT_EQ(0, proc2.wait().exitStatus());

  cmd = "ip addr add {} dev {}"_shellify(
    folly::IPAddress::networkToString(kIpAddr2).c_str(), kVethNameY.c_str());
  folly::Subprocess proc3(std::move(cmd));
  EXPECT_EQ(0, proc3.wait().exitStatus());

  // Adding routes with system calls
  cmd = "ip route add {} via {} dev {}"_shellify(
    folly::IPAddress::networkToString(kPrefix1).c_str(),
    kNextHopIp1.str().c_str(), kVethNameX.c_str());
  folly::Subprocess proc4(std::move(cmd));
  EXPECT_EQ(0, proc4.wait().exitStatus());
  cmd = "ip route add {} via {} dev {}"_shellify(
    folly::IPAddress::networkToString(kPrefix2).c_str(),
    kNextHopIp2.str().c_str(), kVethNameX.c_str());
  folly::Subprocess proc5(std::move(cmd));
  EXPECT_EQ(0, proc5.wait().exitStatus());
  cmd = "ip route add {} via {} dev {}"_shellify(
    folly::IPAddress::networkToString(kPrefix3).c_str(),
    kNextHopIp3.str().c_str(), kVethNameX.c_str());
  folly::Subprocess proc6(std::move(cmd));
  EXPECT_EQ(0, proc6.wait().exitStatus());

  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);
  eventThread.join();

  EXPECT_LE(3, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);

  // Verify routes
  EXPECT_EQ(1, myHandler.routes.count(kPrefix1));
  EXPECT_EQ(1, myHandler.routes.count(kPrefix2));
  EXPECT_EQ(1, myHandler.routes.count(kPrefix3));

  myHandler.routeAddEventCount = 0;
  myHandler.replaceEventFunc([&] {
    VLOG(3) << "Received event from netlink";
    if (zmqLoop.isRunning() &&
        !myHandler.routes.count(kPrefix1) &&
        !myHandler.routes.count(kPrefix2) &&
        !myHandler.routes.count(kPrefix3)) {
      VLOG(3) << "Expected events received. Stopping zmq event loop";
      zmqLoop.stop();
    }
  });
  timeout = setTimeout();
  std::thread eventThread2([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  // Delete routes
  cmd = "ip route delete {} via {}"_shellify(
    folly::IPAddress::networkToString(kPrefix1).c_str(),
    kNextHopIp1.str().c_str());
  folly::Subprocess proc7(std::move(cmd));
  EXPECT_EQ(0, proc7.wait().exitStatus());
  cmd = "ip route delete {} via {}"_shellify(
    folly::IPAddress::networkToString(kPrefix2).c_str(),
    kNextHopIp2.str().c_str());
  folly::Subprocess proc8(std::move(cmd));
  EXPECT_EQ(0, proc8.wait().exitStatus());
  cmd = "ip route delete {} via {}"_shellify(
    folly::IPAddress::networkToString(kPrefix3).c_str(),
    kNextHopIp3.str().c_str());
  folly::Subprocess proc9(std::move(cmd));
  EXPECT_EQ(0, proc9.wait().exitStatus());

  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  EXPECT_EQ(3, myHandler.routeDelEventCount);

  // Verify routes deleted
  EXPECT_EQ(0, myHandler.routes.count(kPrefix1));
  EXPECT_EQ(0, myHandler.routes.count(kPrefix2));
  EXPECT_EQ(0, myHandler.routes.count(kPrefix3));

  eventThread2.join();
}

// Add/Del route, test route events
TEST_F(NetlinkSubscriberFixture, RouteFlagTest) {
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

  // We expect both links to be up after which we stop the zmq loop
  // This func helps stop zmq loop when expected events are processed
  MyNetlinkHandler myHandler(
      [&]() noexcept {
        VLOG(3) << "Received event from netlink";
        if (zmqLoop.isRunning() &&
            myHandler.routes.count(kPrefix1) &&
            myHandler.routes.count(kPrefix2) &&
            myHandler.routes.count(kPrefix3)) {
          LOG(INFO) << "Expected events received. Stopping zmq event loop";
          zmqLoop.stop();
        }
      },
      "vethTest" /* Filter on test links only */);

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);
  netlinkSubscriber.unsubscribeAllEvents();
  netlinkSubscriber.subscribeEvent(fbnl::ROUTE_EVENT);

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

  cmd = "ip addr add {} dev {}"_shellify(
    folly::IPAddress::networkToString(kIpAddr1).c_str(), kVethNameX.c_str());
  folly::Subprocess proc2(std::move(cmd));
  EXPECT_EQ(0, proc2.wait().exitStatus());

  cmd = "ip addr add {} dev {}"_shellify(
    folly::IPAddress::networkToString(kIpAddr2).c_str(), kVethNameY.c_str());
  folly::Subprocess proc3(std::move(cmd));
  EXPECT_EQ(0, proc3.wait().exitStatus());

  // Add route
  cmd = "ip route add {} via {} dev {}"_shellify(
    folly::IPAddress::networkToString(kPrefix1).c_str(),
    kNextHopIp1.str().c_str(), kVethNameX.c_str());
  folly::Subprocess proc4(std::move(cmd));
  EXPECT_EQ(0, proc4.wait().exitStatus());
  cmd = "ip route add {} via {} dev {}"_shellify(
    folly::IPAddress::networkToString(kPrefix2).c_str(),
    kNextHopIp2.str().c_str(), kVethNameX.c_str());
  folly::Subprocess proc5(std::move(cmd));
  EXPECT_EQ(0, proc5.wait().exitStatus());
  cmd = "ip route add {} via {} dev {}"_shellify(
    folly::IPAddress::networkToString(kPrefix3).c_str(),
    kNextHopIp3.str().c_str(), kVethNameX.c_str());
  folly::Subprocess proc6(std::move(cmd));
  EXPECT_EQ(0, proc6.wait().exitStatus());

  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);
  eventThread.join();

  EXPECT_LE(3, myHandler.routeAddEventCount);
  EXPECT_EQ(0, myHandler.routeDelEventCount);
  EXPECT_EQ(0, myHandler.addrEventCount);
  EXPECT_EQ(0, myHandler.neighborEventCount);
  EXPECT_EQ(0, myHandler.linkEventCount);

  // Verify routes
  EXPECT_EQ(1, myHandler.routes.count(kPrefix1));
  EXPECT_EQ(1, myHandler.routes.count(kPrefix2));
  EXPECT_EQ(1, myHandler.routes.count(kPrefix3));

  myHandler.routeAddEventCount = 0;
  netlinkSubscriber.unsubscribeEvent(fbnl::ROUTE_EVENT);
  timeout = setTimeout();
  std::thread eventThread2([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  // Delete routes
  cmd = "ip route delete {} via {}"_shellify(
    folly::IPAddress::networkToString(kPrefix1).c_str(),
    kNextHopIp1.str().c_str());
  folly::Subprocess proc7(std::move(cmd));
  EXPECT_EQ(0, proc7.wait().exitStatus());
  cmd = "ip route delete {} via {}"_shellify(
    folly::IPAddress::networkToString(kPrefix2).c_str(),
    kNextHopIp2.str().c_str());
  folly::Subprocess proc8(std::move(cmd));
  EXPECT_EQ(0, proc8.wait().exitStatus());
  cmd = "ip route delete {} via {}"_shellify(
    folly::IPAddress::networkToString(kPrefix3).c_str(),
    kNextHopIp3.str().c_str());
  folly::Subprocess proc9(std::move(cmd));
  EXPECT_EQ(0, proc9.wait().exitStatus());

  zmqLoop.waitUntilStopped();
  zmqLoop.cancelTimeout(timeout);

  EXPECT_LE(0, myHandler.routeDelEventCount);
  EXPECT_EQ(0, myHandler.addrEventCount);
  EXPECT_EQ(0, myHandler.neighborEventCount);
  EXPECT_EQ(0, myHandler.linkEventCount);

  eventThread2.join();
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
