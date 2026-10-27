/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <future>

#include <fb303/ServiceData.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include <openr/common/MainUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStorePeerIngressLimit.h>
#include <openr/kvstore/KvStoreServiceHandler.h>
#include <openr/kvstore/KvStoreWrapper.h>

namespace fb303 = facebook::fb303;

using namespace openr;

namespace {

using KvStoreServiceClient =
    apache::thrift::Client<openr::thrift::KvStoreService>;

constexpr size_t kPauseAboveGlobalBytes{1000};
constexpr size_t kResumeBelowGlobalBytes{500};
constexpr std::chrono::milliseconds kLongPause{60000};
constexpr std::chrono::milliseconds kShortPause{100};

OpenrIngressKvStoreLimits
makeLimits(std::chrono::milliseconds maxPauseDuration) {
  return OpenrIngressKvStoreLimits{
      .pauseAboveGlobalBytes = kPauseAboveGlobalBytes,
      .resumeBelowGlobalBytes = kResumeBelowGlobalBytes,
      .maxPauseDuration = maxPauseDuration,
  };
}

} // namespace

TEST(
    OpenrIngressKvStoreBackpressureTest,
    PendingMergeGuardReleasesBytesWhenDestroyed) {
  const auto backpressure =
      std::make_shared<OpenrIngressKvStoreBackpressure>(makeLimits(kLongPause));

  auto guard = backpressure->trackOpenrIngressKvStorePendingMergeBytes(100);
  EXPECT_EQ(100, backpressure->getOpenrIngressKvStorePendingMergeGlobalBytes());

  guard.reset();
  EXPECT_EQ(0, backpressure->getOpenrIngressKvStorePendingMergeGlobalBytes());
}

class DrainingKvStoreService
    : public apache::thrift::ServiceHandler<thrift::KvStoreService> {
 public:
  folly::SemiFuture<folly::Unit>
  semifuture_setKvStoreKeyVals(
      std::unique_ptr<thrift::KeySetParams> /* setParams */,
      std::unique_ptr<std::string> /* area */) override {
    started.post();
    return completion.getSemiFuture();
  }

  folly::Baton<> started;
  folly::Promise<folly::Unit> completion;
};

TEST(
    OpenrIngressKvStoreBackpressureTest,
    ShutdownDrainsRequestsBeyondOneSecond) {
  const auto backpressure =
      std::make_shared<OpenrIngressKvStoreBackpressure>(makeLimits(kLongPause));
  const auto service = std::make_shared<DrainingKvStoreService>();
  auto server = std::make_unique<apache::thrift::ScopedServerInterfaceThread>(
      service, "::1", 0, [&](apache::thrift::ThriftServer& thriftServer) {
        thriftServer.setNumIOWorkerThreads(1);
        thriftServer.setWorkersJoinTimeout(std::chrono::seconds(4));
        configureKvStorePeerThriftServerMemoryLimits(
            thriftServer, backpressure);
      });
  auto client = server->newStickyClient<KvStoreServiceClient>();
  auto request = client->semifuture_setKvStoreKeyVals(
      thrift::KeySetParams{}, kTestingAreaName);
  service->started.wait();

  backpressure->stopPausing();
  server->getThriftServer().stop();
  auto shutdown = std::async(std::launch::async, [&]() { server.reset(); });
  EXPECT_EQ(
      std::future_status::timeout,
      shutdown.wait_for(std::chrono::milliseconds(1100)));
  service->completion.setValue();
  EXPECT_FALSE(
      std::move(request).getTry(std::chrono::seconds(5)).hasException());
  shutdown.get();
}

class OpenrIngressKvStoreBackpressureTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    thrift::KvStoreConfig kvStoreConfig;
    kvStoreConfig.node_name() = nodeName_;
    const folly::F14FastSet<std::string> areaIds{kTestingAreaName.t};

    kvStoreWrapper_ = std::make_unique<
        KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>>(
        areaIds, kvStoreConfig);
    kvStoreWrapper_->run();
  }

  void
  TearDown() override {
    if (openrIngressKvStoreBackpressure_) {
      openrIngressKvStoreBackpressure_->stopPausing();
    }
    client_.reset();
    server_.reset();
    handler_.reset();
    kvStoreWrapper_->stop();
  }

  void
  startServer(std::chrono::milliseconds maxPauseDuration) {
    openrIngressKvStoreBackpressure_ =
        std::make_shared<OpenrIngressKvStoreBackpressure>(
            makeLimits(maxPauseDuration));
    handler_ = std::make_shared<
        KvStoreServiceHandler<apache::thrift::Client<thrift::KvStoreService>>>(
        nodeName_,
        kvStoreWrapper_->getKvStore(),
        openrIngressKvStoreBackpressure_);
    server_ = std::make_unique<apache::thrift::ScopedServerInterfaceThread>(
        handler_, "::1", 0, [this](apache::thrift::ThriftServer& server) {
          server.setNumIOWorkerThreads(1);
          server.addRoutingHandler(
              std::make_unique<KvStorePeerRoutingHandler>(
                  server, openrIngressKvStoreBackpressure_));
        });
    // One connection, as a KvStore peer uses.
    client_ = server_->newStickyClient<KvStoreServiceClient>();
  }

  /*
   * Returns once the server's IO thread has run everything already queued,
   * including the write-quiescence callback of a response the client got.
   */
  void
  waitForServerIo() {
    server_->getThriftServer()
        .getIOThreadPool()
        ->getEventBase()
        ->runInEventBaseThreadAndWait([] {});
  }

  int64_t
  getPauseTimeouts() {
    return fb303::fbData->getCounter(
        "kvstore.peer_ingress.num_pause_timeouts.count");
  }

  folly::coro::Task<void>
  setKey(int64_t version) {
    thrift::KeySetParams params;
    params.keyVals()->emplace(
        "key", createThriftValue(version, nodeName_, std::string("value")));
    co_await client_->co_setKvStoreKeyVals(params, kTestingAreaName);
  }

 protected:
  const std::string nodeName_{"node"};
  std::unique_ptr<
      KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>>
      kvStoreWrapper_;
  std::shared_ptr<OpenrIngressKvStoreBackpressure>
      openrIngressKvStoreBackpressure_;
  std::shared_ptr<
      KvStoreServiceHandler<apache::thrift::Client<thrift::KvStoreService>>>
      handler_;
  std::unique_ptr<apache::thrift::ScopedServerInterfaceThread> server_;
  std::unique_ptr<KvStoreServiceClient> client_;
};

CO_TEST_F(
    OpenrIngressKvStoreBackpressureTestFixture,
    DoesNotPauseBelowPauseAboveGlobalBytes) {
  startServer(kLongPause);

  co_await setKey(1);
  waitForServerIo();

  EXPECT_EQ(0, openrIngressKvStoreBackpressure_->getPausedConnectionCount());
  EXPECT_EQ(
      0,
      openrIngressKvStoreBackpressure_
          ->getOpenrIngressKvStorePendingMergeGlobalBytes());
}

CO_TEST_F(
    OpenrIngressKvStoreBackpressureTestFixture,
    PausesAboveGlobalBytesAndResumesBelowGlobalBytes) {
  startServer(kLongPause);
  /*
   * Finish the first request's writes before holding pending-merge bytes, so
   * that the connection is paused by the second response, not the first.
   */
  co_await setKey(1);
  waitForServerIo();
  auto heldPendingMergeGuard = openrIngressKvStoreBackpressure_
                                   ->trackOpenrIngressKvStorePendingMergeBytes(
                                       kPauseAboveGlobalBytes + 1);

  co_await setKey(2);
  waitForServerIo();
  CO_ASSERT_EQ(1, openrIngressKvStoreBackpressure_->getPausedConnectionCount());

  heldPendingMergeGuard.reset();
  EXPECT_EQ(0, openrIngressKvStoreBackpressure_->getPausedConnectionCount());
  // Completes only if the connection is read again.
  co_await setKey(3);
}

CO_TEST_F(
    OpenrIngressKvStoreBackpressureTestFixture, ResumesAfterMaxPauseDuration) {
  startServer(kShortPause);
  co_await setKey(1);
  waitForServerIo();
  const auto pauseTimeouts = getPauseTimeouts();
  const auto heldPendingMergeGuard =
      openrIngressKvStoreBackpressure_
          ->trackOpenrIngressKvStorePendingMergeBytes(
              kPauseAboveGlobalBytes + 1);

  // The second response pauses the connection while the guard is held.
  co_await setKey(2);
  // Completes only once maxPauseDuration resumes the connection.
  co_await setKey(3);

  EXPECT_LT(pauseTimeouts, getPauseTimeouts());
}

/*
 * stopPausing() resumes a paused connection right away, and later responses
 * do not pause it again even while pending-merge bytes stay high.
 */
CO_TEST_F(
    OpenrIngressKvStoreBackpressureTestFixture,
    StopPausingResumesAndPreventsNewPauses) {
  startServer(kLongPause);
  co_await setKey(1);
  waitForServerIo();
  const auto heldPendingMergeGuard =
      openrIngressKvStoreBackpressure_
          ->trackOpenrIngressKvStorePendingMergeBytes(
              kPauseAboveGlobalBytes + 1);

  co_await setKey(2);
  waitForServerIo();
  CO_ASSERT_EQ(1, openrIngressKvStoreBackpressure_->getPausedConnectionCount());

  openrIngressKvStoreBackpressure_->stopPausing();
  EXPECT_EQ(0, openrIngressKvStoreBackpressure_->getPausedConnectionCount());

  // Completes only if the connection is read again, and is not paused after.
  co_await setKey(3);
  waitForServerIo();
  EXPECT_EQ(0, openrIngressKvStoreBackpressure_->getPausedConnectionCount());
}
