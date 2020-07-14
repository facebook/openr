/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <syslog.h>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <fbzmq/zmq/Zmq.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/if/gen-cpp2/SystemService.h>
#include <openr/nl/tests/FakeNetlinkProtocolSocket.h>
#include <openr/platform/PlatformPublisher.h>

namespace openr {

/**
 * This class implements Netlink Platform thrift interface for programming
 * NetlinkEvent Publisher as well as System Service on linux platform.
 */

class MockNetlinkSystemHandler final : public thrift::SystemServiceSvIf {
 public:
  explicit MockNetlinkSystemHandler(fbnl::FakeNetlinkProtocolSocket* nlSock);

  ~MockNetlinkSystemHandler() override = default;

  MockNetlinkSystemHandler(const MockNetlinkSystemHandler&) = delete;
  MockNetlinkSystemHandler& operator=(const MockNetlinkSystemHandler&) = delete;

  void getAllLinks(std::vector<thrift::Link>& linkDb) override;

  void sendLinkEvent(
      const std::string& ifName, const uint64_t ifIndex, const bool isUp);

  void sendAddrEvent(
      const std::string& ifName, const std::string& prefix, const bool isValid);

  void stop();

 private:
  // mocked version of netlink protocols socket
  fbnl::FakeNetlinkProtocolSocket* nlSock_{nullptr};

  // Interface/link name => link attributes mapping
  folly::Synchronized<fbnl::NlLinks> linkDb_{};
};

} // namespace openr
