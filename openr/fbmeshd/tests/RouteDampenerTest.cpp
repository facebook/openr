/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <condition_variable>
#include <future>
#include <mutex>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <folly/ScopeGuard.h>
#include <openr/fbmeshd/gateway-connectivity-monitor/RouteDampener.h>

using namespace std::chrono_literals;
using namespace openr::fbmeshd;

#define Notify(_notifier)                 \
  WillOnce(testing::Invoke([&]() -> int { \
    _notifier.notify();                   \
    return 1;                             \
  }))

// This is a simple wrapper class around std primatives which allows a thread
// to notify another thread. It should be pulled out and put into a lib but
// this is fine for now.
class Notifier {
 public:
  Notifier() = default;
  virtual ~Notifier() = default;
  Notifier(const Notifier&) = delete;
  Notifier& operator=(const Notifier&) = delete;
  Notifier(Notifier&&) = delete;
  Notifier& operator=(Notifier&&) = delete;

 public:
  void
  wait() {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [this] { return ready; });
    ready = false;
  }

  void
  notify() {
    {
      std::lock_guard<std::mutex> lk(m);
      ready = true;
    }
    cv.notify_one();
  }

 private:
  std::mutex m;
  std::condition_variable cv;
  bool ready{false};
};

namespace {
static constexpr unsigned int testPenalty{1000};
static constexpr unsigned int testSuppressLimit{2000};
constexpr unsigned int testReuseLimit{750};
static constexpr std::chrono::seconds testHalfLife{1};
static constexpr std::chrono::seconds testMaxSuppressLimit{3};
} // namespace

class TestRouteDampener : public fbzmq::ZmqEventLoop, public RouteDampener {
 public:
  TestRouteDampener(unsigned int penalty = testPenalty)
      : RouteDampener{this,
                      penalty,
                      testSuppressLimit,
                      testReuseLimit,
                      testHalfLife,
                      testMaxSuppressLimit} {}
  ~TestRouteDampener() override = default;
  TestRouteDampener(const TestRouteDampener&) = delete;
  TestRouteDampener(TestRouteDampener&&) = delete;
  TestRouteDampener& operator=(const TestRouteDampener&) = delete;
  TestRouteDampener& operator=(TestRouteDampener&&) = delete;

 public:
  MOCK_METHOD0(dampen, void());
  MOCK_METHOD0(undampen, void());
  MOCK_METHOD0(userHalfLifeTimerExpired, void());
  MOCK_METHOD0(userSuppressLimitTimerExpired, void());
  MOCK_METHOD2(setStat, void(const std::string& path, int value));
};

template <typename T>
auto
runAsync(T& runable) {
  auto future = std::async(std::launch::async, [&runable]() { runable.run(); });

  runable.waitUntilRunning();

  return [&runable, future = std::move(future)]() {
    runable.stop();
    runable.waitUntilStopped();
    future.wait();
  };
}

class RouteDampenerTest : public ::testing::Test {
 protected:
  template <typename T>
  void
  test(T&& callable) {
    ::testing::StrictMock<TestRouteDampener> rd;
    auto cleanup = runAsync(rd);

    SCOPE_EXIT {
      cleanup();
    };

    rd.runInEventLoop([&callable, &rd]() { callable(rd); });
  }
};

TEST_F(RouteDampenerTest, FlapWithoutReachingSuppressionLimit) {
  test([](::testing::StrictMock<TestRouteDampener>& rd) {
    EXPECT_CALL(
        rd, setStat("route_dampener.default_route_history", testPenalty));
    rd.flap();
    EXPECT_EQ(testPenalty, rd.getHistory());
    EXPECT_FALSE(rd.isDampened());
  });
}

TEST_F(RouteDampenerTest, FlapAndReachedSuppressionLimit) {
  test([](::testing::StrictMock<TestRouteDampener>& rd) {
    EXPECT_CALL(
        rd, setStat("route_dampener.default_route_history", testPenalty));
    rd.flap();
    EXPECT_EQ(testPenalty, rd.getHistory());
    EXPECT_FALSE(rd.isDampened());
    EXPECT_CALL(rd, dampen()).Times(1);
    EXPECT_CALL(
        rd, setStat("route_dampener.default_route_history", testPenalty * 2));
    EXPECT_CALL(rd, setStat("route_dampener.default_route_dampened", 1));
    rd.flap();
  });
}

TEST_F(RouteDampenerTest, ContinueFlapping) {
  test([](::testing::StrictMock<TestRouteDampener>& rd) {
    unsigned int history{testPenalty};
    EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
    rd.flap();
    EXPECT_EQ(testPenalty, rd.getHistory());
    EXPECT_FALSE(rd.isDampened());
    EXPECT_CALL(rd, dampen()).Times(1);
    history += testPenalty;
    EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
    EXPECT_CALL(rd, setStat("route_dampener.default_route_dampened", 1));
    rd.flap();

    for (int i{0}; i < 3; ++i) {
      EXPECT_EQ(history, rd.getHistory());
      EXPECT_TRUE(rd.isDampened());

      history += testPenalty;
      EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
      rd.flap();
    }
  });
}

TEST_F(RouteDampenerTest, HalfLife) {
  unsigned int history{testPenalty};
  Notifier halfLifeNotifier;
  ::testing::StrictMock<TestRouteDampener> rd;
  auto cleanup = runAsync(rd);

  SCOPE_EXIT {
    cleanup();
  };

  rd.runInEventLoop([&rd, &halfLifeNotifier, &history]() {
    EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
    rd.flap();
    EXPECT_EQ(history, rd.getHistory());
    EXPECT_FALSE(rd.isDampened());
    EXPECT_CALL(rd, dampen()).Times(1);
    history += testPenalty;
    EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
    EXPECT_CALL(rd, setStat("route_dampener.default_route_dampened", 1));
    rd.flap();

    history /= 2;

    ::testing::Mock::VerifyAndClear(&rd);
    EXPECT_CALL(rd, userHalfLifeTimerExpired())
        .Times(1)
        .Notify(halfLifeNotifier);
    EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
  });

  halfLifeNotifier.wait();

  EXPECT_EQ(history, rd.getHistory());

  EXPECT_TRUE(
      (rd.getHistory() > testReuseLimit && rd.isDampened()) or
      !rd.isDampened());

  // next halflife goes below reuse so not longer dampened.
  history /= 2;
  EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
  EXPECT_CALL(rd, setStat("route_dampener.default_route_dampened", 0));
  EXPECT_CALL(rd, undampen()).Times(1);
  EXPECT_CALL(rd, userHalfLifeTimerExpired()).Times(1).Notify(halfLifeNotifier);
  halfLifeNotifier.wait();

  EXPECT_EQ(history, rd.getHistory());

  EXPECT_TRUE(
      (rd.getHistory() > testReuseLimit && rd.isDampened()) or
      !rd.isDampened());

  // Drop below half reuse
  history /= 2;
  EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
  history = 0;
  EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
  EXPECT_CALL(rd, userHalfLifeTimerExpired()).Times(1).Notify(halfLifeNotifier);
  halfLifeNotifier.wait();

  EXPECT_TRUE(
      (rd.getHistory() > testReuseLimit && rd.isDampened()) or
      !rd.isDampened());
  EXPECT_EQ(history, rd.getHistory());
}

TEST_F(RouteDampenerTest, MaxSuppressLimit) {
  unsigned int history{testPenalty};
  Notifier halfLifeNotifier;
  ::testing::StrictMock<TestRouteDampener> rd;
  auto cleanup = runAsync(rd);

  SCOPE_EXIT {
    cleanup();
  };

  rd.runInEventLoop([&rd, &history, &halfLifeNotifier]() {
    EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
    rd.flap();
    EXPECT_EQ(history, rd.getHistory());
    EXPECT_FALSE(rd.isDampened());
    EXPECT_CALL(rd, dampen()).Times(1);
    history += testPenalty;
    EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
    EXPECT_CALL(rd, setStat("route_dampener.default_route_dampened", 1));
    rd.flap();
    EXPECT_TRUE(rd.isDampened());
    history += testPenalty;
    EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
    rd.flap();
    halfLifeNotifier.notify();
  });

  halfLifeNotifier.wait();

  for (int i{0}; i < testMaxSuppressLimit.count() - 1; ++i) {
    history /= 2;
    EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
    EXPECT_CALL(rd, userHalfLifeTimerExpired())
        .Times(1)
        .Notify(halfLifeNotifier);
    halfLifeNotifier.wait();
    std::cerr << "waited " << i << std::endl;
    EXPECT_TRUE(rd.isDampened());
    rd.runInEventLoop([&rd, &history, &halfLifeNotifier]() {
      history += testPenalty;
      EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
      rd.flap();
      history += testPenalty;
      EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
      rd.flap();
      halfLifeNotifier.notify();
    });
    halfLifeNotifier.wait();
  }

  EXPECT_EQ(history, rd.getHistory());

  // Hit the max suppression time.
  history = 0;
  EXPECT_CALL(rd, setStat("route_dampener.default_route_dampened", 0));
  EXPECT_CALL(rd, setStat("route_dampener.default_route_history", history));
  EXPECT_CALL(rd, undampen()).Times(1);
  EXPECT_CALL(rd, userSuppressLimitTimerExpired())
      .Times(1)
      .Notify(halfLifeNotifier);
  halfLifeNotifier.wait();
  EXPECT_FALSE(rd.isDampened());
  EXPECT_EQ(0, rd.getHistory());
}

TEST_F(RouteDampenerTest, InvalidParameters) {
  EXPECT_THROW(::testing::StrictMock<TestRouteDampener> rd{10000};
               , std::range_error);
}

int
main(int argc, char** argv) {
  ::google::InitGoogleLogging(argv[0]);
  ::testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  return RUN_ALL_TESTS();
}
