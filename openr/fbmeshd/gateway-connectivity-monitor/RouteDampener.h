/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cmath>

#include <glog/logging.h>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <folly/Format.h>

#include <openr/fbmeshd/common/Constants.h>

namespace openr {
namespace fbmeshd {

// This is a loose implementation of route dampening similar to BGP. Generally
// is not advisable to deploy bgp route dampening but that mostly has to do with
// the network effects of route propagation. This should be better as its only
// done on the edge.
class RouteDampener {
 public:
  explicit RouteDampener(
      fbzmq::ZmqEventLoop* _eventLoop,
      unsigned int penalty = Constants::kDefaultPenalty,
      unsigned int suppressLimit = Constants::kDefaultSuppressLimit,
      unsigned int reuseLimit = Constants::kDefaultReuseLimit,
      std::chrono::seconds halfLife = Constants::kDefaultHalfLife,
      std::chrono::seconds maxSuppressLimit =
          Constants::kDefaultMaxSuppressLimit);
  RouteDampener() = delete;
  virtual ~RouteDampener() = default;
  RouteDampener(const RouteDampener&) = delete;
  RouteDampener& operator=(const RouteDampener&) = delete;
  RouteDampener(RouteDampener&&) = delete;
  RouteDampener& operator=(RouteDampener&&) = delete;

 public:
  void flap();

  bool isDampened() const;

  bool isValid() const;

  virtual void dampen() = 0;
  virtual void undampen() = 0;
  virtual void setStat(const std::string& path, int value) = 0;

  virtual void
  userHalfLifeTimerExpired() {}
  virtual void
  userSuppressLimitTimerExpired() {}

  void setHistory(unsigned int newHistory);

  unsigned int getHistory() const;

  template <typename... Params>
  void
  setRdStat(const std::string& templateS, int value, Params&&... params) {
    static constexpr folly::StringPiece rdPrefixTemplate{"route_dampener.{}"};
    setStat(
        folly::sformat(
            rdPrefixTemplate,
            folly::sformat(templateS, std::forward<Params>(params)...)),
        value);
  }

 private:
  void halfLifeTimerExpired();
  void maxSuppressLimitTimerExpired();

  void undampenImpl();

  void startHalfLifeTimer();
  void startMaxSuppressLimitTimer();
  void stopMaxSuppressLimitTimer();
  void stopHalfLifeTimer();

 private:
  fbzmq::ZmqEventLoop* eventLoop_{nullptr};
  unsigned int history_{0};
  bool dampened_{false};
  std::unique_ptr<fbzmq::ZmqTimeout> halfLifeTimer_{nullptr};
  std::unique_ptr<fbzmq::ZmqTimeout> maxSuppressLimitTimer_{nullptr};
  const unsigned int penalty_;
  const unsigned int suppressLimit_;
  const unsigned int reuseLimit_;
  const std::chrono::seconds halfLife_;
  const std::chrono::seconds maxSuppressLimit_;
};

} // namespace fbmeshd
} // namespace openr
