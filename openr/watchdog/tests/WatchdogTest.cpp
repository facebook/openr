/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdlib>
#include <iostream>

#include <fb303/ServiceData.h>
#include <gtest/gtest.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/messaging/ReplicateQueue.h>

#include <folly/init/Init.h>
#include <folly/synchronization/Baton.h>
#include <openr/tests/utils/Utils.h>
#include <openr/watchdog/Watchdog.h>

namespace fb303 = facebook::fb303;

using namespace openr;

namespace {
const std::chrono::seconds kWatchdogInterval{2};
} // namespace

class WatchdogTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // create config
    thrift::WatchdogConfig watchdogConf;
    watchdogConf.interval_s() = kWatchdogInterval.count();

    auto tConfig = getBasicOpenrConfig(nodeId_);
    tConfig.watchdog_config() = watchdogConf;
    tConfig.enable_watchdog() = true;
    config_ = std::make_shared<Config>(tConfig);

    // spawn watchdog thread
    watchdog_ = std::make_unique<Watchdog>(config_);

    watchdogThread_ = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Starting watchdog thread...";
      watchdog_->run();
      LOG(INFO) << "watchdog thread got stopped.";
    });
    watchdog_->waitUntilRunning();
  }

  void
  setupDummyEvb() {
    dummyEvb_ = std::make_unique<OpenrEventBase>();
    dummyEvb_->setEvbName("dummyEvb");
    dummyThread_ = std::make_unique<std::thread>([&]() {
      LOG(INFO) << "Starting dummyEvb thread...";
      dummyEvb_->run();
      LOG(INFO) << "dummyEvb thread got stopped";
    });
    dummyEvb_->waitUntilRunning();
    watchdog_->addEvb(dummyEvb_.get());
  }

  void
  teardownDummyEvb() {
    dummyEvb_->stop();
    dummyEvb_->waitUntilStopped();
    dummyThread_->join();
  }

  void
  TearDown() override {
    watchdog_->stop();
    watchdogThread_->join();
    watchdog_.reset();
  }

 protected:
  // nodeId
  const std::string nodeId_{"ted"};

  // config
  std::shared_ptr<Config> config_;

  // watchdog
  std::unique_ptr<Watchdog> watchdog_;
  std::unique_ptr<std::thread> watchdogThread_;

  // dummyEvb
  std::unique_ptr<OpenrEventBase> dummyEvb_;
  std::unique_ptr<std::thread> dummyThread_;
};

TEST_F(WatchdogTestFixture, CounterReport) {
  // clean up counters before testing
  fb303::fbData->resetAllData();

  setupDummyEvb();
  OpenrEventBase evb;
  int scheduleAt{0};

  evb.scheduleTimeout(
      std::chrono::seconds(scheduleAt += (1 + kWatchdogInterval.count())),
      [&]() {
        auto counters = fb303::fbData->getCounters();

        // Verify the counter keys exist
        ASSERT_TRUE(counters.count(
            fmt::format(
                "watchdog.evb_queue_size.{}", dummyEvb_->getEvbName())));
        ASSERT_TRUE(counters.count(
            fmt::format(
                "watchdog.thread_mem_usage_kb.{}", dummyEvb_->getEvbName())));

        evb.stop();
      });

  // let magic happen
  evb.run();
  teardownDummyEvb();
}

TEST_F(WatchdogTestFixture, QueueCounterReport) {
  // cleanup the counters before testing
  fb303::fbData->resetAllData();

  setupDummyEvb();

  OpenrEventBase evb;

  // Create couple of ReplicateQueues
  messaging::ReplicateQueue<int> q1;
  messaging::ReplicateQueue<std::string> q2;

  // Add 2 items to q1 and 1 item to q2
  q1.push(1);
  q1.push(2);
  q2.push("one");

  // Create two readers for each queue
  auto q1r1 = q1.getReader();
  auto q1r2 = q1.getReader();
  auto q2r1 = q2.getReader();
  auto q2r2 = q2.getReader();

  // Register the queues with watchdog
  watchdog_->addQueue(q1, "Queue1");
  watchdog_->addQueue(q2, "Queue2");

  int scheduleAt{0};

  evb.scheduleTimeout(
      std::chrono::seconds(scheduleAt += (1 + kWatchdogInterval.count())),
      [&]() {
        ASSERT_EQ(
            fb303::fbData->getCounter(
                fmt::format("messaging.replicate_queue.{}.readers", "Queue1")),
            q1.getNumReaders());
        ASSERT_EQ(
            fb303::fbData->getCounter(
                fmt::format("messaging.replicate_queue.{}.readers", "Queue2")),
            q2.getNumReaders());

        ASSERT_EQ(
            fb303::fbData->getCounter(
                fmt::format(
                    "messaging.replicate_queue.{}.messages_sent", "Queue1")),
            q1.getNumWrites());
        ASSERT_EQ(
            fb303::fbData->getCounter(
                fmt::format(
                    "messaging.replicate_queue.{}.messages_sent", "Queue2")),
            q2.getNumWrites());

        // Check the internal replicated queues
        auto stats = q1.getReplicationStats();
        for (auto& stat : stats) {
          ASSERT_EQ(
              fb303::fbData->getCounter(
                  fmt::format(
                      "messaging.rw_queue.{}-{}.size", "Queue1", stat.queueId)),
              stat.size);
          ASSERT_EQ(
              fb303::fbData->getCounter(
                  fmt::format(
                      "messaging.rw_queue.{}-{}.read", "Queue1", stat.queueId)),
              stat.reads);
          ASSERT_EQ(
              fb303::fbData->getCounter(
                  fmt::format(
                      "messaging.rw_queue.{}-{}.sent", "Queue1", stat.queueId)),
              stat.writes);
        }
        stats = q2.getReplicationStats();
        for (auto& stat : stats) {
          ASSERT_EQ(
              fb303::fbData->getCounter(
                  fmt::format(
                      "messaging.rw_queue.{}-{}.size", "Queue2", stat.queueId)),
              stat.size);
          ASSERT_EQ(
              fb303::fbData->getCounter(
                  fmt::format(
                      "messaging.rw_queue.{}-{}.read", "Queue2", stat.queueId)),
              stat.reads);
          ASSERT_EQ(
              fb303::fbData->getCounter(
                  fmt::format(
                      "messaging.rw_queue.{}-{}.sent", "Queue2", stat.queueId)),
              stat.writes);
        }

        evb.stop();
      });

  evb.run();
  teardownDummyEvb();
}

/*
 * Death test: when the watchdog fires a crash it must invoke the registered
 * pre-crash callback (used to announce graceful restart to peers) *before* it
 * aborts -- and abort() must still run so the core dump is produced. We drive
 * the dead-thread crash path with a registered evb that is never run (so its
 * heartbeat timestamp never advances) and assert the callback's sentinel
 * reaches stderr before the process dies via SIGABRT.
 *
 * Standalone TEST (not the fixture) so the process is single-threaded when
 * EXPECT_DEATH fork()s; the watchdog thread is created inside the child.
 */
TEST(WatchdogDeathTest, PreCrashCallbackRunsBeforeAbort) {
  EXPECT_DEATH(
      {
        thrift::WatchdogConfig watchdogConf;
        watchdogConf.interval_s() = 1;
        watchdogConf.thread_timeout_s() = 1;

        auto tConfig = getBasicOpenrConfig("ted");
        tConfig.watchdog_config() = watchdogConf;
        tConfig.enable_watchdog() = true;
        auto config = std::make_shared<Config>(tConfig);

        auto watchdog = std::make_unique<Watchdog>(config);
        // Posted from the pre-crash hook so the waiter below blocks only as
        // long as the watchdog needs to fire -- no fixed sleep.
        folly::Baton<> preCrashHookRan;
        watchdog->setPreCrashCallback([&]() {
          std::cerr << "PRECRASH_HOOK_RAN" << std::endl;
          preCrashHookRan.post();
        });

        std::thread watchdogThread([&]() { watchdog->run(); });
        watchdog->waitUntilRunning();

        // A "stuck" evb: constructed but never run, so its heartbeat timestamp
        // never advances and the watchdog declares it a dead thread, which
        // triggers fireCrash() after two monitor cycles.
        OpenrEventBase stuckEvb;
        stuckEvb.setEvbName("stuckEvb");
        watchdog->addEvb(&stuckEvb);

        // Wait only as long as the watchdog needs to detect the dead thread
        // and run the pre-crash hook (signalled via the baton). If it never
        // fires within the bound, exit cleanly with a clear message so
        // EXPECT_DEATH fails fast (no SIGABRT) instead of hanging on join().
        if (!preCrashHookRan.try_wait_for(std::chrono::seconds(10))) {
          std::cerr << "watchdog did not fire pre-crash hook within 10s"
                    << std::endl;
          std::_Exit(0);
        }
        // Hook ran; abort() is imminent and will terminate the process.
        watchdogThread.join(); // unreached: watchdog aborts first
      },
      "PRECRASH_HOOK_RAN");
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  const folly::Init init(&argc, &argv);

  // Run the tests
  return RUN_ALL_TESTS();
}
