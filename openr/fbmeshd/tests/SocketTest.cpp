/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/fbmeshd/gateway-connectivity-monitor/Socket.h>

#include <folly/Subprocess.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <sys/types.h>
#include <unistd.h>

using namespace openr::fbmeshd;

class SocketTest : public ::testing::Test {
 protected:
  static constexpr auto testInterface{"lo"};
  static const folly::SocketAddress testAddress;
  static constexpr std::chrono::seconds testInterval{1};
};

const folly::SocketAddress SocketTest::testAddress{"127.0.0.1", 1337};
constexpr std::chrono::seconds SocketTest::testInterval;

TEST_F(SocketTest, ConnectFailure) {
  Socket socket;
  Socket::Result result{
      socket.connect(testInterface, testAddress, testInterval)};
  EXPECT_FALSE(result.success);
  EXPECT_EQ(
      getuid() != 0 ? "setsockopt_bindtodevice" : "err_non_zero",
      result.errorMsg);
}

void
waitUntilListening(const folly::SocketAddress& key) {
  std::string stdOut{};
  do {
    folly::Subprocess ss{{"/usr/sbin/ss", std::string{"-tanpl"}},
                         folly::Subprocess::Options().pipeStdout()};
    stdOut = ss.communicate().first;
    EXPECT_EQ(0, ss.wait().exitStatus());
  } while (stdOut.find(key.describe()) == std::string::npos);
}

TEST_F(SocketTest, ConnectSuccess) {
  folly::Subprocess proc{{"/bin/nc",
                          "-l",
                          testAddress.getAddressStr(),
                          folly::to<std::string>(testAddress.getPort())}};
  waitUntilListening(testAddress);

  {
    Socket socket;
    Socket::Result result{
        socket.connect(testInterface, testAddress, testInterval)};

    // This test is really only interesting when run as root. But this makes it
    // so it will at least pass otherwise.
    if (getuid() == 0) {
      EXPECT_TRUE(result.success);
      EXPECT_EQ("", result.errorMsg);
    } else {
      proc.terminate();
    }
  }

  auto status = proc.wait();
  EXPECT_TRUE(getuid() != 0 || status.exitStatus() == 0);
}

int
main(int argc, char** argv) {
  ::google::InitGoogleLogging(argv[0]);
  ::testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  return RUN_ALL_TESTS();
}
