/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <condition_variable>
#include <mutex>
#include <thread>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <folly/io/async/EventBase.h>
#include <openr/common/AsyncDebounce.h>

namespace openr {

TEST(AsyncDebounce, BasicOperation) {
  folly::EventBase evb;

  std::mutex m;
  std::condition_variable cv;

  std::chrono::milliseconds minBackOff{10};
  std::chrono::milliseconds maxBackOff{100};
  AsyncDebounce<std::chrono::milliseconds> debouncedFn(
      &evb, minBackOff, maxBackOff, [&m, &cv]() noexcept {
        std::unique_lock<std::mutex> lk(m);
        cv.notify_one();
      });

  auto evbThread = std::thread(&folly::EventBase::loopForever, &evb);

  for (auto debouncedCalls : {1, 3, 5, 10}) {
    std::unique_lock<std::mutex> lk(m);
    evb.runInEventBaseThread([&m, debouncedCalls, &debouncedFn]() {
      std::unique_lock<std::mutex> l(m);
      for (int i = 0; i < debouncedCalls; ++i) {
        debouncedFn();
      }
    });
    auto expectedWait =
        std::min(maxBackOff, minBackOff * (1 << (debouncedCalls - 1)));
    // don't wait for more than twice what's expected
    auto start = std::chrono::steady_clock::now();
    cv.wait_for(lk, expectedWait * 2);
    auto finish = std::chrono::steady_clock::now();

    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - start);
    EXPECT_GE(duration, expectedWait);
    EXPECT_LT(duration, expectedWait + minBackOff);
  }

  evb.terminateLoopSoon();
  evbThread.join();
}

} // namespace openr

int
main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  return RUN_ALL_TESTS();
}
