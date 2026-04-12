/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/core.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <chrono>

#include <openr/common/OpenrProfiler.h>

using namespace openr;

class OpenrProfilerTest : public ::testing::Test {
 protected:
  void
  SetUp() override {
    auto* profiler = OpenrProfiler::getInstance();
    profiler->setCurrentThread(ProfilerThread::KVSTORE);
    profiler->setEnabled(false);
    profiler->clearStats();
    profiler->setFilterRegex("");
  }
};

/*
 * Verify profiler is disabled by default and OPENR_PROFILE is a no-op
 */
TEST_F(OpenrProfilerTest, DisabledByDefault) {
  auto* profiler = OpenrProfiler::getInstance();
  EXPECT_FALSE(profiler->isEnabled());

  {
    OPENR_PROFILE("TestFunc::shouldNotRecord");
  }

  auto stats = profiler->getStats();
  EXPECT_TRUE(stats.empty());
}

/*
 * Verify singleton returns the same instance
 */
TEST_F(OpenrProfilerTest, SingletonIdentity) {
  auto* a = OpenrProfiler::getInstance();
  auto* b = OpenrProfiler::getInstance();
  EXPECT_EQ(a, b);
}

/*
 * Enable profiler, inject a known duration via recordFinish, verify stats
 */
TEST_F(OpenrProfilerTest, BasicRecording) {
  auto* profiler = OpenrProfiler::getInstance();
  profiler->setEnabled(true);
  EXPECT_TRUE(profiler->isEnabled());

  profiler->recordFinish(
      "KvStore::mergePublication", std::chrono::milliseconds(5));

  auto stats = profiler->getStats();
  ASSERT_EQ(stats.size(), 1);
  EXPECT_EQ(stats[0].name, "KvStore::mergePublication");
  EXPECT_EQ(stats[0].count, 1);
  EXPECT_EQ(stats[0].maxMs, 5);
  EXPECT_EQ(stats[0].totalMs, 5);
}

/*
 * Multiple invocations of the same function accumulate correctly
 */
TEST_F(OpenrProfilerTest, MultipleInvocations) {
  auto* profiler = OpenrProfiler::getInstance();
  profiler->setEnabled(true);

  constexpr int kIterations = 10;
  for (int i = 0; i < kIterations; ++i) {
    profiler->recordFinish(
        "Decision::rebuildRoutes", std::chrono::milliseconds(1));
  }

  auto stats = profiler->getStats();
  ASSERT_EQ(stats.size(), 1);
  EXPECT_EQ(stats[0].name, "Decision::rebuildRoutes");
  EXPECT_EQ(stats[0].count, kIterations);
  EXPECT_EQ(stats[0].totalMs, kIterations);
}

/*
 * Multiple distinct functions are tracked independently
 */
TEST_F(OpenrProfilerTest, MultipleFunctions) {
  auto* profiler = OpenrProfiler::getInstance();
  profiler->setEnabled(true);

  {
    OPENR_PROFILE("Fib::updateRoutes");
  }
  {
    OPENR_PROFILE("Fib::syncRoutes");
  }
  {
    OPENR_PROFILE("Fib::updateRoutes");
  }

  auto stats = profiler->getStats();
  EXPECT_EQ(stats.size(), 2);

  /* Find each stat by name */
  const OpenrProfiler::StatSummary* updateStat = nullptr;
  const OpenrProfiler::StatSummary* syncStat = nullptr;
  for (const auto& s : stats) {
    if (s.name == "Fib::updateRoutes") {
      updateStat = &s;
    } else if (s.name == "Fib::syncRoutes") {
      syncStat = &s;
    }
  }

  ASSERT_NE(updateStat, nullptr);
  ASSERT_NE(syncStat, nullptr);
  EXPECT_EQ(updateStat->count, 2);
  EXPECT_EQ(syncStat->count, 1);
}

/*
 * clearStats resets everything
 */
TEST_F(OpenrProfilerTest, ClearStats) {
  auto* profiler = OpenrProfiler::getInstance();
  profiler->setEnabled(true);

  {
    OPENR_PROFILE("SpfSolver::buildRouteDb");
  }

  EXPECT_EQ(profiler->getStats().size(), 1);
  profiler->clearStats();
  EXPECT_TRUE(profiler->getStats().empty());
}

/*
 * Enable/disable toggle: disabling stops recording, re-enabling resumes
 */
TEST_F(OpenrProfilerTest, EnableDisableToggle) {
  auto* profiler = OpenrProfiler::getInstance();

  profiler->setEnabled(true);
  {
    OPENR_PROFILE("LinkState::runSpf");
  }
  EXPECT_EQ(profiler->getStats().size(), 1);

  profiler->setEnabled(false);
  {
    OPENR_PROFILE("LinkState::runSpf");
  }
  /* Count should still be 1 since profiler was disabled */
  auto stats = profiler->getStats();
  ASSERT_EQ(stats.size(), 1);
  EXPECT_EQ(stats[0].count, 1);

  profiler->setEnabled(true);
  {
    OPENR_PROFILE("LinkState::runSpf");
  }
  stats = profiler->getStats();
  ASSERT_EQ(stats.size(), 1);
  EXPECT_EQ(stats[0].count, 2);
}

/*
 * Regex filter: only matching functions are recorded
 */
TEST_F(OpenrProfilerTest, FilterRegex) {
  auto* profiler = OpenrProfiler::getInstance();
  profiler->setEnabled(true);
  profiler->setFilterRegex("KvStore::.*");

  EXPECT_TRUE(profiler->hasFilter());

  {
    OPENR_PROFILE("KvStore::mergePublication");
  }
  {
    OPENR_PROFILE("Decision::rebuildRoutes");
  }
  {
    OPENR_PROFILE("KvStore::floodPublication");
  }

  auto stats = profiler->getStats();
  /* Only KvStore functions should be recorded */
  EXPECT_EQ(stats.size(), 2);
  for (const auto& s : stats) {
    EXPECT_TRUE(s.name.find("KvStore::") == 0);
  }
}

/*
 * Clearing filter allows all functions through again
 */
TEST_F(OpenrProfilerTest, ClearFilter) {
  auto* profiler = OpenrProfiler::getInstance();
  profiler->setEnabled(true);
  profiler->setFilterRegex("Fib::.*");

  {
    OPENR_PROFILE("Decision::processPublication");
  }
  EXPECT_TRUE(profiler->getStats().empty());

  profiler->setFilterRegex("");
  EXPECT_FALSE(profiler->hasFilter());

  {
    OPENR_PROFILE("Decision::processPublication");
  }
  EXPECT_EQ(profiler->getStats().size(), 1);
}

/*
 * Invalid regex is rejected gracefully (filter stays unchanged)
 */
TEST_F(OpenrProfilerTest, InvalidRegex) {
  auto* profiler = OpenrProfiler::getInstance();
  profiler->setEnabled(true);

  profiler->setFilterRegex("[invalid");
  /* Should not have set a filter */
  EXPECT_FALSE(profiler->hasFilter());

  /* All functions should still record since no filter is active */
  {
    OPENR_PROFILE("KvStore::processKeyValueRequest");
  }
  EXPECT_EQ(profiler->getStats().size(), 1);
}

/*
 * ODS export runs without crashing (smoke test)
 */
TEST_F(OpenrProfilerTest, OdsExportSmoke) {
  auto* profiler = OpenrProfiler::getInstance();
  profiler->setEnabled(true);

  {
    OPENR_PROFILE("KvStore::processPeerUpdates");
  }
  {
    OPENR_PROFILE("Fib::retryRoutes");
  }

  /* Should not throw or crash */
  EXPECT_NO_THROW(profiler->exportToOds());
}

/*
 * recordFinish with zero duration doesn't crash
 */
TEST_F(OpenrProfilerTest, ZeroDuration) {
  auto* profiler = OpenrProfiler::getInstance();
  profiler->setEnabled(true);

  profiler->recordFinish("TestFunc::zeroDuration", std::chrono::nanoseconds(0));

  auto stats = profiler->getStats();
  ASSERT_EQ(stats.size(), 1);
  EXPECT_EQ(stats[0].count, 1);
  EXPECT_EQ(stats[0].maxMs, 0);
}

/*
 * Demonstrate the profiler API end-to-end with deterministic durations:
 * 1. Enable profiler
 * 2. Set a filter to focus on specific components
 * 3. Inject known durations via recordFinish (no sleep)
 * 4. Retrieve and inspect stats (p50/p90/p99/max/total)
 * 5. Export to ODS
 * 6. Clear and disable
 */
TEST_F(OpenrProfilerTest, EndToEndDemo) {
  auto* profiler = OpenrProfiler::getInstance();

  /* Step 1: Enable */
  profiler->setEnabled(true);

  /* Step 2: Filter to only KvStore and Decision */
  profiler->setFilterRegex("(KvStore|Decision)::.*");

  /*
   * Step 3: Inject known durations via recordFinish.
   * Use matchesFilter to replicate ScopedProfile's filter behavior.
   */
  struct FuncDuration {
    const char* name;
    int ms;
  };
  for (int i = 0; i < 10; ++i) {
    std::vector<FuncDuration> calls = {
        {"KvStore::mergePublication", (i == 7) ? 15 : 2 + (i % 4)},
        {"Decision::rebuildRoutes", (i == 9) ? 25 : 5 + (i % 8)},
        {"Fib::updateRoutes", 3},
    };
    for (const auto& [name, ms] : calls) {
      if (profiler->matchesFilter(name)) {
        profiler->recordFinish(name, std::chrono::milliseconds(ms));
      }
    }
  }

  /* Step 4: Check stats and print results */
  auto stats = profiler->getStats();
  EXPECT_EQ(stats.size(), 2);

  LOG(INFO) << "";
  LOG(INFO)
      << "=== OpenR Profiler Results (lock-free per-thread histograms) ===";
  LOG(INFO) << fmt::format(
      "{:<35s} {:>6s} {:>8s} {:>8s} {:>8s} {:>8s} {:>10s}",
      "Function",
      "Count",
      "P50",
      "P90",
      "P99",
      "Max",
      "Total");
  LOG(INFO) << std::string(85, '-');

  for (const auto& s : stats) {
    EXPECT_EQ(s.count, 10);
    EXPECT_TRUE(
        s.name == "KvStore::mergePublication" ||
        s.name == "Decision::rebuildRoutes");
    /* Verify we got meaningful latency data with exact values */
    EXPECT_GE(s.p50Ms, 1);
    EXPECT_EQ(s.maxMs, s.name == "KvStore::mergePublication" ? 15 : 25);

    LOG(INFO) << fmt::format(
        "{:<35s} {:>6d} {:>6d}ms {:>6d}ms {:>6d}ms {:>6d}ms {:>8d}ms",
        s.name,
        s.count,
        s.p50Ms,
        s.p90Ms,
        s.p99Ms,
        s.maxMs,
        s.totalMs);
  }
  LOG(INFO) << std::string(85, '-');
  LOG(INFO) << "(Fib::updateRoutes filtered out - not shown)";
  LOG(INFO) << "";

  /* Step 5: Export to ODS (smoke) */
  profiler->exportToOds();

  /* Step 6: Cleanup */
  profiler->clearStats();
  profiler->setEnabled(false);
  EXPECT_TRUE(profiler->getStats().empty());
  EXPECT_FALSE(profiler->isEnabled());
}
