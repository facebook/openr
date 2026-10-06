/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/monitor/MonitorBase.h>

#include <atomic>
#include <mutex>

#include <folly/init/Init.h>
#include <folly/synchronization/Baton.h>
#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fb303/ServiceData.h>
#include <fb303/ThreadCachedServiceData.h>

#include <openr/common/Constants.h>
#include <openr/config/Config.h>

using namespace std;
using namespace openr;
using namespace testing;

// MockClass for mocking the processEventLog() function
class MonitorMock : public MonitorBase {
 public:
  MonitorMock(
      std::shared_ptr<const Config> config,
      const std::string& category,
      messaging::RQueue<LogSample> eventLogUpdatesQueue)
      : MonitorBase(config, category, eventLogUpdatesQueue) {}
  MOCK_METHOD1(processEventLog, void(LogSample const& eventLog));
  MOCK_METHOD0(dumpHeapProfile, void());
  MOCK_METHOD0(onCpuSampleRecorded, void());
};

class MonitorTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // generate a config for testing
    openr::thrift::OpenrConfig config;
    *config.node_name() = "node1";

    monitor = make_unique<MonitorMock>(
        std::make_unique<openr::Config>(config),
        category,
        eventLogUpdatesQueue.getReader());
    EXPECT_CALL(*monitor, onCpuSampleRecorded())
        .Times(AnyNumber())
        .WillRepeatedly([this]() {
          std::call_once(
              cpuSampleRecordedOnce, [this]() { cpuSampleRecorded.post(); });
          if (cpuSampleCount.fetch_add(1) == 1) {
            secondCpuSampleRecorded.post();
          }
        });
    monitorThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "monitor thread starting";
      monitor->run();
      LOG(INFO) << "monitor thread finishing";
    });
    monitor->waitUntilRunning();
  }

  void
  TearDown() override {
    eventLogUpdatesQueue.close();
    LOG(INFO) << "Stopping the monitor thread";
    monitor->stop();
    monitorThread->join();
    LOG(INFO) << "Monitor thread got stopped";
  }
  // monitor owned by the unit tests
  std::unique_ptr<MonitorMock> monitor{nullptr};

  // Thread in which monitor will be running.
  std::unique_ptr<std::thread> monitorThread{nullptr};

  // Queue for adding scirbe updates
  messaging::ReplicateQueue<LogSample> eventLogUpdatesQueue;

  // category for testing
  std::string category = "openr_scribe_mock_test";

  folly::Baton<> cpuSampleRecorded;
  std::once_flag cpuSampleRecordedOnce;
  std::atomic<uint32_t> cpuSampleCount{0};
  folly::Baton<> secondCpuSampleRecorded;
};

// Matcher macro for comparing LogSample in UT LogBasicOperation
MATCHER_P(LogSampleEq, log, "") {
  return arg.getString("event") == log.getString("event") &&
      arg.getInt("num") == log.getInt("num");
};

TEST_F(MonitorTestFixture, LogBasicOperation) {
  // Define an invalid log without event-type
  LogSample log1;
  log1.addInt("num", 100);
  // Define a valid log
  LogSample log2;
  log2.addString("event", "event_unit_test");
  log2.addInt("num", 200);

  // Expecting not to process the invalid log
  EXPECT_CALL(*monitor, processEventLog(LogSampleEq(log1))).Times(0);
  // Expecting to process the valid log once
  EXPECT_CALL(*monitor, processEventLog(LogSampleEq(log2))).Times(1);

  // Publish two logs
  eventLogUpdatesQueue.push(log1);
  eventLogUpdatesQueue.push(log2);

  // Wait for the fiber to process to get one log from list
  while (true) {
    if (monitor->getRecentEventLogs().size() == 1) {
      // Should only get the valid log from queue, discard the invalid one
      auto sample = LogSample::fromJson(monitor->getRecentEventLogs().front());
      EXPECT_EQ(sample.getString("event"), "event_unit_test");
      EXPECT_EQ(sample.getInt("num"), 200);
      // `node_name` should be added to each log message
      EXPECT_FALSE(sample.getString("node_name").empty());
      break;
    }
    std::this_thread::yield();
  }
}

TEST_F(MonitorTestFixture, ProcessCounterTest) {
  ASSERT_TRUE(cpuSampleRecorded.try_wait_for(std::chrono::seconds(60)))
      << "No successful CPU sample was recorded within 60 seconds";

  /*
   * Publish per-thread caches, then force-merge buffered samples into
   * the digest before checking the rolling-window tail.
   */
  facebook::fb303::ThreadCachedServiceData::get()->publishStats();
  facebook::fb303::fbData->flushAllData();
  auto counters = facebook::fb303::fbData->getCounters();
  EXPECT_TRUE(counters.contains("process.cpu.pct"));
  EXPECT_TRUE(counters.contains("process.cpu.pct.avg.60"));
  EXPECT_GT(counters["process.memory.rss"], 0);
  EXPECT_TRUE(counters.count("process.cpu.peak_pct.count.60"));
  EXPECT_GT(counters["process.cpu.peak_pct.count.60"], 0);
  EXPECT_TRUE(counters.count("process.cpu.peak_pct.p99.60"));
  EXPECT_TRUE(counters.count("process.cpu.peak_pct.p100.60"));
  // Lifetime peak-hold gauge is gone; the rolling window max replaces it.
  EXPECT_FALSE(counters.contains("process.cpu.peak_pct"));
}

TEST_F(MonitorTestFixture, RepeatedCpuSamplesDoNotPostBatonTwice) {
  ASSERT_TRUE(secondCpuSampleRecorded.try_wait_for(std::chrono::seconds(60)));
  EXPECT_GE(cpuSampleCount.load(), 2);
}

int
main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  folly::Init init(&argc, &argv);
  google::InstallFailureSignalHandler();

  return RUN_ALL_TESTS();
}
