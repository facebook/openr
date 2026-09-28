/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <folly/IPAddress.h>
#include <folly/synchronization/Baton.h>
#include <thrift/lib/cpp/server/TServerObserver.h>
#include <thrift/lib/cpp/transport/TTransportException.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include <openr/common/OpenrClient.h>
#include <openr/if/gen-cpp2/OpenrCtrlCpp.h>

using apache::thrift::ScopedServerInterfaceThread;
using testing::Eq;

namespace openr {
namespace {

const std::string kRunningConfig{R"({"node_name":"test-node"})"};

class TestOpenrCtrlHandler
    : public apache::thrift::ServiceHandler<thrift::OpenrCtrlCpp> {
 public:
  void
  getRunningConfig(std::string& config) override {
    config = kRunningConfig;
  }
};

/* Count num requests that reach inner handler */
class CountingOpenrCtrlHandler
    : public apache::thrift::ServiceHandler<thrift::OpenrCtrlCpp> {
 public:
  void
  getRunningConfig(std::string& config) override {
    ++handledCount;
    config = kRunningConfig;
  }

  std::atomic<int> handledCount{0};
};

/* Count num requests that reach the server, including those transport rejects.
 */
class CountingServerObserver : public apache::thrift::server::TServerObserver {
 public:
  void
  receivedRequest(const std::string* /*method*/) override {
    if (++receivedCount == 1) {
      firstRequest.post();
    }
  }

  std::atomic<int> receivedCount{0};
  folly::Baton<> firstRequest;
};

using OpenrCtrlClient = apache::thrift::Client<thrift::OpenrCtrlCpp>;

} // namespace

/* We can build the client and use it to make requests */
TEST(OpenrClientTest, IssuesRpcAgainstLiveServer) {
  ScopedServerInterfaceThread server(
      std::make_shared<TestOpenrCtrlHandler>(), "::1");

  const std::shared_ptr<OpenrCtrlClient> client =
      getOpenrClient<thrift::OpenrCtrlCpp>(
          server.getAddress().getIPAddress(), server.getPort());

  std::string config;
  client->sync_getRunningConfig(config);

  EXPECT_THAT(config, Eq(kRunningConfig));
}

/* Client is safely usable from multiple threads concurrently;
 * also multiplex across multiple eventbase threads for good measure */
TEST(OpenrClientTest, ServesConcurrentCallsFromManyThreads) {
  constexpr size_t kNumThreads = 16;

  ScopedServerInterfaceThread server(
      std::make_shared<TestOpenrCtrlHandler>(), "::1");

  const std::shared_ptr<OpenrCtrlClient> client =
      getOpenrClient<thrift::OpenrCtrlCpp>(
          server.getAddress().getIPAddress(),
          server.getPort(),
          OpenrClientOpts{.numIOThreads = 4});

  // Trigger everyone at once
  std::latch start{1};
  std::vector<std::string> configs(kNumThreads);
  std::vector<std::thread> threads;

  threads.reserve(kNumThreads);
  for (size_t i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&client, &start, &configs, i]() {
      start.wait();
      client->sync_getRunningConfig(configs[i]);
    });
  }

  start.count_down();
  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_THAT(
      configs, Eq(std::vector<std::string>(kNumThreads, kRunningConfig)));
}

/* If we destroy the server backing the client the client can automatically
 * recover when the server comes back up */
TEST(OpenrClientTest, RecoversAfterServerRestart) {
  const std::shared_ptr<TestOpenrCtrlHandler> handler =
      std::make_shared<TestOpenrCtrlHandler>();
  std::unique_ptr<ScopedServerInterfaceThread> server =
      std::make_unique<ScopedServerInterfaceThread>(handler, "::1");
  const folly::IPAddress addr = server->getAddress().getIPAddress();
  const uint16_t port = server->getPort();

  const std::shared_ptr<OpenrCtrlClient> client =
      getOpenrClient<thrift::OpenrCtrlCpp>(addr, port);

  std::string before;
  client->sync_getRunningConfig(before);
  ASSERT_THAT(before, Eq(kRunningConfig));

  // Bring server down
  server.reset();
  std::string whileDown;
  EXPECT_THROW(
      client->sync_getRunningConfig(whileDown),
      apache::thrift::transport::TTransportException);

  // Bring server back up
  server = std::make_unique<ScopedServerInterfaceThread>(handler, "::1", port);

  std::string after;
  client->sync_getRunningConfig(after);

  EXPECT_THAT(after, Eq(kRunningConfig));
}

/* We retry transport errors the specified number of times */
TEST(OpenrClientTest, RetriesDroppedRequest) {
  auto handler = std::make_shared<CountingOpenrCtrlHandler>();
  auto observer = std::make_shared<CountingServerObserver>();
  ScopedServerInterfaceThread server(
      handler, "::1", 0, [observer](apache::thrift::ThriftServer& ts) {
        // count incoming requests including drops
        ts.setObserver(observer);
      });

  apache::thrift::ThriftServer::FailureInjection dropAll;
  dropAll.dropFraction = 1;
  server.getThriftServer().setFailureInjection(dropAll);

  const std::shared_ptr<OpenrCtrlClient> client =
      getOpenrClient<thrift::OpenrCtrlCpp>(
          server.getAddress().getIPAddress(),
          server.getPort(),
          OpenrClientOpts{
              .processingTimeout = std::chrono::milliseconds(200),
              .numTransportFailureRetries = 1});

  auto resultFuture = std::async(std::launch::async, [&] {
    std::string config;
    client->sync_getRunningConfig(config);
    return config;
  });
  /* The moment we get the first request clear failure injection state
   * We drop the request & 200ms is plenty of time to clear it. */
  ASSERT_TRUE(observer->firstRequest.try_wait_for(std::chrono::seconds(10)));
  server.getThriftServer().setFailureInjection({});

  EXPECT_THAT(resultFuture.get(), Eq(kRunningConfig));
  // We actually retried
  EXPECT_THAT(observer->receivedCount.load(), Eq(2));
  // Verify first request was actually dropped
  EXPECT_THAT(handler->handledCount.load(), Eq(1));
}

/* No retry behaviour when we're configured to not retry */
TEST(OpenrClientTest, NoRetrySurfacesDroppedRequest) {
  auto handler = std::make_shared<CountingOpenrCtrlHandler>();
  ScopedServerInterfaceThread server(handler, "::1");

  apache::thrift::ThriftServer::FailureInjection dropAll;
  dropAll.dropFraction = 1;
  server.getThriftServer().setFailureInjection(dropAll);

  const std::shared_ptr<OpenrCtrlClient> client =
      getOpenrClient<thrift::OpenrCtrlCpp>(
          server.getAddress().getIPAddress(),
          server.getPort(),
          OpenrClientOpts{.processingTimeout = std::chrono::milliseconds(200)});

  std::string config;
  EXPECT_THROW(
      client->sync_getRunningConfig(config),
      apache::thrift::transport::TTransportException);
  EXPECT_THAT(handler->handledCount.load(), Eq(0));
}

} // namespace openr
