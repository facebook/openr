# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sources grouped by their Buck Decision-library ownership.
set(OPENR_LINK_SOURCES openr/decision/Link.cpp)
set(OPENR_DECISION_STRUCTS_SOURCES openr/decision/RibPolicy.cpp)
set(OPENR_FABRIC_HELPER_SOURCES openr/decision/FabricHelper.cpp)
set(
  OPENR_SPF_SOLVER_SOURCES
  openr/decision/LinkState.cpp
  openr/decision/PrefixState.cpp
  openr/decision/SpfSolver.cpp
)
set(OPENR_DECISION_SOURCES openr/decision/Decision.cpp)

set(OPENR_DECISION_EXPECTED_SOURCE_COUNT 7)

# Create Decision libraries from the leaf graph toward the service.
macro(openr_add_decision_libraries)
  # Buck2 target: //openr/decision:link
  openr_add_library(
    NAME openr_link
    SOURCES ${OPENR_LINK_SOURCES}
    PUBLIC_DEPENDENCIES
      network_cpp2
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::link ALIAS openr_link)

  # Buck2 target: //openr/decision:structs
  openr_add_library(
    NAME openr_decision_structs
    SOURCES ${OPENR_DECISION_STRUCTS_SOURCES}
    PRIVATE_DEPENDENCIES
      fb303::fb303
    PUBLIC_DEPENDENCIES
      openr_rib_entry
      openr_network_util
      openr_ctrl_cpp2
      platform_cpp2
      network_cpp2
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::decision_structs ALIAS openr_decision_structs)

  # Buck2 target: //openr/decision:fabric_helper
  openr_add_library(
    NAME openr_fabric_helper
    SOURCES ${OPENR_FABRIC_HELPER_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_constants
      glog::glog
    PUBLIC_DEPENDENCIES
      fmt::fmt
      openr_link
      openr_common
      openr_lsdb_util
      openr_network_util
      openr_util
      openr_config
      kv_store_cpp2
      network_cpp2
      types_cpp2
      openr_messaging
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::fabric_helper ALIAS openr_fabric_helper)

  # Buck2 target: //openr/decision:spf_solver
  openr_add_library(
    NAME openr_spf_solver
    SOURCES ${OPENR_SPF_SOLVER_SOURCES}
    PRIVATE_DEPENDENCIES
      fb303::fb303
      openr_mpls_util
      openr_config_cpp2
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_fabric_helper
      openr_link
      openr_rib_entry
      openr_decision_structs
      openr_constants
      openr_lsdb_util
      openr_network_util
      openr_ctrl_cpp2
      network_cpp2
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::spf_solver ALIAS openr_spf_solver)

  # Buck2 target: //openr/decision:decision
  openr_add_library(
    NAME openr_decision
    SOURCES ${OPENR_DECISION_SOURCES}
    PRIVATE_DEPENDENCIES
      fmt::fmt
      fb303::fb303
      ${FOLLY_EXCEPTION_TRACER}
      openr_constants
      openr_flags
      openr_lsdb_util
      openr_network_util
      openr_profiler
      openr_ctrl_cpp2
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_rib_entry
      openr_spf_solver
      openr_decision_structs
      openr_common
      openr_util
      openr_config
      kv_store_cpp2
      types_cpp2
      openr_messaging
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::decision ALIAS openr_decision)
endmacro()
