# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Source owned by the Buck //openr/watchdog:watchdog target.
set(OPENR_WATCHDOG_SOURCES
  openr/watchdog/Watchdog.cpp
)

set(OPENR_WATCHDOG_EXPECTED_SOURCE_COUNT 1)

# Create the process-health monitor. SystemMetrics is public because the
# Watchdog object embeds it, while counters and crash formatting are private
# implementation details.
macro(openr_add_watchdog_library)
  # Buck2 target: //openr/watchdog:watchdog
  openr_add_library(
    NAME openr_watchdog
    SOURCES ${OPENR_WATCHDOG_SOURCES}
    PRIVATE_DEPENDENCIES
      fmt::fmt
      fb303::fb303
      openr_constants
      openr_util
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_common
      openr_config
      openr_messaging
      openr_system_metrics
      Folly::folly
  )
  add_library(OpenR::watchdog ALIAS openr_watchdog)
endmacro()
