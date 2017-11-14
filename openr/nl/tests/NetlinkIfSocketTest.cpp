/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <poll.h>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

#include <folly/Exception.h>
#include <folly/Format.h>
#include <folly/MacAddress.h>

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

#include <openr/nl/NetlinkException.h>
#include <openr/nl/NetlinkIfSocket.h>

using namespace openr;

namespace {
const std::string kVethNameX("vethTestX");
const std::string kVethNameY("vethTestY");
const auto kPollTotalTimeout = std::chrono::milliseconds(2000);
} // namespace

// This fixture creates virtual interface (veths)
// which the UT can then flap
class NetlinkIfFixture : public testing::Test {
 public:
  NetlinkIfFixture() = default;
  ~NetlinkIfFixture() override = default;

  void
  SetUp() override {
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

 protected:
  static int
  timeLeftToPoll(std::chrono::steady_clock::time_point end) {
    using namespace std::chrono;
    auto left = end - steady_clock::now();
    int ms = duration_cast<milliseconds>(left).count();
    return std::max(ms, 0);
  }
};

TEST_F(NetlinkIfFixture, IfEventTest) {
  std::map<std::string, bool> interfaceDb_{};
  int numEvents = 0;
  auto ifUpFunc = [&interfaceDb_,
                   &numEvents](const std::string& ifName, bool isUp) -> void {
    interfaceDb_[ifName] = isUp;
    ++numEvents;
  };

  const int kNumEventsExpected = 4;

  // Create the netlinkIfSocket object and setup polling
  NetlinkIfSocket netlinkIfSocket(ifUpFunc);

  // Now emulate the links going up
  // We deliberately choose system calls here to completely
  // decouple from netlink socket behavior being tested
  std::system(
      folly::to<std::string>("ip link set dev ", kVethNameX, " up").c_str());
  std::system(
      folly::to<std::string>("ip link set dev ", kVethNameY, " up").c_str());

  struct pollfd fds[] = {
      {
          .fd = netlinkIfSocket.getSocketFd(),
          .events = POLLIN,
          .revents = 0,
      },
  };

  auto end = std::chrono::steady_clock::now() + kPollTotalTimeout;
  while (1) {
    int pollTimeout = timeLeftToPoll(end);
    if (poll(fds, sizeof(fds) / sizeof(struct pollfd), pollTimeout) <= 0) {
      // break on error or timeout
      break;
    }
    if (fds[0].revents & POLLIN) {
      netlinkIfSocket.dataReady();
    }
    if (numEvents >= kNumEventsExpected) {
      break;
    }
  }

  // We expect 4 events, shrug at netlink
  // IFF_LOWERUP and then IFF_RUNNING for each of the interfaces
  EXPECT_EQ(kNumEventsExpected, numEvents);

  // Expect both interfaces to be up..
  EXPECT_TRUE(interfaceDb_[kVethNameX]);
  EXPECT_TRUE(interfaceDb_[kVethNameY]);
}

// Delete a interface and ensure that we get the notification
// as a down event
TEST_F(NetlinkIfFixture, IfDeleteTest) {
  std::map<std::string, bool> interfaceDb_{};
  int numEvents = 0;
  auto ifUpFunc = [&interfaceDb_,
                   &numEvents](const std::string& ifName, bool isUp) -> void {
    interfaceDb_[ifName] = isUp;
    ++numEvents;
  };

  const int kNumEventsExpected = 2;

  // Now add a dummy interface which we will later remove to test deletion
  std::system("ip link add name veth-dummy-a type veth peer name veth-dummy-b");

  // Create the netlinkIfSocket object and setup polling
  NetlinkIfSocket netlinkIfSocket(ifUpFunc);

  // Now delete the dummy interface which we test for in our handler
  std::system("ip link del dev veth-dummy-a");

  struct pollfd fds[] = {
      {
          .fd = netlinkIfSocket.getSocketFd(),
          .events = POLLIN,
          .revents = 0,
      },
  };

  auto end = std::chrono::steady_clock::now() + kPollTotalTimeout;
  while (1) {
    int pollTimeout = timeLeftToPoll(end);
    if (poll(fds, sizeof(fds) / sizeof(struct pollfd), pollTimeout) <= 0) {
      // break on error or timeout
      break;
    }
    if (fds[0].revents & POLLIN) {
      netlinkIfSocket.dataReady();
    }
    if (numEvents >= kNumEventsExpected) {
      break;
    }
  }

  // We expect 2 events, a DOWN notification for each interface
  EXPECT_EQ(kNumEventsExpected, numEvents);

  // Expect both interfaces to be down..
  EXPECT_FALSE(interfaceDb_["veth-dummy-a"]);
  EXPECT_FALSE(interfaceDb_["veth-dummy-b"]);
}

TEST(NetlinkIfTest, ChangeThreadGetFdTest) {
  auto ifUpFunc = [](const std::string&, bool) -> void {};

  int fd = -1;
  NetlinkIfSocket netlinkIfSocket(ifUpFunc);
  std::thread tempThread([&netlinkIfSocket, &fd]() {
    EXPECT_THROW((fd = netlinkIfSocket.getSocketFd()), NetlinkException);
  });
  tempThread.join();

  EXPECT_EQ(-1, fd);
}

TEST(NetlinkIfTest, ChangeThreadDataReadyTest) {
  auto ifUpFunc = [](const std::string&, bool) -> void {};

  NetlinkIfSocket netlinkIfSocket(ifUpFunc);
  std::thread tempThread([&netlinkIfSocket]() {
    EXPECT_THROW(netlinkIfSocket.dataReady(), NetlinkException);
  });
  tempThread.join();
}

TEST(NetlinkIfTest, CreateMultipleUniqueSocketsTest) {
  auto ifUpFunc = [](const std::string&, bool) -> void {};

  std::thread thread1([ifUpFunc]() {
    EXPECT_NO_THROW({ NetlinkIfSocket netlinkIfSocket(ifUpFunc); });
  });
  std::thread thread2([ifUpFunc]() {
    EXPECT_NO_THROW({ NetlinkIfSocket netlinkIfSocket(ifUpFunc); });
  });
  thread1.join();
  thread2.join();
}

TEST(NetlinkIfTest, CreateSocketsFromSameThreadTest) {
  auto ifUpFunc = [](const std::string&, bool) -> void {};

  std::thread tempThread([ifUpFunc]() {
    EXPECT_THROW(
        {
          NetlinkIfSocket netlinkIfSocket1(ifUpFunc);
          NetlinkIfSocket netlinkIfSocket2(ifUpFunc);
        },
        NetlinkException);
  });
  tempThread.join();
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
