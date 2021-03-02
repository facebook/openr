/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sys/eventfd.h>

#include <fbzmq/zmq/Context.h>
#include <fbzmq/zmq/Socket.h>
#include <folly/futures/Promise.h>
#include <folly/init/Init.h>
#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>

#include <openr/common/OpenrEventBase.h>

using namespace openr;

class OpenrEventBaseTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    evbThread_ = std::thread([&]() { evb.run(); });
    evb.waitUntilRunning();
  }

  void
  TearDown() override {
    evb.stop();
    evb.waitUntilStopped();
    evbThread_.join();
  }

 private:
  std::thread evbThread_;

 public:
  fbzmq::Context context;
  OpenrEventBase evb;
};

TEST(OpenrEventBaseTest, CreateDestroy) {
  OpenrEventBase evb;
  EXPECT_TRUE(evb.getEvb() != nullptr);
}

TEST(OpenrEventBaseTest, FiberTest) {
  // test addFiberTask()
  folly::Promise<folly::Unit> p1;
  auto sf = p1.getSemiFuture();
  OpenrEventBase evb;
  evb.addFiberTask(
      [p = std::move(p1)]() mutable noexcept { p.setValue(folly::Unit()); });
  evb.getEvb()->loopOnce();
  EXPECT_TRUE(sf.valid());
  EXPECT_TRUE(sf.isReady());
  EXPECT_TRUE(sf.hasValue());

  // test addFiberTaskAndGetFuture()
  folly::Promise<folly::Unit> p2;
  folly::Future<folly::Unit> f;
  f = evb.addFiberTaskFuture(
      [p = std::move(p2)]() mutable noexcept { p.setValue(folly::Unit()); });
  EXPECT_TRUE(f.valid());
  EXPECT_FALSE(f.isReady());

  evb.getEvb()->loopOnce();
  EXPECT_TRUE(f.isReady());
  EXPECT_TRUE(f.hasValue());
}

TEST(OpenrEventBaseTest, RunnableApi) {
  OpenrEventBase evb;

  // 1. Eventbase is not running
  EXPECT_FALSE(evb.isRunning());

  // 2. Run event base
  std::thread evbThread([&]() { evb.run(); });
  evb.waitUntilRunning();
  EXPECT_TRUE(evb.isRunning());

  // 3. Stop
  evb.stop();
  evb.waitUntilStopped();
  EXPECT_FALSE(evb.isRunning());
  evbThread.join();

  // 4. Restart
  evbThread = std::thread([&]() { evb.run(); });
  evb.waitUntilRunning();
  EXPECT_TRUE(evb.isRunning());

  // 5. Stop again
  evb.stop();
  evb.waitUntilStopped();
  EXPECT_FALSE(evb.isRunning());
  evbThread.join();
}

TEST(OpenrEventBaseTest, DefaultConstructor) {
  OpenrEventBase evb;
  folly::Baton waitBaton;
  evb.scheduleTimeout(
      std::chrono::milliseconds(100), [&]() { waitBaton.post(); });

  std::thread evbThread([&]() { evb.run(); });
  evb.waitUntilRunning();
  waitBaton.wait();
  evb.stop();
  evbThread.join();
}

TEST_F(OpenrEventBaseTestFixture, Timestamp) {
  // Expect non empty timestamp
  auto ts1 = evb.getTimestamp();
  EXPECT_GT(
      ts1, std::chrono::steady_clock::time_point(std::chrono::seconds(0)));

  // Sleep for a while
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // Verify timestamp increases
  auto ts2 = evb.getTimestamp();
  EXPECT_GE(ts2, ts1);

  // Stop thread
  evb.stop();
  evb.waitUntilStopped();

  // Verify timestamp doesn't increase when eventloop is not running
  auto ts3 = evb.getTimestamp();
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::seconds(2));
  auto ts4 = evb.getTimestamp();
  EXPECT_EQ(ts3, ts4);
}

TEST_F(OpenrEventBaseTestFixture, TimeoutTest) {
  folly::Baton waitBaton;

  const auto startTs = std::chrono::steady_clock::now();
  evb.getEvb()->runInEventBaseThread([&]() noexcept {
    evb.scheduleTimeout(std::chrono::milliseconds(200), [&]() {
      EXPECT_TRUE(true);
      waitBaton.post();
    });
  });

  waitBaton.wait();
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTs);
  EXPECT_LE(std::chrono::milliseconds(200), elapsedMs);
}

TEST_F(OpenrEventBaseTestFixture, ZmqSocketPollTest) {
  const auto msg = fbzmq::Message::from(std::string("test message")).value();
  const size_t expectedMsgs{16};
  std::atomic<size_t> rcvdMsgs{0};
  folly::Baton waitBaton;

  // Create PUB socket (ZMQ_PUB)
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> pubSocket{context};
  ASSERT_TRUE(pubSocket.bind(fbzmq::SocketUrl{"inproc://test"}).hasValue());

  // Define sub socket
  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> subSocket{
      context, folly::none, folly::none, fbzmq::NonblockingFlag{true}};
  ASSERT_TRUE(subSocket.connect(fbzmq::SocketUrl{"inproc://test"}).hasValue());
  ASSERT_TRUE(subSocket.setSockOpt(ZMQ_SUBSCRIBE, "", 0).hasValue());

  // Add sub socket to polling
  evb.getEvb()->runInEventBaseThreadAndWait([&]() {
    evb.addSocket(*subSocket, ZMQ_POLLIN, [&](int revents) {
      EXPECT_TRUE(revents & ZMQ_POLLIN);
      auto msg = subSocket.recvOne();
      ASSERT_TRUE(msg.hasValue());
      if (msg.hasValue()) {
        ++rcvdMsgs;
      }
      VLOG(3) << "Received " << rcvdMsgs.load();
      if (rcvdMsgs == expectedMsgs) {
        waitBaton.post();
      }
    });
  });

  // Send messages on pub socket
  for (size_t i = 0; i < expectedMsgs; ++i) {
    VLOG(3) << "Sending " << i + 1;
    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(pubSocket.sendOne(msg).hasValue());
  }

  // Wait and verify messages are received
  waitBaton.wait();
  EXPECT_EQ(rcvdMsgs, expectedMsgs);

  // Remove socket from polling
  evb.getEvb()->runInEventBaseThreadAndWait(
      [&]() { evb.removeSocket(*subSocket); });

  // Send messages again
  rcvdMsgs = 0;
  for (size_t i = 0; i < expectedMsgs; ++i) {
    EXPECT_TRUE(pubSocket.sendOne(msg).hasValue());
  }

  // Wait and verify that no new messages are received
  waitBaton.reset();
  evb.getEvb()->runInEventBaseThread([&]() {
    evb.scheduleTimeout(std::chrono::seconds(1), [&]() { waitBaton.post(); });
  });
  waitBaton.wait();
  EXPECT_EQ(0, rcvdMsgs);
}

TEST_F(OpenrEventBaseTestFixture, SocketFdPollTest) {
  folly::Baton waitBaton;

  // create signalfd and register for polling. unblock baton on successful poll
  int testFd = eventfd(0 /* initial value */, 0 /* flags */);
  evb.getEvb()->runInEventBaseThreadAndWait([&]() {
    evb.addSocketFd(testFd, ZMQ_POLLIN, [&](int revents) {
      EXPECT_TRUE(revents & ZMQ_POLLIN);
      waitBaton.post();
      uint64_t buf;
      EXPECT_EQ(
          sizeof(buf), read(testFd, static_cast<void*>(&buf), sizeof(buf)));
    });
  });

  // Perform write
  uint64_t buf{1};
  EXPECT_EQ(sizeof(buf), write(testFd, static_cast<void*>(&buf), sizeof(buf)));
  waitBaton.wait();
  EXPECT_TRUE(true);
}

int
main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  return RUN_ALL_TESTS();
}
