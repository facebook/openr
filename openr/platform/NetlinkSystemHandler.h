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

#include <folly/futures/Future.h>
#include <folly/io/async/AsyncSocket.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/if/gen-cpp2/SystemService.h>
#include <openr/nl/NetlinkProtocolSocket.h>

namespace openr {

/**
 * This class implements Netlink Platform thrift interface for programming
 * NetlinkEvent Publisher as well as System Service on linux platform.
 * We implement the futures API to allow for easy async activity within
 * the handlers
 */

class NetlinkSystemHandler : public thrift::SystemServiceSvIf {
 public:
  explicit NetlinkSystemHandler(fbnl::NetlinkProtocolSocket* nlSock);

  NetlinkSystemHandler(const NetlinkSystemHandler&) = delete;
  NetlinkSystemHandler& operator=(const NetlinkSystemHandler&) = delete;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::Link>>>
  semifuture_getAllLinks() override;

  folly::SemiFuture<folly::Unit> semifuture_addIfaceAddresses(
      std::unique_ptr<std::string> iface,
      std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) override;

  folly::SemiFuture<folly::Unit> semifuture_removeIfaceAddresses(
      std::unique_ptr<std::string> iface,
      std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) override;

  folly::SemiFuture<folly::Unit> semifuture_syncIfaceAddresses(
      std::unique_ptr<std::string> iface,
      int16_t family,
      int16_t scope,
      std::unique_ptr<std::vector<thrift::IpPrefix>> addrs) override;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::IpPrefix>>>
  semifuture_getIfaceAddresses(
      std::unique_ptr<std::string> iface,
      int16_t family,
      int16_t scope) override;

 private:
  /**
   * Helper function to add/remove addresses
   */
  folly::SemiFuture<folly::Unit> addRemoveIfAddresses(
      const bool isAdd,
      const std::string& ifName,
      const std::vector<thrift::IpPrefix>& addrs);

  /**
   * Synchronous API to query interface index from kernel.
   * NOTE: We intentionally don't use cache to optimize this call as APIs of
   * this handlers for add/remove/sync addresses are rarely invoked.
   */
  std::optional<int> getIfIndex(const std::string& ifName);

  fbnl::NetlinkProtocolSocket* nlSock_{nullptr};
};

} // namespace openr
