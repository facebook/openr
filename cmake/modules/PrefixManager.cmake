# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Source owned by the Buck //openr/prefix-manager:prefix-manager target.
set(OPENR_PREFIX_MANAGER_SOURCES
  openr/prefix-manager/PrefixManager.cpp
)

set(OPENR_PREFIX_MANAGER_EXPECTED_SOURCE_COUNT 1)

# Create the prefix advertisement and redistribution module. The public OSS
# build uses the openr_policy fallback; internal Buck builds inject their
# policy implementation through the same PolicyManager API.
macro(openr_add_prefix_manager_library)
  # Buck2 target: //openr/prefix-manager:prefix-manager
  openr_add_library(
    NAME openr_prefix_manager
    SOURCES ${OPENR_PREFIX_MANAGER_SOURCES}
    PRIVATE_DEPENDENCIES
      fmt::fmt
      fb303::fb303
      ${FOLLY_EXCEPTION_TRACER}
      openr_constants
      openr_network_util
      openr_config_cpp2
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_common
      openr_lsdb_util
      openr_util
      openr_config
      openr_decision_structs
      openr_messaging
      openr_policy
      network_cpp2
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::prefix_manager ALIAS openr_prefix_manager)
endmacro()
