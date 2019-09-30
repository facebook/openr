// Copyright 2004-present Facebook. All Rights Reserved.

#ifdef ENABLE_SYSTEMD_NOTIFY
#include <systemd/sd-daemon.h> // @manual
#endif

#include <folly/Format.h>
#include <glog/logging.h>

#include "openr/fbmeshd/notifier/Notifier.h"

namespace openr {
namespace fbmeshd {

Notifier::Notifier(folly::EventBase* evb, std::chrono::milliseconds interval) {
  (void)evb;
  (void)interval;
#ifdef ENABLE_SYSTEMD_NOTIFY
  uint64_t watchdogEnv{0};
  int status{0};
  // Always expect the watchdog to be set if systemd is here.
  CHECK_GE((status = sd_watchdog_enabled(0, &watchdogEnv)), 0)
      << "Problem when fetching systemd watchdog";
  if (status == 0) {
    return;
  }

  notifierTimer_ = folly::AsyncTimeout::make(*evb, [this, interval]() noexcept {
    doNotify();
    notifierTimer_->scheduleTimeout(interval);
  });
  notifierTimer_->scheduleTimeout(interval);

  VLOG(8) << folly::sformat(
      "Started timer to notify systemd every {} ms.", interval.count());
#endif
}

void
Notifier::doNotify() {
#ifdef ENABLE_SYSTEMD_NOTIFY
  VLOG(8) << "Notifying the systemd watchdog...";

  sd_notify(0, "WATCHDOG=1");
#endif
}

} // namespace fbmeshd
} // namespace openr
