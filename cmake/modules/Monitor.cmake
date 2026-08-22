# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sources corresponding to the four Buck target boundaries in monitor/.
set(
  OPENR_LOG_SAMPLE_SOURCES
  openr/monitor/LogSample.cpp
)

set(
  OPENR_SYSTEM_METRICS_SOURCES
  openr/monitor/SystemMetrics.cpp
)

set(
  OPENR_MONITOR_BASE_SOURCES
  openr/monitor/MonitorBase.cpp
)

# The OSS adapter logs events and leaves heap profiling to integrators.
# Internal Buck builds use facebook/Monitor.cpp instead; the two sources
# implement the same methods and must never be compiled together.
set(OPENR_MONITOR_SOURCES openr/monitor/Monitor.cpp)

set(OPENR_MONITOR_EXPECTED_SOURCE_COUNT 4)

# Create the Monitor libraries in dependency order.
macro(openr_add_monitor_libraries)
  # Buck2 target: //openr/monitor:log_sample
  openr_add_library(
    NAME openr_log_sample
    SOURCES ${OPENR_LOG_SAMPLE_SOURCES}
    PRIVATE_DEPENDENCIES fmt::fmt
    PUBLIC_DEPENDENCIES Folly::folly
  )
  add_library(OpenR::log_sample ALIAS openr_log_sample)

  # Buck2 target: //openr/monitor:system_metrics
  openr_add_library(
    NAME openr_system_metrics
    SOURCES ${OPENR_SYSTEM_METRICS_SOURCES}
    PRIVATE_DEPENDENCIES fmt::fmt Folly::folly
    PUBLIC_DEPENDENCIES
      glog::glog
      ${RE2}
  )
  add_library(OpenR::system_metrics ALIAS openr_system_metrics)

  # Buck2 target: //openr/monitor:monitor_base
  openr_add_library(
    NAME openr_monitor_base
    SOURCES ${OPENR_MONITOR_BASE_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_constants
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_common
      openr_config
      openr_log_sample
      openr_messaging
      openr_system_metrics
      fb303::fb303
      Folly::folly
  )
  add_library(OpenR::monitor_base ALIAS openr_monitor_base)

  # Buck2 target: //openr/monitor:monitor
  openr_add_library(
    NAME openr_monitor
    SOURCES ${OPENR_MONITOR_SOURCES}
    PRIVATE_DEPENDENCIES Folly::folly
    PUBLIC_DEPENDENCIES openr_monitor_base
  )
  add_library(OpenR::monitor ALIAS openr_monitor)
endmacro()
