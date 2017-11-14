/**
 * Copyright 20__-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the license found in the
 * LICENSE-examples file in the root directory of this source tree.
 */

#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include <folly/Exception.h>
#include <folly/Format.h>
#include <folly/MacAddress.h>

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

#include <openr/nl/NetlinkIfSocket.h>

using namespace openr;

DEFINE_bool(
    create_test_if,
    false,
    "Create Test interface (veth) so user can flap them");

namespace {
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");
const int kPollTimeout = 1000; // ms
} // namespace

// This fixture creates virtual interface (veths)
// which the monitor can then flap for testing
class NetlinkTestIf final {
 public:
  explicit NetlinkTestIf() {
    // Not handling errors here ...
    LOG(INFO) << "Create veths to test interface ..";
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
IfEventListenerFunc(int signalFd) {
  LOG(INFO) << "Creating a listener for Interface events ..";

  auto ifEventHandler = [](const std::string& ifName, bool isUp) -> void {
    LOG(INFO) << folly::sformat(
        "Event on Interface {} -> isUp ? {}", ifName.c_str(), isUp);
  };

  // Create the netlinkIfSocket object
  NetlinkIfSocket netlinkIfSocket(ifEventHandler);

  // Setup polling for netlink socket and signal fd
  struct pollfd fds[] = {
      {
          .fd = netlinkIfSocket.getSocketFd(),
          .events = POLLIN,
          .revents = 0,
      },
      {
          .fd = signalFd,
          .events = POLLIN,
          .revents = 0,
      },
  };

  LOG(INFO) << "Waiting for interface events. hit CTRL+C (SIGINT) to stop";

  while (1) {
    poll(&fds[0], (sizeof(fds) / sizeof(struct pollfd)), kPollTimeout);

    if (fds[0].revents & POLLIN) {
      netlinkIfSocket.dataReady();
      LOG(INFO) << "Waiting for interface events. hit CTRL+C (SIGINT) to stop";
    }
    if (fds[1].revents & POLLIN) {
      LOG(INFO) << "Received a signal to stop. Exiting ..";
      break;
    }
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  // Install a signal handler. This is needed to do cleanup
  // while allowing this to run like a daemon listening to events
  // forever. Currently the cleanup is only for the test interface
  // which we may optionally create, but we install this handler
  // always anyway...
  int signalFd{-1};
  sigset_t signalMask;

  sigemptyset(&signalMask);
  sigaddset(&signalMask, SIGINT);
  sigaddset(&signalMask, SIGQUIT);
  if (pthread_sigmask(SIG_BLOCK, &signalMask, nullptr) == -1) {
    LOG(ERROR) << "Could not set signal masks";
    return -1;
  }
  signalFd = signalfd(-1, &signalMask, 0);
  if (signalFd == -1) {
    LOG(ERROR) << "Could not set signal fd";
    return -1;
  }

  // We optionally create a test interface for the user to
  // flap and validate this monitoring tool
  std::shared_ptr<NetlinkTestIf> testIf;
  if (FLAGS_create_test_if) {
    LOG(INFO) << "Creating test veth interface " << kVethNameX << "/"
              << kVethNameY << " for user";
    testIf = std::make_shared<NetlinkTestIf>();
  }

  // signalFd is for the thread to catch signals and exit cleanly
  // and destroy test interface we may have created
  LOG(INFO) << "Starting thread for interface event listener ..";
  std::thread t(IfEventListenerFunc, signalFd);
  t.join();

  return 0;
}
