/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>

#include <folly/IPAddressV6.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>

namespace openr {
namespace fbmeshd {

class PeriodicPinger : public folly::AsyncTimeout {
 public:
  PeriodicPinger(
      folly::EventBase* evb,
      folly::IPAddressV6 dst,
      folly::IPAddressV6 src,
      std::chrono::milliseconds interval,
      const std::string& interface);

  PeriodicPinger() = delete;
  ~PeriodicPinger() override = default;
  PeriodicPinger(const PeriodicPinger&) = delete;
  PeriodicPinger(PeriodicPinger&&) = delete;
  PeriodicPinger& operator=(const PeriodicPinger&) = delete;
  PeriodicPinger& operator=(PeriodicPinger&&) = delete;

 private:
  virtual void timeoutExpired() noexcept override;

  folly::IPAddressV6 dst_;
  folly::IPAddressV6 src_;
  std::chrono::milliseconds interval_;
  const std::string& interface_;
};

} // namespace fbmeshd
} // namespace openr
