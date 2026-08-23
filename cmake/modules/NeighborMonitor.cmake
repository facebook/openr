# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# OSS implementation of the Buck //openr/neighbor-monitor:neighbor-monitor-header API.
set(OPENR_NEIGHBOR_MONITOR_SOURCES
  openr/neighbor-monitor/NeighborMonitor.cpp
)

set(OPENR_NEIGHBOR_MONITOR_EXPECTED_SOURCE_COUNT 1)

# Create the OSS neighbor monitor library. The OSS implementation is a
# no-op adapter; internal Buck builds provide the FSDB-backed implementation.
# Both implementations define the same symbols and must not be linked
# together.
macro(openr_add_neighbor_monitor_library)
  # Buck2 API target: //openr/neighbor-monitor:neighbor-monitor-header
  openr_add_library(
    NAME openr_neighbor_monitor
    SOURCES ${OPENR_NEIGHBOR_MONITOR_SOURCES}
    PUBLIC_DEPENDENCIES
      openr_common
      openr_lsdb_util
      openr_messaging
  )
  add_library(OpenR::neighbor_monitor ALIAS openr_neighbor_monitor)
endmacro()
