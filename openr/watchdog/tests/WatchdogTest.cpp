/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <fb303/ServiceData.h>
#include <folly/init/Init.h>
#include <openr/config/tests/Utils.h>
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
    watchdogConf.set_interval_s(kWatchdogInterval.count());

    auto tConfig = getBasicOpenrConfig(nodeId_);
    tConfig.set_watchdog_config(watchdogConf);
    tConfig.set_enable_watchdog(true);
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
};

TEST_F(WatchdogTestFixture, CounterReport) {
  // clean up counters before testing
  fb303::fbData->resetAllData();

  // create dummy eventbase to let watchdog monitor
  auto dummyEvb = std::make_unique<OpenrEventBase>();
  dummyEvb->setEvbName("dummyEvb");

  auto dummyThread = std::make_unique<std::thread>([&]() {
    LOG(INFO) << "Starting dummyEvb thread...";
    dummyEvb->run();
    LOG(INFO) << "dummyEvb thread got stopped.";
  });
  dummyEvb->waitUntilRunning();

  // add dummy thread to watchdog
  watchdog_->addEvb(dummyEvb.get());

  OpenrEventBase evb;
  int scheduleAt{0};

  evb.scheduleTimeout(
      std::chrono::seconds(scheduleAt += (1 + kWatchdogInterval.count())),
      [&]() {
        auto counters = fb303::fbData->getCounters();

        // Verify the counter keys exist
        ASSERT_TRUE(counters.count(
            fmt::format("watchdog.evb_queue_size.{}", dummyEvb->getEvbName())));
        ASSERT_TRUE(counters.count(fmt::format(
            "watchdog.thread_mem_usage_kb.{}", dummyEvb->getEvbName())));

        evb.stop();
      });

  // let magic happen
  evb.run();

  dummyEvb->stop();
  dummyEvb->waitUntilStopped();
  dummyThread->join();
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);

  // Run the tests
  return RUN_ALL_TESTS();
}
