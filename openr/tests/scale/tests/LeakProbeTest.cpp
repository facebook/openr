/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/tests/scale/LeakProbe.h>

namespace openr {

namespace {

thrift::KeyVals
oneKey(const std::string& key) {
  thrift::KeyVals kv;
  thrift::Value v;
  v.version() = 1;
  v.originatorId() = "seed";
  kv.emplace(key, std::move(v));
  return kv;
}

CounterSample
sampleAt(int32_t round, const std::map<std::string, int64_t>& counters) {
  CounterSample s;
  s.round = round;
  s.counters = counters;
  return s;
}

} // namespace

TEST(
    LeakProbeTest,
    BuildTransientAreaChurnProducesDistinctAreasCarryingKeyVals) {
  auto kv = oneKey("adj:seed");
  auto plan = buildTransientAreaChurn("transient", 4, kv);

  ASSERT_EQ(4u, plan.size());
  std::vector<std::string> areas;
  for (const auto& round : plan) {
    areas.push_back(round.area);
    /* Every round carries the supplied key-vals for a valid publication. */
    EXPECT_EQ(1u, round.keyVals.size());
    EXPECT_EQ(1u, round.keyVals.count("adj:seed"));
  }
  EXPECT_THAT(
      areas,
      ::testing::ElementsAre(
          "transient-0", "transient-1", "transient-2", "transient-3"));
}

TEST(LeakProbeTest, BuildTransientAreaChurnNonPositiveRoundsIsEmpty) {
  EXPECT_TRUE(buildTransientAreaChurn("t", 0, oneKey("k")).empty());
  EXPECT_TRUE(buildTransientAreaChurn("t", -3, oneKey("k")).empty());
}

TEST(LeakProbeTest, LeakSignatureCountersAreTheVerifiedNames) {
  EXPECT_THAT(
      leakSignatureCounters(),
      ::testing::UnorderedElementsAre(
          "process.memory.rss", "decision.num_complete_adjacencies"));
  /* Regex escapes the dots so they match literally. */
  EXPECT_EQ(
      "process\\.memory\\.rss|decision\\.num_complete_adjacencies",
      leakSignatureRegex());
}

TEST(LeakProbeTest, AnalyzeFlagsMonotonicGrowthAsLeak) {
  // rss and num_complete_adjacencies climb every round; num_nodes stays flat.
  std::vector<CounterSample> samples{
      sampleAt(
          0,
          {{"process.memory.rss", 100},
           {"decision.num_complete_adjacencies", 10},
           {"decision.num_nodes", 5}}),
      sampleAt(
          1,
          {{"process.memory.rss", 150},
           {"decision.num_complete_adjacencies", 20},
           {"decision.num_nodes", 5}}),
      sampleAt(
          2,
          {{"process.memory.rss", 210},
           {"decision.num_complete_adjacencies", 30},
           {"decision.num_nodes", 5}}),
  };

  auto report = analyzeCounterGrowth(
      samples,
      {"process.memory.rss",
       "decision.num_complete_adjacencies",
       "decision.num_nodes"});

  EXPECT_THAT(
      report.leaking,
      ::testing::UnorderedElementsAre(
          "process.memory.rss", "decision.num_complete_adjacencies"));

  const auto& rss = report.perCounter.at("process.memory.rss");
  EXPECT_EQ(100, rss.first);
  EXPECT_EQ(210, rss.last);
  EXPECT_EQ(110, rss.delta);
  EXPECT_TRUE(rss.monotonicNonDecreasing);

  /* Flat counter: present, zero delta, not leaking. */
  const auto& nodes = report.perCounter.at("decision.num_nodes");
  EXPECT_TRUE(nodes.present);
  EXPECT_EQ(0, nodes.delta);
}

TEST(LeakProbeTest, AnalyzeDoesNotFlagOscillatingCounter) {
  // Grows then drops: a reclaimed resource, not a leak.
  std::vector<CounterSample> samples{
      sampleAt(0, {{"x", 10}}),
      sampleAt(1, {{"x", 30}}),
      sampleAt(2, {{"x", 15}}),
  };
  auto report = analyzeCounterGrowth(samples, {"x"});
  EXPECT_TRUE(report.leaking.empty());
  const auto& g = report.perCounter.at("x");
  EXPECT_FALSE(g.monotonicNonDecreasing);
  EXPECT_EQ(5, g.delta); // net positive, but it dropped, so not flagged
}

TEST(LeakProbeTest, AnalyzeAbsentAndSingleSampleCountersNotFlagged) {
  std::vector<CounterSample> samples{
      sampleAt(0, {{"present_once", 7}}),
  };
  auto report = analyzeCounterGrowth(samples, {"present_once", "never_seen"});

  EXPECT_TRUE(report.leaking.empty());
  EXPECT_TRUE(report.perCounter.at("present_once").present);
  EXPECT_EQ(0, report.perCounter.at("present_once").delta);
  EXPECT_FALSE(report.perCounter.at("never_seen").present);
}

} // namespace openr
