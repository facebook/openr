/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>

#include <openr/tests/scale/InterruptibleCommandReader.h>

namespace openr {
namespace {

class Pipe {
 public:
  Pipe() {
    if (::pipe2(fds_.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
      fds_ = {-1, -1};
    }
  }

  ~Pipe() {
    for (const int fd : fds_) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  }

  bool
  valid() const {
    return fds_[0] >= 0;
  }

  int
  readFd() const {
    return fds_[0];
  }

  int
  writeFd() const {
    return fds_[1];
  }

  void
  closeWriteFd() {
    if (fds_[1] >= 0) {
      ::close(fds_[1]);
      fds_[1] = -1;
    }
  }

 private:
  std::array<int, 2> fds_;
};

} // namespace

TEST(InterruptibleCommandReaderTest, StopWakesReaderWhileInputRemainsOpen) {
  Pipe input;
  Pipe stop;
  ASSERT_TRUE(input.valid());
  ASSERT_TRUE(stop.valid());

  std::vector<std::string> commands;
  folly::Baton<> commandsRead;
  folly::Baton<> readerExited;
  std::thread reader([&]() {
    readCommandsUntilStopped(
        input.readFd(), stop.readFd(), [&](std::string command) {
          commands.push_back(std::move(command));
          if (commands.size() == 2) {
            commandsRead.post();
          }
        });
    readerExited.post();
  });

  constexpr std::string_view kInput{"down node-1\r\nup node-1\n"};
  const auto inputBytesWritten =
      ::write(input.writeFd(), kInput.data(), kInput.size());
  const bool readInTime = commandsRead.try_wait_for(std::chrono::seconds(1));

  constexpr char kStop = 1;
  const auto stopBytesWritten = ::write(stop.writeFd(), &kStop, sizeof(kStop));
  const bool stoppedInTime = readerExited.try_wait_for(std::chrono::seconds(1));

  if (!stoppedInTime) {
    input.closeWriteFd();
  }
  reader.join();

  EXPECT_EQ(static_cast<ssize_t>(kInput.size()), inputBytesWritten);
  EXPECT_TRUE(readInTime);
  EXPECT_EQ(static_cast<ssize_t>(sizeof(kStop)), stopBytesWritten);
  EXPECT_TRUE(stoppedInTime);
  EXPECT_EQ((std::vector<std::string>{"down node-1", "up node-1"}), commands);
}

} // namespace openr
