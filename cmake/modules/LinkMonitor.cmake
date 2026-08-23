# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sources owned by the Buck adjacency-entry and link-monitor targets.
set(
  OPENR_ADJACENCY_ENTRY_SOURCES
  openr/link-monitor/AdjacencyEntry.cpp
)

set(
  OPENR_LINK_MONITOR_SOURCES
  openr/link-monitor/InterfaceEntry.cpp
  openr/link-monitor/LinkMonitor.cpp
)

set(OPENR_LINK_MONITOR_EXPECTED_SOURCE_COUNT 3)

# Create the Link Monitor libraries in dependency order.
macro(openr_add_link_monitor_libraries)
  # Buck2 target: //openr/link-monitor:adjacency-entry
  openr_add_library(
    NAME openr_adjacency_entry
    SOURCES ${OPENR_ADJACENCY_ENTRY_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_network_util
      openr_util
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_common
      openr_lsdb_util
      Folly::folly
  )
  add_library(OpenR::adjacency_entry ALIAS openr_adjacency_entry)

  # Buck2 target: //openr/link-monitor:link-monitor
  openr_add_library(
    NAME openr_link_monitor
    SOURCES ${OPENR_LINK_MONITOR_SOURCES}
    PRIVATE_DEPENDENCIES
      fb303::fb303
      openr_constants
      openr_network_util
      openr_util
      openr_config
      network_cpp2
    PUBLIC_DEPENDENCIES
      openr_adjacency_entry
      openr_common
      openr_lsdb_util
      openr_persistent_store
      openr_messaging
      openr_log_sample
      openr_fbnl
      kv_store_cpp2
      openr_config_cpp2
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
      glog::glog
      ${RE2}
  )
  add_library(OpenR::link_monitor ALIAS openr_link_monitor)
endmacro()
