/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RouteDampener.h"

#include <stdexcept>

using namespace openr::fbmeshd;

RouteDampener::RouteDampener(
    fbzmq::ZmqEventLoop* eventLoop,
    unsigned int penalty,
    unsigned int suppressLimit,
    unsigned int reuseLimit,
    std::chrono::seconds halfLife,
    std::chrono::seconds maxSuppressLimit)
    : eventLoop_{eventLoop},
      halfLifeTimer_{fbzmq::ZmqTimeout::make(
          eventLoop_, [this]() mutable noexcept { halfLifeTimerExpired(); })},
      maxSuppressLimitTimer_{fbzmq::ZmqTimeout::make(
          eventLoop_,
          [this]() mutable noexcept { maxSuppressLimitTimerExpired(); })},
      penalty_{penalty},
      suppressLimit_{suppressLimit},
      reuseLimit_{reuseLimit},
      halfLife_{halfLife},
      maxSuppressLimit_{maxSuppressLimit} {
  if (!isValid()) {
    throw std::range_error(
        "route dampener values are not logically consistent");
  }
}

bool
RouteDampener::isValid() const {
  const unsigned int maxPenalty{reuseLimit_ *
                                (1 << (maxSuppressLimit_ / halfLife_))};
  return penalty_ <= maxPenalty;
}

unsigned int
RouteDampener::getHistory() const {
  return history_;
}

void
RouteDampener::setHistory(unsigned int newHistory) {
  history_ = newHistory;
  setRdStat("default_route_history", history_);
  LOG(INFO) << "route dampener history set to " << history_;
}

void
RouteDampener::flap() {
  eventLoop_->runImmediatelyOrInEventLoop([this]() {
    setHistory(history_ + penalty_);

    LOG(INFO) << "route dampener received flap";

    if (!dampened_ && history_ >= suppressLimit_) {
      LOG(INFO) << "route dampener dampening route ";
      dampened_ = true;
      setRdStat("default_route_dampened", 1);
      startHalfLifeTimer();
      startMaxSuppressLimitTimer();
      dampen();
    }
  });
}

bool
RouteDampener::isDampened() const {
  return dampened_;
}

void
RouteDampener::undampenImpl() {
  CHECK(dampened_);

  LOG(INFO) << "route dampener undampening route ";

  dampened_ = false;
  stopMaxSuppressLimitTimer();
  undampen();
  setRdStat("default_route_dampened", 0);
}

void
RouteDampener::halfLifeTimerExpired() {
  setHistory(history_ / 2);

  LOG(INFO) << "route dampener half life timer expired";

  if (history_ <= (reuseLimit_ / 2)) {
    LOG(INFO) << "route dampener reseting history";
    setHistory(0);
    stopHalfLifeTimer();
  }

  if (dampened_ && history_ <= reuseLimit_) {
    undampenImpl();
  }
  userHalfLifeTimerExpired();
}

void
RouteDampener::maxSuppressLimitTimerExpired() {
  LOG(INFO) << "route dampener max suppress limit timer expired";
  if (dampened_) {
    setHistory(0);
    stopHalfLifeTimer();
    undampenImpl();
  }
  userSuppressLimitTimerExpired();
}

void
RouteDampener::startHalfLifeTimer() {
  DCHECK_NOTNULL(halfLifeTimer_.get());
  halfLifeTimer_->scheduleTimeout(halfLife_, true);
}

void
RouteDampener::startMaxSuppressLimitTimer() {
  DCHECK_NOTNULL(maxSuppressLimitTimer_.get());
  maxSuppressLimitTimer_->scheduleTimeout(maxSuppressLimit_, false);
}

void
RouteDampener::stopMaxSuppressLimitTimer() {
  DCHECK_NOTNULL(maxSuppressLimitTimer_.get());
  if (maxSuppressLimitTimer_->isScheduled()) {
    maxSuppressLimitTimer_->cancelTimeout();
  }
}

void
RouteDampener::stopHalfLifeTimer() {
  DCHECK_NOTNULL(halfLifeTimer_.get());
  if (halfLifeTimer_->isScheduled()) {
    halfLifeTimer_->cancelTimeout();
  }
}
