/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>

#include <folly/IPAddressV6.h>

namespace openr {
namespace fbmeshd {

class PeriodicPinger : public fbzmq::ZmqEventLoop {
 public:
  PeriodicPinger(
      folly::IPAddressV6 dst,
      folly::IPAddressV6 src,
      std::chrono::milliseconds interval,
      const std::string& interface);

  PeriodicPinger() = delete;
  ~PeriodicPinger() = default;
  PeriodicPinger(const PeriodicPinger&) = delete;
  PeriodicPinger(PeriodicPinger&&) = delete;
  PeriodicPinger& operator=(const PeriodicPinger&) = delete;
  PeriodicPinger& operator=(PeriodicPinger&&) = delete;

 private:
  void doPing();

  folly::IPAddressV6 dst_;
  folly::IPAddressV6 src_;
  std::unique_ptr<fbzmq::ZmqTimeout> periodicPingerTimer_;
  const std::string& interface_;
};

} // namespace fbmeshd
} // namespace openr
