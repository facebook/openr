/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/tests/mocks/MockNetlinkProtocolSocket.h>

namespace openr {

/**
 * This class implements Netlink Platform thrift interface for programming
 * NetlinkEvent Publisher as well as System Service on linux platform.
 */

class MockNetlinkSystemHandler {
 public:
  explicit MockNetlinkSystemHandler(fbnl::MockNetlinkProtocolSocket* nlSock);

  ~MockNetlinkSystemHandler() = default;

  MockNetlinkSystemHandler(const MockNetlinkSystemHandler&) = delete;
  MockNetlinkSystemHandler& operator=(const MockNetlinkSystemHandler&) = delete;

  void getAllLinks(std::vector<thrift::Link>& linkDb);

  void sendLinkEvent(
      const std::string& ifName, const uint64_t ifIndex, const bool isUp);

  void sendAddrEvent(
      const std::string& ifName, const std::string& prefix, const bool isValid);

 private:
  // mocked version of netlink protocols socket
  fbnl::MockNetlinkProtocolSocket* nlSock_{nullptr};

  // Interface/link name => link attributes mapping
  folly::Synchronized<fbnl::NlLinks> linkDb_{};
};

} // namespace openr
