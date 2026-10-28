/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/testing/TestUtil.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

#include <openr/messaging/ReplicateQueue.h>
#include <openr/nl/NetlinkProtocolSocket.h>
#include <openr/tests/mocks/MockNetlinkFibHandler.h>
#include <openr/tests/mocks/MockNetlinkProtocolSocket.h>
#include <openr/tests/mocks/NetlinkEventsInjector.h>

namespace openr::e2e {

class TestPlatform final {
 public:
  struct FibUpdateCounts {
    std::size_t syncCalls{0};
    std::size_t addedRoutes{0};
    std::size_t deletedRoutes{0};
  };

  TestPlatform(
      std::string nodeName,
      messaging::ReplicateQueue<fbnl::NetlinkEvent>& netlinkEvents);
  ~TestPlatform();

  TestPlatform(const TestPlatform&) = delete;
  TestPlatform& operator=(const TestPlatform&) = delete;

  void start();
  void stop();
  void restartFibService();

  uint16_t fibPort() const;
  const std::string& persistentStorePath() const;

  fbnl::MockNetlinkProtocolSocket& netlinkSocket();
  MockNetlinkFibHandler& fibHandler();

  void setLink(const std::string& interfaceName, int interfaceIndex, bool isUp);
  void setAddress(
      const std::string& interfaceName,
      const std::string& prefix,
      bool isPresent);

  std::vector<thrift::UnicastRoute> fibRoutes() const;
  FibUpdateCounts fibUpdateCounts() const;

 private:
  folly::test::TemporaryDirectory stateDirectory_;
  std::string persistentStorePath_;
  folly::EventBase netlinkEventBase_;
  std::unique_ptr<fbnl::MockNetlinkProtocolSocket> netlinkSocket_;
  std::unique_ptr<NetlinkEventsInjector> netlinkEvents_;
  std::shared_ptr<MockNetlinkFibHandler> fibHandler_;
  folly::AsyncServerSocket::UniquePtr fibSocket_;
  std::shared_ptr<apache::thrift::ThriftServer> fibServer_;
  std::unique_ptr<apache::thrift::util::ScopedServerThread> fibServerThread_;
  uint16_t fibPort_{0};
  bool started_{false};
  bool running_{false};
};

} // namespace openr::e2e
