/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <list>

#include <fbzmq/async/ZmqEventLoop.h>

#include <openr/nl/NetlinkProtocolSocket.h>

namespace openr::fbnl {

/**
 * Defines a fake implementation for netlink protocol socket. Instead of writing
 * state to Linux kernel, the API calls made here instead read/write into the
 * state maintained in memory. There are also specialized APIs to update the
 * state.
 *
 * This class facilitates testing of application logic with unit-tests.
 */
class FakeNetlinkProtocolSocket : public NetlinkProtocolSocket {
 public:
  explicit FakeNetlinkProtocolSocket(fbzmq::ZmqEventLoop* evl)
      : NetlinkProtocolSocket(evl) {}

  /**
   * API to create links for testing purposes
   */
  folly::SemiFuture<int> addLink(const fbnl::Link& link);

  /**
   * Overrides API of NetlinkProtocolSocket for testing
   */

  folly::SemiFuture<int> addRoute(const fbnl::Route& route) override;
  folly::SemiFuture<int> deleteRoute(const fbnl::Route& route) override;
  folly::SemiFuture<std::vector<fbnl::Route>> getRoutes(
      const fbnl::Route& filter) override;

  folly::SemiFuture<int> addIfAddress(const fbnl::IfAddress&) override;
  folly::SemiFuture<int> deleteIfAddress(const fbnl::IfAddress&) override;
  folly::SemiFuture<std::vector<fbnl::IfAddress>> getAllIfAddresses() override;

  folly::SemiFuture<std::vector<fbnl::Link>> getAllLinks() override;

  folly::SemiFuture<std::vector<fbnl::Neighbor>> getAllNeighbors() override;

 protected:
  void
  init() override {
    // empty
  }

 private:
  // map<ifIndex -> Link>
  std::map<int, fbnl::Link> links_;

  // map<ifIndex -> list<IfAddress>>
  std::map<int, std::list<fbnl::IfAddress>> ifAddrs_;

  // map<table-id -> map<{prefix, priority}, Route>
  std::map<int, std::map<std::pair<folly::CIDRNetwork, uint8_t>, fbnl::Route>>
      routes_;
};

} // namespace openr::fbnl
