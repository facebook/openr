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
#include <openr/if/gen-cpp2/KvStoreService.h>
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
const std::string kPeer{"peer1"};
constexpr size_t kQueuedPeerLimitBytes{1 << 30};

OpenrIngressKvStoreLimits
makeLimits(
    std::chrono::milliseconds maxPauseDuration,
    size_t queuedPeerLimitBytes = kQueuedPeerLimitBytes) {
  return OpenrIngressKvStoreLimits{
      .pauseAboveGlobalBytes = kPauseAboveGlobalBytes,
      .resumeBelowGlobalBytes = kResumeBelowGlobalBytes,
      .maxPauseDuration = maxPauseDuration,
      .queuedPeerLimitBytes = queuedPeerLimitBytes,
  };
}

} // namespace

TEST(
    OpenrIngressKvStoreBackpressureTest,
    PendingMergeGuardReleasesBytesWhenDestroyed) {
  const auto backpressure =
      std::make_shared<OpenrIngressKvStoreBackpressure>(makeLimits(kLongPause));

  auto guard =
      backpressure->trackOpenrIngressKvStorePendingMergeBytes(kPeer, 100);
  EXPECT_EQ(100, backpressure->getOpenrIngressKvStorePendingMergeGlobalBytes());
  EXPECT_EQ(
      100, backpressure->getOpenrIngressKvStorePendingMergePeerBytes(kPeer));

  guard.reset();
  EXPECT_EQ(0, backpressure->getOpenrIngressKvStorePendingMergeGlobalBytes());
  EXPECT_EQ(
      0, backpressure->getOpenrIngressKvStorePendingMergePeerBytes(kPeer));
}

TEST(OpenrIngressKvStoreBackpressureTest, CountsQueuedBytesUntilReleased) {
  const auto backpressure =
      std::make_shared<OpenrIngressKvStoreBackpressure>(makeLimits(kLongPause));
  apache::thrift::Cpp2ConnContext connection;
  OpenrIngressKvStoreBackpressure::initConnection(connection);

  EXPECT_TRUE(
      backpressure->admitOpenrIngressKvStoreQueuedRequest(connection, 100));
  EXPECT_EQ(100, backpressure->getOpenrIngressKvStoreQueuedGlobalBytes());

  backpressure->releaseOpenrIngressKvStoreQueuedRequest(connection, 100);
  EXPECT_EQ(0, backpressure->getOpenrIngressKvStoreQueuedGlobalBytes());
}

TEST(
    OpenrIngressKvStoreBackpressureTest,
    RejectsOnlyPeerOverQueuedPeerLimitBytes) {
  const auto backpressure = std::make_shared<OpenrIngressKvStoreBackpressure>(
      makeLimits(kLongPause, /*queuedPeerLimitBytes=*/100));
  apache::thrift::Cpp2ConnContext heavyPeer;
  apache::thrift::Cpp2ConnContext lightPeer;
  OpenrIngressKvStoreBackpressure::initConnection(heavyPeer);
  OpenrIngressKvStoreBackpressure::initConnection(lightPeer);

  // The second request takes the heavy peer over the limit and is admitted.
  EXPECT_TRUE(
      backpressure->admitOpenrIngressKvStoreQueuedRequest(heavyPeer, 60));
  EXPECT_TRUE(
      backpressure->admitOpenrIngressKvStoreQueuedRequest(heavyPeer, 60));
  EXPECT_FALSE(
      backpressure->admitOpenrIngressKvStoreQueuedRequest(heavyPeer, 1));
  EXPECT_TRUE(
      backpressure->admitOpenrIngressKvStoreQueuedRequest(lightPeer, 60));
  EXPECT_EQ(180, backpressure->getOpenrIngressKvStoreQueuedGlobalBytes());
}

TEST(
    OpenrIngressKvStoreBackpressureTest,
    AdmitsRequestLargerThanLimitWhenNothingQueued) {
  const auto backpressure = std::make_shared<OpenrIngressKvStoreBackpressure>(
      makeLimits(kLongPause, /*queuedPeerLimitBytes=*/100));
  apache::thrift::Cpp2ConnContext connection;
  OpenrIngressKvStoreBackpressure::initConnection(connection);

  EXPECT_TRUE(
      backpressure->admitOpenrIngressKvStoreQueuedRequest(connection, 500));
  EXPECT_FALSE(
      backpressure->admitOpenrIngressKvStoreQueuedRequest(connection, 1));

  backpressure->releaseOpenrIngressKvStoreQueuedRequest(connection, 500);
  EXPECT_TRUE(
      backpressure->admitOpenrIngressKvStoreQueuedRequest(connection, 500));
}

/*
 * The held-bytes stat reports the peak of pending-merge plus queued bytes,
 * even after both have been released. The bytes are larger than any other
 * test uses, since the stat is shared by every backpressure in the process.
 */
TEST(OpenrIngressKvStoreBackpressureTest, RecordsPeakHeldGlobalBytes) {
  const auto backpressure =
      std::make_shared<OpenrIngressKvStoreBackpressure>(makeLimits(kLongPause));
  apache::thrift::Cpp2ConnContext connection;
  OpenrIngressKvStoreBackpressure::initConnection(connection);

  EXPECT_TRUE(backpressure->admitOpenrIngressKvStoreQueuedRequest(
      connection, 1'000'000));
  auto guard =
      backpressure->trackOpenrIngressKvStorePendingMergeBytes(kPeer, 2'000'000);
  backpressure->releaseOpenrIngressKvStoreQueuedRequest(connection, 1'000'000);
  guard.reset();

  EXPECT_EQ(0, backpressure->getOpenrIngressKvStoreQueuedGlobalBytes());
  EXPECT_EQ(0, backpressure->getOpenrIngressKvStorePendingMergeGlobalBytes());
  fb303::fbData->flushAllData();
  EXPECT_EQ(
      3'000'000,
      fb303::fbData->getCounter(
          "kvstore.peer_ingress.openr_held_global_bytes.p100.60"));
}

TEST(
    OpenrIngressKvStoreBackpressureTest,
    ReleaseWithoutMatchingAdmitDoesNotWrapTotals) {
  const auto backpressure =
      std::make_shared<OpenrIngressKvStoreBackpressure>(makeLimits(kLongPause));
  apache::thrift::Cpp2ConnContext connection;
  OpenrIngressKvStoreBackpressure::initConnection(connection);
  EXPECT_TRUE(
      backpressure->admitOpenrIngressKvStoreQueuedRequest(connection, 100));

  backpressure->releaseOpenrIngressKvStoreQueuedRequest(connection, 300);

  EXPECT_EQ(0, backpressure->getOpenrIngressKvStoreQueuedGlobalBytes());
  EXPECT_EQ(0, backpressure->getNumPeersHoldingBytes());
}

TEST(OpenrIngressKvStoreBackpressureTest, MovesQueuedBytesToIdentifiedPeer) {
  const auto backpressure =
      std::make_shared<OpenrIngressKvStoreBackpressure>(makeLimits(kLongPause));
  apache::thrift::Cpp2ConnContext first;
  apache::thrift::Cpp2ConnContext second;
  OpenrIngressKvStoreBackpressure::initConnection(first);
  OpenrIngressKvStoreBackpressure::initConnection(second);
  EXPECT_TRUE(backpressure->admitOpenrIngressKvStoreQueuedRequest(first, 100));
  EXPECT_TRUE(backpressure->admitOpenrIngressKvStoreQueuedRequest(second, 100));
  EXPECT_EQ(2, backpressure->getNumPeersHoldingBytes());

  // Both connections turn out to belong to the same peer.
  backpressure->recordSenderId(first, kPeer);
  backpressure->recordSenderId(second, kPeer);
  EXPECT_EQ(1, backpressure->getNumPeersHoldingBytes());

  backpressure->releaseOpenrIngressKvStoreQueuedRequest(first, 100);
  backpressure->releaseOpenrIngressKvStoreQueuedRequest(second, 100);
  EXPECT_EQ(0, backpressure->getNumPeersHoldingBytes());
}

TEST(OpenrIngressKvStoreBackpressureTest, InitConnectionTwiceKeepsQueuedBytes) {
  const auto backpressure =
      std::make_shared<OpenrIngressKvStoreBackpressure>(makeLimits(kLongPause));
  apache::thrift::Cpp2ConnContext connection;
  OpenrIngressKvStoreBackpressure::initConnection(connection);
  EXPECT_TRUE(
      backpressure->admitOpenrIngressKvStoreQueuedRequest(connection, 100));

  OpenrIngressKvStoreBackpressure::initConnection(connection);
  backpressure->releaseOpenrIngressKvStoreQueuedRequest(connection, 100);

  EXPECT_EQ(0, backpressure->getOpenrIngressKvStoreQueuedGlobalBytes());
  EXPECT_EQ(0, backpressure->getNumPeersHoldingBytes());
}

/*
 * A connection whose senderId changes moves its queued bytes to the new peer,
 * and the old peer, holding nothing else, stops counting.
 */
TEST(OpenrIngressKvStoreBackpressureTest, MovesQueuedBytesWhenSenderIdChanges) {
  const auto backpressure =
      std::make_shared<OpenrIngressKvStoreBackpressure>(makeLimits(kLongPause));
  apache::thrift::Cpp2ConnContext connection;
  OpenrIngressKvStoreBackpressure::initConnection(connection);
  backpressure->recordSenderId(connection, "old-peer");
  EXPECT_TRUE(
      backpressure->admitOpenrIngressKvStoreQueuedRequest(connection, 100));
  const auto newPeerGuard =
      backpressure->trackOpenrIngressKvStorePendingMergeBytes("new-peer", 50);
  EXPECT_EQ(2, backpressure->getNumPeersHoldingBytes());

  backpressure->recordSenderId(connection, "new-peer");
  EXPECT_EQ(1, backpressure->getNumPeersHoldingBytes());

  backpressure->releaseOpenrIngressKvStoreQueuedRequest(connection, 100);
  EXPECT_EQ(1, backpressure->getNumPeersHoldingBytes());
  EXPECT_EQ(0, backpressure->getOpenrIngressKvStoreQueuedGlobalBytes());
}

/*
 * Holds the only CPU worker on its first setKvStoreKeyVals until released, so
 * that later requests wait in the request pile.
 */
class BlockingKvStoreService
    : public apache::thrift::ServiceHandler<thrift::KvStoreService> {
 public:
  void
  setKvStoreKeyVals(
      std::unique_ptr<thrift::KeySetParams> /* setParams */,
      std::unique_ptr<std::string> /* area */) override {
    if (numCalls_++ == 0) {
      started.post();
    }
    released_.wait();
  }

  void
  release() {
    release_.set_value();
  }

  folly::Baton<> started;

 private:
  std::atomic<int> numCalls_{0};
  std::promise<void> release_;
  // Every call waits on it, which a Baton does not allow.
  std::shared_future<void> released_{release_.get_future().share()};
};

TEST(
    OpenrIngressKvStoreBackpressureTest,
    RejectsPeerRequestsOnceQueuedPeerLimitBytesReached) {
  const auto backpressure = std::make_shared<OpenrIngressKvStoreBackpressure>(
      makeLimits(kLongPause, /*queuedPeerLimitBytes=*/1));
  const auto service = std::make_shared<BlockingKvStoreService>();
  apache::thrift::ScopedServerInterfaceThread server(
      service, "::1", 0, [&](apache::thrift::ThriftServer& thriftServer) {
        thriftServer.setNumIOWorkerThreads(1);
        thriftServer.setNumCPUWorkerThreads(1);
        backpressure->installRequestPile(thriftServer);
        thriftServer.addRoutingHandler(
            std::make_unique<KvStorePeerRoutingHandler>(
                thriftServer, backpressure));
      });
  auto client = server.newStickyClient<KvStoreServiceClient>();
  thrift::KeySetParams params;
  params.keyVals()->emplace(
      "key", createThriftValue(1, "node", std::string("value")));

  auto first = client->semifuture_setKvStoreKeyVals(params, kTestingAreaName);
  service->started.wait();
  // Waits behind the first and takes the peer over the 1-byte limit.
  auto second = client->semifuture_setKvStoreKeyVals(params, kTestingAreaName);
  // The peer has reached the limit.
  auto third = client->semifuture_setKvStoreKeyVals(params, kTestingAreaName);
  auto thirdResult = std::move(third).getTry(std::chrono::seconds(5));
  service->release();

  EXPECT_FALSE(std::move(first).getTry(std::chrono::seconds(5)).hasException());
  EXPECT_FALSE(
      std::move(second).getTry(std::chrono::seconds(5)).hasException());
  ASSERT_TRUE(thirdResult.hasException());
  const auto* error =
      thirdResult.exception()
          .get_exception<apache::thrift::TApplicationException>();
  ASSERT_NE(nullptr, error);
  EXPECT_EQ(
      apache::thrift::TApplicationException::LOADSHEDDING, error->getType());
  EXPECT_EQ(0, backpressure->getOpenrIngressKvStoreQueuedGlobalBytes());
  EXPECT_EQ(0, backpressure->getNumPeersHoldingBytes());
}

/*
 * A peer that disconnects while its request waits for the CPU worker still has
 * that request's bytes released: the connection stays alive until the request
 * leaves the pile.
 */
TEST(
    OpenrIngressKvStoreBackpressureTest,
    ReleasesQueuedBytesWhenPeerDisconnectsWhileQueued) {
  const auto backpressure = std::make_shared<OpenrIngressKvStoreBackpressure>(
      makeLimits(kLongPause, /*queuedPeerLimitBytes=*/1));
  const auto service = std::make_shared<BlockingKvStoreService>();
  auto server = std::make_unique<apache::thrift::ScopedServerInterfaceThread>(
      service, "::1", 0, [&](apache::thrift::ThriftServer& thriftServer) {
        thriftServer.setNumIOWorkerThreads(1);
        thriftServer.setNumCPUWorkerThreads(1);
        backpressure->installRequestPile(thriftServer);
        thriftServer.addRoutingHandler(
            std::make_unique<KvStorePeerRoutingHandler>(
                thriftServer, backpressure));
      });
  auto client = server->newStickyClient<KvStoreServiceClient>();
  thrift::KeySetParams params;
  params.keyVals()->emplace(
      "key", createThriftValue(1, "node", std::string("value")));

  auto first = client->semifuture_setKvStoreKeyVals(params, kTestingAreaName);
  service->started.wait();
  auto second = client->semifuture_setKvStoreKeyVals(params, kTestingAreaName);
  // Rejected only once the second request is queued over the 1-byte limit.
  auto third = client->semifuture_setKvStoreKeyVals(params, kTestingAreaName);
  ASSERT_TRUE(std::move(third).getTry(std::chrono::seconds(5)).hasException());
  ASSERT_LT(0, backpressure->getOpenrIngressKvStoreQueuedGlobalBytes());

  // The peer goes away with the second request still queued.
  client.reset();
  service->release();
  // Stopping the server runs every queued request out of the pile.
  server.reset();

  EXPECT_EQ(0, backpressure->getOpenrIngressKvStoreQueuedGlobalBytes());
  EXPECT_EQ(0, backpressure->getNumPeersHoldingBytes());
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
      // Every request has finished, so every byte counted was released.
      EXPECT_EQ(
          0,
          openrIngressKvStoreBackpressure_
              ->getOpenrIngressKvStorePendingMergeGlobalBytes());
      EXPECT_EQ(
          0,
          openrIngressKvStoreBackpressure_
              ->getOpenrIngressKvStoreQueuedGlobalBytes());
      EXPECT_EQ(0, openrIngressKvStoreBackpressure_->getNumPeersHoldingBytes());
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
          server.setNumCPUWorkerThreads(1);
          openrIngressKvStoreBackpressure_->installRequestPile(server);
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

  int64_t
  getPauseMsCount() {
    fb303::fbData->flushAllData();
    return fb303::fbData->getCounter("kvstore.peer_ingress.pause_ms.count");
  }

  folly::coro::Task<void>
  setKey(int64_t version) {
    thrift::KeySetParams params;
    params.keyVals()->emplace(
        "key", createThriftValue(version, nodeName_, std::string("value")));
    params.senderId() = kPeer;
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
  EXPECT_EQ(
      0,
      openrIngressKvStoreBackpressure_
          ->getOpenrIngressKvStoreQueuedGlobalBytes());
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
                                       kPeer, kPauseAboveGlobalBytes + 1);

  co_await setKey(2);
  waitForServerIo();
  CO_ASSERT_EQ(1, openrIngressKvStoreBackpressure_->getPausedConnectionCount());

  heldPendingMergeGuard.reset();
  EXPECT_EQ(0, openrIngressKvStoreBackpressure_->getPausedConnectionCount());
  // Completes only if the connection is read again.
  co_await setKey(3);
}

CO_TEST_F(
    OpenrIngressKvStoreBackpressureTestFixture,
    DoesNotPausePeerBelowFairShare) {
  startServer(kLongPause);
  co_await setKey(1);
  waitForServerIo();
  // Another peer holds the bytes over the high mark.
  const auto heldPendingMergeGuard =
      openrIngressKvStoreBackpressure_
          ->trackOpenrIngressKvStorePendingMergeBytes(
              "noisy-peer", kPauseAboveGlobalBytes + 1);

  co_await setKey(2);
  waitForServerIo();

  EXPECT_EQ(0, openrIngressKvStoreBackpressure_->getPausedConnectionCount());
}

CO_TEST_F(
    OpenrIngressKvStoreBackpressureTestFixture,
    IncludesQueuedPeersInFairShare) {
  startServer(kLongPause);
  co_await setKey(1);
  waitForServerIo();

  for (bool identifyPeers : {false, true}) {
    std::vector<std::unique_ptr<apache::thrift::Cpp2ConnContext>> connections;
    for (int i = 0; i < 9; ++i) {
      auto& connection = connections.emplace_back(
          std::make_unique<apache::thrift::Cpp2ConnContext>());
      OpenrIngressKvStoreBackpressure::initConnection(*connection);
      if (identifyPeers) {
        openrIngressKvStoreBackpressure_->recordSenderId(
            *connection, "queued-peer" + std::to_string(i));
      }
      EXPECT_TRUE(
          openrIngressKvStoreBackpressure_
              ->admitOpenrIngressKvStoreQueuedRequest(*connection, 110));
    }
    auto heldPendingMergeGuard =
        openrIngressKvStoreBackpressure_
            ->trackOpenrIngressKvStorePendingMergeBytes(kPeer, 110);
    co_await setKey(identifyPeers ? 4 : 2);
    waitForServerIo();
    EXPECT_EQ(1, openrIngressKvStoreBackpressure_->getPausedConnectionCount());

    for (const auto& connection : connections) {
      openrIngressKvStoreBackpressure_->releaseOpenrIngressKvStoreQueuedRequest(
          *connection, 110);
    }
    heldPendingMergeGuard.reset();
    EXPECT_EQ(0, openrIngressKvStoreBackpressure_->getPausedConnectionCount());
    EXPECT_EQ(
        0,
        openrIngressKvStoreBackpressure_
            ->getOpenrIngressKvStoreQueuedGlobalBytes());
    co_await setKey(identifyPeers ? 5 : 3);
    waitForServerIo();
  }
}

CO_TEST_F(
    OpenrIngressKvStoreBackpressureTestFixture,
    CountsPeerWithQueuedAndPendingBytesOnce) {
  startServer(kLongPause);
  co_await setKey(1);
  waitForServerIo();
  apache::thrift::Cpp2ConnContext connection;
  OpenrIngressKvStoreBackpressure::initConnection(connection);
  openrIngressKvStoreBackpressure_->recordSenderId(connection, kPeer);
  EXPECT_TRUE(
      openrIngressKvStoreBackpressure_->admitOpenrIngressKvStoreQueuedRequest(
          connection, 100));
  const auto lightPeerGuard =
      openrIngressKvStoreBackpressure_
          ->trackOpenrIngressKvStorePendingMergeBytes(kPeer, 400);
  const auto heavyPeerGuard =
      openrIngressKvStoreBackpressure_
          ->trackOpenrIngressKvStorePendingMergeBytes("heavy-peer", 1001);

  co_await setKey(2);
  waitForServerIo();
  EXPECT_EQ(0, openrIngressKvStoreBackpressure_->getPausedConnectionCount());
  openrIngressKvStoreBackpressure_->releaseOpenrIngressKvStoreQueuedRequest(
      connection, 100);
}

CO_TEST_F(
    OpenrIngressKvStoreBackpressureTestFixture, ResumesAfterMaxPauseDuration) {
  startServer(kShortPause);
  co_await setKey(1);
  waitForServerIo();
  const auto pauseTimeouts = getPauseTimeouts();
  const auto pauseMsCount = getPauseMsCount();
  const auto heldPendingMergeGuard =
      openrIngressKvStoreBackpressure_
          ->trackOpenrIngressKvStorePendingMergeBytes(
              kPeer, kPauseAboveGlobalBytes + 1);

  // The second response pauses the connection while the guard is held.
  co_await setKey(2);
  // Completes only once maxPauseDuration resumes the connection.
  co_await setKey(3);

  EXPECT_LT(pauseTimeouts, getPauseTimeouts());
  EXPECT_LT(pauseMsCount, getPauseMsCount());
  EXPECT_LE(
      kShortPause.count(),
      fb303::fbData->getCounter("kvstore.peer_ingress.pause_ms.p100.60"));
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
              kPeer, kPauseAboveGlobalBytes + 1);

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
