/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/tests/mocks/MockNetlinkProtocolSocket.h>

namespace openr {

/**
 * This class serves as a wrapper to inject netlink events to
 * MockNetlinkProtocolSocket inside UT environment to mimick
 * netlink events happening.
 */

class NetlinkEventsInjector {
 public:
  explicit NetlinkEventsInjector(fbnl::MockNetlinkProtocolSocket* nlSock);

  ~NetlinkEventsInjector() = default;

  NetlinkEventsInjector(const NetlinkEventsInjector&) = delete;
  NetlinkEventsInjector& operator=(const NetlinkEventsInjector&) = delete;

  void getAllLinks(InterfaceDatabase& ifDb);

  void sendLinkEvent(
      const std::string& ifName, const uint64_t ifIndex, const bool isUp);

  void sendAddrEvent(
      const std::string& ifName, const std::string& prefix, const bool isValid);

 private:
  // mocked version of netlink protocols socket
  fbnl::MockNetlinkProtocolSocket* nlSock_{nullptr};

  // Interface/link name => link attributes mapping
  folly::Synchronized<
      std::unordered_map<std::string /* ifName */, InterfaceInfo>>
      linkDb_{};
};

} // namespace openr
