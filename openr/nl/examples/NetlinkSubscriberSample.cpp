/**
 * Copyright 20__-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the license found in the
 * LICENSE-examples file in the root directory of this source tree.
 */

#include <cstdlib>
#include <string>
#include <thread>

#include <folly/String.h>
#include <folly/gen/Base.h>
#include <folly/gen/Core.h>

#include <gflags/gflags.h>
#include <glog/logging.h>

extern "C" {
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>

#include <netlink/errno.h>
#include <netlink/netlink.h>
#include <netlink/route/link.h>
#include <netlink/route/link/veth.h>
#include <netlink/socket.h>
}

#include <openr/nl/NetlinkSubscriber.h>

using namespace openr;
using namespace fbzmq;

DEFINE_bool(
    live_monitor,
    false,
    "Live subscribe to any updates to link and neighbor events");

DEFINE_bool(
    create_test_if,
    false,
    "Create Test interface (veth) so user can do some sandbox testing"
    "for live monitor mode");

namespace {
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");
} // namespace

// Used to create virtual interface (veths)
// so user can add neighbor entries and flap link.
// A "sandbox" sorts of ..
class NetlinkTestIf final {
 public:
  explicit NetlinkTestIf() {
    // Not handling errors here ...
    LOG(INFO) << "Create veths to test neighbor entries..";
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

  ~NetlinkTestIf() {
    rtnl_link_delete(socket_, link_);
    nl_socket_free(socket_);
    rtnl_link_veth_release(link_);
  }

 private:
  struct rtnl_link* link_{nullptr};
  struct nl_sock* socket_{nullptr};
};

void
LinkDump() {
  LOG(INFO) << "Starting Link Dump....";

  ZmqEventLoop zmqLoop;
  // base class handler just logs entries
  NetlinkSubscriber::Handler handler;

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &handler);

  LOG(INFO) << "Dumping Routes";
  LOG(INFO) << "==============";
  auto links = netlinkSubscriber.getAllLinks();

  for (const auto& kv : links) {
    LOG(INFO)
        << "Link: " << kv.first << (kv.second.isUp ? " UP" : " DOWN")
        << " IPs: "
        << folly::join(
               ", ",
               folly::gen::from(kv.second.networks) |
                   folly::gen::mapped([](const folly::CIDRNetwork& network) {
                     return folly::IPAddress::networkToString(network);
                   }) |
                   folly::gen::as<std::vector<std::string>>());
  }
  LOG(INFO) << "==============";
}

void
NeighborDump() {
  LOG(INFO) << "Starting Neighbor Dump....";

  ZmqEventLoop zmqLoop;
  // base class handler just logs entries
  NetlinkSubscriber::Handler handler;

  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &handler);

  LOG(INFO) << "Dumping all reachable Neighbors";
  LOG(INFO) << "==============";
  auto neighbors = netlinkSubscriber.getAllReachableNeighbors();

  for (const auto& kv : neighbors) {
    LOG(INFO) << "Neighbor: " << kv.first.second << " dev " << kv.first.first
              << " lladdr " << kv.second;
  }
  LOG(INFO) << "==============";
}

void
netlinkSubscriberThread(int sigFd) {
  LOG(INFO) << "Starting Subscriber....";

  ZmqEventLoop zmqLoop;

  class MyHandler final : public NetlinkSubscriber::Handler {
   public:
    MyHandler() = default;
    ~MyHandler() override = default;

    void
    linkEventFunc(const LinkEntry& linkEntry) override {
      LOG(INFO) << " ** Link : " << linkEntry.ifName
                << (linkEntry.isUp ? " UP" : " DOWN");
    }

    void
    neighborEventFunc(const NeighborEntry& neighborEntry) override {
      LOG(INFO)
          << " ** Neighbor entry: " << neighborEntry.ifName << " : "
          << neighborEntry.destination << " -> " << neighborEntry.linkAddress
          << (neighborEntry.isReachable ? " : Reachable" : " : Unreachable");
    }

    void
    addrEventFunc(const AddrEntry& addrEntry) override {
      LOG(INFO) << " ** Address: "
                << folly::IPAddress::networkToString(addrEntry.network)
                << " on link: " << addrEntry.ifName
                << (addrEntry.isValid ? " ADDED" : " DELETED");
    }

   private:
    MyHandler(const MyHandler&) = delete;
    MyHandler& operator=(const MyHandler&) = delete;
  };

  MyHandler myHandler;
  NetlinkSubscriber netlinkSubscriber(&zmqLoop, &myHandler);

  // if we receive a sigint / sigquit, stop the zmq loop
  zmqLoop.addSocketFd(sigFd, POLLIN, [&](int) noexcept { zmqLoop.stop(); });

  LOG(INFO) << "Starting zmq event loop";
  zmqLoop.run();
  zmqLoop.waitUntilStopped();
  LOG(INFO) << "Event loop stopped .. ";
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  LinkDump();
  NeighborDump();

  if (!FLAGS_live_monitor) {
    return 0;
  }

  // We optionally create a test interface for the user to
  // add neighbor entries against and validate this monitoring tool
  std::shared_ptr<NetlinkTestIf> testIf;
  if (FLAGS_create_test_if) {
    LOG(INFO) << "Creating test veth interface " << kVethNameX << "/"
              << kVethNameY << " for user";
    testIf = std::make_shared<NetlinkTestIf>();
  }

  LOG(INFO) << "Starting live monitoring of link and neighbor entries ....";

  // Install a signal handler. This is needed to do cleanup
  // while allowing this to run like a daemon listening to events
  // forever. Currently the cleanup is only for the test interface
  // which we may optionally create, but we install this handler
  // always anyway...
  int sigFd{-1};
  sigset_t signalMask;

  sigemptyset(&signalMask);
  sigaddset(&signalMask, SIGINT);
  sigaddset(&signalMask, SIGQUIT);
  if (pthread_sigmask(SIG_BLOCK, &signalMask, nullptr) == -1) {
    LOG(ERROR) << "Could not set signal masks";
    return -1;
  }
  sigFd = signalfd(-1, &signalMask, 0);
  if (sigFd == -1) {
    LOG(ERROR) << "Could not set signal fd";
    return -1;
  }

  // sigFd is for the thread to catch signals and exit cleanly
  // and destroy test interface we may have created
  LOG(INFO) << "Starting thread for our monitor ..";
  std::thread t(netlinkSubscriberThread, sigFd);
  t.join();

  return 0;
}
