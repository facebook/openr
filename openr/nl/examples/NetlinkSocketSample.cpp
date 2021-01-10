/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the license found in the
 * LICENSE-examples file in the root directory of this source tree.
 */

#include <map>
#include <memory>
#include <string>
#include <thread>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Exception.h>
#include <folly/Format.h>
#include <folly/Subprocess.h>
#include <folly/gen/Base.h>
#include <folly/gen/Core.h>
#include <folly/gen/String.h>
#include <folly/system/Shell.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <openr/nl/NetlinkSocket.h>

using namespace openr;
using namespace openr::fbnl;
using namespace folly::literals::shell_literals;

namespace {
const uint8_t kAqRouteProtoId = 99;
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");
const folly::IPAddress kNextHopIp1("fe80::1");
const folly::IPAddress kNextHopIp2("fe80::2");
const folly::IPAddress kNextHopIp3("169.254.0.1");
const folly::IPAddress kNextHopIp4("169.254.0.2");
const folly::CIDRNetwork kPrefix1{folly::IPAddress("fc00:cafe:3::3"), 128};
const folly::CIDRNetwork kPrefix2{folly::IPAddress("192.168.0.11"), 32};
const std::chrono::milliseconds kEventLoopTimeout(5000);
} // namespace

/**
 * This example code will show basic functions of NetlinkSocket (fbnl)
 * 1. Route programming. (Add/Del route in route table)
 * 2. Address management (Add/Del/Get address)
 * 3. Events subscription, all the operations aforementioned will generate
 *    different types of events, and notify uplayer user.
 *
 * The process will be like this:
 * Create virtual interfaces
 * Do address operations
 * Do route programming (ECMP routing for IPv4, IPv6, Non-ECMP routing for IPv4)
 * Monitor events
 */

/**
 * For subscribing events, user should inherit NetlinkSocket::EventsHandler and
 * implement events handlers.
 * In this example, we just print received events
 */
class MyNetlinkHandler final : public NetlinkSocket::EventsHandler {
 public:
  MyNetlinkHandler() = default;
  ~MyNetlinkHandler() override = default;

  void
  linkEventFunc(const std::string&, const openr::fbnl::Link& linkEntry) noexcept
      override {
    std::string ifName = linkEntry.getLinkName();
    LOG(INFO) << "**Link : " << ifName << (linkEntry.isUp() ? " UP" : " DOWN");
    LOG(INFO) << "============================================================";
  }

  void
  addrEventFunc(
      const std::string&,
      const openr::fbnl::IfAddress& addrEntry) noexcept override {
    bool isValid = addrEntry.isValid();
    LOG(INFO)
        << "**Address : "
        << folly::IPAddress::networkToString(addrEntry.getPrefix().value())
        << "@IfaceIndex" << addrEntry.getIfIndex()
        << (isValid ? " ADDED" : " DELETED");
    LOG(INFO) << "============================================================";
  }

  void
  neighborEventFunc(
      const std::string&,
      const openr::fbnl::Neighbor& neighborEntry) noexcept override {
    LOG(INFO)
        << "** Neighbor entry: " << neighborEntry.getDestination().str()
        << " -> " << neighborEntry.getLinkAddress().value().toString()
        << (neighborEntry.isReachable() ? " : Reachable" : " : Unreachable");
    LOG(INFO) << "============================================================";
  }

 private:
  MyNetlinkHandler(const MyNetlinkHandler&) = delete;
  MyNetlinkHandler& operator=(const MyNetlinkHandler&) = delete;
};

// Creat virtual interface for testing
void
SetUp() {
  auto cmd = "ip link del {}"_shellify(kVethNameX.c_str());
  folly::Subprocess proc(std::move(cmd));
  proc.wait();
  cmd = "ip link del {}"_shellify(kVethNameY.c_str());
  folly::Subprocess proc1(std::move(cmd));
  proc1.wait();

  // add veth interface pair
  cmd = "ip link add {} type veth peer name {}"_shellify(
      kVethNameX.c_str(), kVethNameY.c_str());
  folly::Subprocess proc2(std::move(cmd));
  proc2.wait();

  // Bring up interface
  cmd = "ip link set dev {} up"_shellify(kVethNameX.c_str());
  folly::Subprocess proc3(std::move(cmd));
  proc3.wait();
  cmd = "ip link set dev {} up"_shellify(kVethNameY.c_str());
  folly::Subprocess proc4(std::move(cmd));
  proc4.wait();
}

void
TearDown() {
  // cleanup virtual interfaces
  auto cmd = "ip link del {} 2>/dev/null"_shellify(kVethNameX.c_str());
  folly::Subprocess proc(std::move(cmd));
  // Ignore result
  proc.wait();
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  SetUp();

  fbzmq::ZmqEventLoop zmqLoop;
  /**
   * Schedule timeout, wait for 5s to receive all events, it's enough for
   * NetlinkSocket to receive events, cause this is usually in
   * milliseconds level.
   */
  zmqLoop.scheduleTimeout(kEventLoopTimeout, [&]() noexcept {
    LOG(INFO) << "Timeout waiting for events... ";
    zmqLoop.stop();
  });

  std::shared_ptr<MyNetlinkHandler> myHandler =
      std::make_shared<MyNetlinkHandler>();

  NetlinkSocket netlinkSocket(&zmqLoop, myHandler.get());
  // By default, NetlinkSocket subscribes no event
  netlinkSocket.subscribeAllEvents();

  /**
   * We can start event loop in another thread, NetlinkSocket uses eventloop to
   * execute API calls, otherwise NetlinkSocket will execute in the caller
   * thread
   */
  std::thread eventThread([&]() { zmqLoop.run(); });
  zmqLoop.waitUntilRunning();

  // Adding address to interface
  // IPv6
  folly::CIDRNetwork prefixV6{folly::IPAddress("face:b00c::2"), 128};
  IfAddressBuilder builder;
  int ifIndex = netlinkSocket.getIfIndex(kVethNameX).get();
  auto ifAddr = builder.setPrefix(prefixV6).setIfIndex(ifIndex).build();
  netlinkSocket.addIfAddress(std::move(ifAddr)).get();
  // IPv4
  builder.reset();
  int ifIndex1 = netlinkSocket.getIfIndex(kVethNameY).get();
  const folly::CIDRNetwork prefixV4{folly::IPAddress("192.168.0.11"), 32};
  auto ifAddr1 = builder.setPrefix(prefixV4).setIfIndex(ifIndex1).build();
  netlinkSocket.addIfAddress(std::move(ifAddr1)).get();

  /**
   * Adding IPV6 route with two nexthops
   * (Currently, IPv6 only support ECMP, non-ECMP patch was just created on
   * 9 Jan, 2018,
   * https://www.mail-archive.com/netdev@vger.kernel.org/msg210214.html)
   * So in IPv6 even if we set different weight values we got the same weight
   * for each nexthop.
   */
  fbnl::RouteBuilder rtBuilderV6;
  // Set basic attributes
  rtBuilderV6.setDestination(kPrefix1)
      .setProtocolId(kAqRouteProtoId)
      .setScope(RT_SCOPE_UNIVERSE)
      .setType(RTN_UNICAST)
      .setRouteTable(RT_TABLE_MAIN);
  fbnl::NextHopBuilder nhBuilder;
  nhBuilder.setIfIndex(ifIndex).setGateway(kNextHopIp1).setWeight(1);
  rtBuilderV6.addNextHop(nhBuilder.build());
  nhBuilder.reset();
  nhBuilder.setIfIndex(ifIndex).setGateway(kNextHopIp2).setWeight(2);
  rtBuilderV6.addNextHop(nhBuilder.build());
  netlinkSocket.addRoute(rtBuilderV6.build()).get();
  /**
   * At this point we can see route has already been added to routing table
   * We can check through cmd ip -6 route, it will show:
   *
   * fc00:cafe:3::3 proto 99 metric 1024
   *      nexthop via fe80::1 dev vethTestX weight 1
   *      nexthop via fe80::2 dev vethTestX weight 1
   */

  /**
   * Add IPv4 route with two nexthops
   * IPv4 alread support ECMP and non-ECMP features
   */
  fbnl::RouteBuilder rtBuilderV4;
  rtBuilderV4.setDestination(kPrefix2)
      .setProtocolId(kAqRouteProtoId)
      .setScope(RT_SCOPE_UNIVERSE)
      .setType(RTN_UNICAST)
      .setRouteTable(RT_TABLE_MAIN);
  nhBuilder.reset();
  nhBuilder.setIfIndex(ifIndex).setGateway(kNextHopIp3).setWeight(1);
  rtBuilderV4.addNextHop(nhBuilder.build());
  nhBuilder.reset();
  nhBuilder.setIfIndex(ifIndex).setGateway(kNextHopIp4).setWeight(2);
  rtBuilderV4.addNextHop(nhBuilder.build());
  netlinkSocket.addRoute(rtBuilderV4.build()).get();
  /**
   * At this point we can see route has already been added to routing table
   * We can check through cmd ip -4 route, it will show a route with different
   * nexthop weight:
   * 192.168.0.11 proto 99
   *         nexthop via 169.254.0.1 dev vethTestX weight 2 onlink
   *         nexthop via 169.254.0.2 dev vethTestX weight 3 onlink
   */

  zmqLoop.waitUntilStopped();
  eventThread.join();

  return 0;
}
