# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sources owned by the Buck //openr/common:lsdb_util target.
set(
  OPENR_LSDB_UTIL_SOURCES
  openr/common/LsdbTypes.cpp
  openr/common/LsdbUtil.cpp
)

set(OPENR_LSDB_EXPECTED_SOURCE_COUNT 2)

# Create the LSDB library and the header-only leaf targets it exposes.
#
# RibEntry and PolicyStructs are standalone Buck leaf targets. Keeping LSDB
# dependent on those leaves preserves layering and avoids Decision's
# dependency back to LSDB.
macro(openr_add_lsdb_library)
  add_library(openr_policy_struct INTERFACE)
  add_library(OpenR::policy_struct ALIAS openr_policy_struct)

  add_library(openr_rib_entry INTERFACE)
  target_link_libraries(
    openr_rib_entry
    INTERFACE
      openr_network_util
      openr_ctrl_cpp2
      network_cpp2
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
      glog::glog
  )
  add_library(OpenR::rib_entry ALIAS openr_rib_entry)

  # Buck2 target: //openr/common:lsdb_util
  openr_add_library(
    NAME openr_lsdb_util
    SOURCES ${OPENR_LSDB_UTIL_SOURCES}
    PRIVATE_DEPENDENCIES
      fmt::fmt
      openr_mpls_util
    PUBLIC_DEPENDENCIES
      openr_constants
      openr_build_info
      openr_common
      openr_network_util
      openr_util
      openr_rib_entry
      openr_policy_struct
      network_cpp2
      openr_config_cpp2
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
      ${RE2}
      ${Boost_LIBRARIES}
  )
  add_library(OpenR::lsdb_util ALIAS openr_lsdb_util)
endmacro()
