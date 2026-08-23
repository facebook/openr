# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Source owned by the Buck //openr/fib:fib target.
set(OPENR_FIB_SOURCES openr/fib/Fib.cpp)

set(OPENR_FIB_EXPECTED_SOURCE_COUNT 1)

# Create the FIB programming module. The public dependency set follows the
# types exposed by Fib.h; implementation-only helpers stay private so leaf
# consumers do not inherit unnecessary link edges.
macro(openr_add_fib_library)
  # Buck2 target: //openr/fib:fib
  openr_add_library(
    NAME openr_fib
    SOURCES ${OPENR_FIB_SOURCES}
    PRIVATE_DEPENDENCIES
      fb303::fb303
      openr_constants
      openr_lsdb_util
      openr_network_util
      openr_profiler
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_common
      openr_config
      openr_rib_entry
      openr_decision_structs
      openr_messaging
      openr_log_sample
      platform_cpp2
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::fib ALIAS openr_fib)
endmacro()
