/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>
#include <random>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/common/StepDetector.h>

namespace {
// generate specified number of samples from a given Guassian distribution
std::vector<double>
genGaussianSamples(double mean, double stddev, size_t numOfSamples) {
  std::normal_distribution<double> distribution(mean, stddev);
  std::default_random_engine generator;
  auto roll = std::bind(distribution, generator);

  std::vector<double> res(numOfSamples, 0);
  for (size_t i = 0; i < numOfSamples; ++i) {
    res[i] = roll();
  }
  return res;
}
} // namespace

// time series consists of large jumps
TEST(StepDetectorTest, LargeStep) {
  uint32_t changeCount = 0;
  uint32_t timeStamp = 0;
  double expectedAvg = 0.0;
  // sampled mean can still be more than delta away from population mean
  // but the probability is so small we regard it would not happen in testing
  double delta = 1.0;

  auto stepCb = [&](const double& avg) {
    ++changeCount;
    LOG(INFO) << expectedAvg << " vs " << avg;
    EXPECT_GE(avg, expectedAvg - delta);
    EXPECT_LE(avg, expectedAvg + delta);
  };

  openr::StepDetector<double, std::chrono::seconds> stepDetector(
      std::chrono::seconds(1) /* sampling period */,
      10 /* small window size */,
      30 /* large window size */,
      2 /* lower threshold */,
      10 /* upper threshold */,
      5 /* absolute threshold */,
      stepCb /* callback function */);

  {
    // stable mean w/o step
    expectedAvg = 100;
    auto samples = genGaussianSamples(expectedAvg, 1, 50);
    for (auto sample : samples) {
      stepDetector.addValue(std::chrono::seconds(timeStamp++), sample);
    }
    EXPECT_EQ(0, changeCount);
  }

  {
    // mean increase
    expectedAvg += 50;
    auto samples = genGaussianSamples(expectedAvg, 1, 50);
    for (auto sample : samples) {
      stepDetector.addValue(std::chrono::seconds(timeStamp++), sample);
    }
    EXPECT_EQ(1, changeCount);
  }

  {
    // another mean increase
    expectedAvg += 50;
    auto samples = genGaussianSamples(expectedAvg, 1, 50);
    for (auto sample : samples) {
      stepDetector.addValue(std::chrono::seconds(timeStamp++), sample);
    }
    EXPECT_EQ(2, changeCount);
  }

  {
    // mean decrease
    expectedAvg -= 100;
    auto samples = genGaussianSamples(expectedAvg, 1, 50);
    for (auto sample : samples) {
      stepDetector.addValue(std::chrono::seconds(timeStamp++), sample);
    }
    EXPECT_EQ(3, changeCount);
  }
}

// time series consists of gradual small changes
TEST(StepDetectorTest, SlowBoiling) {
  uint32_t changeCount = 0;
  uint32_t timeStamp = 0;
  double expectedAvg = 0.0;
  // sampled mean can still be more than delta away from population mean
  // but the probability is so small we regard it would not happen in testing
  double delta = 1.0;

  auto stepCb = [&](const double& avg) {
    ++changeCount;
    LOG(INFO) << expectedAvg << " vs " << avg;
    EXPECT_GE(avg, expectedAvg - delta);
    EXPECT_LE(avg, expectedAvg + delta);
  };

  openr::StepDetector<double, std::chrono::seconds> stepDetector(
      std::chrono::seconds(1) /* sampling period */,
      10 /* small window size */,
      30 /* large window size */,
      2 /* lower threshold */,
      10 /* upper threshold */,
      5 /* absolute threshold */,
      stepCb /* callback function */);

  {
    // stable mean w/o step
    expectedAvg = 100;
    auto samples = genGaussianSamples(expectedAvg, 1, 50);
    for (auto sample : samples) {
      stepDetector.addValue(std::chrono::seconds(timeStamp++), sample);
    }
    EXPECT_EQ(0, changeCount);
  }

  {
    // small mean change not regarded as a step
    expectedAvg += 2;
    auto samples = genGaussianSamples(expectedAvg, 1, 50);
    for (auto sample : samples) {
      stepDetector.addValue(std::chrono::seconds(timeStamp++), sample);
    }
    EXPECT_EQ(0, changeCount);
  }

  {
    // small mean change not regarded as a step
    expectedAvg += 2;
    auto samples = genGaussianSamples(expectedAvg, 1, 50);
    for (auto sample : samples) {
      stepDetector.addValue(std::chrono::seconds(timeStamp++), sample);
    }
    EXPECT_EQ(0, changeCount);
  }

  {
    // small mean change but accumulative change of 6 exceeds threshold 5
    expectedAvg += 2;
    auto samples = genGaussianSamples(expectedAvg, 1, 50);
    for (auto sample : samples) {
      stepDetector.addValue(std::chrono::seconds(timeStamp++), sample);
    }
    EXPECT_EQ(1, changeCount);
  }
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
