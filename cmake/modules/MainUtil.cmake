# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Source owned by the Buck //openr/common:main_util target.
set(OPENR_MAIN_UTIL_SOURCES
  openr/common/MainUtil.cpp
)

set(OPENR_MAIN_UTIL_EXPECTED_SOURCE_COUNT 1)

# Create the daemon lifecycle helpers after the modules they start and the
# control handler they serve. Public dependencies follow the header's
# templates and server-facing function signatures.
macro(openr_add_main_util_library)
  # Buck2 target: //openr/common:main_util
  openr_add_library(
    NAME openr_main_util
    SOURCES ${OPENR_MAIN_UTIL_SOURCES}
    PUBLIC_DEPENDENCIES
      openr_common
      openr_util
      openr_ctrl_handler
      openr_fib
      openr_watchdog
      Folly::folly
      FBThrift::thriftcpp2
      glog::glog
      Threads::Threads
      wangle::wangle_ssl_ssl_context_config
  )
  add_library(OpenR::main_util ALIAS openr_main_util)
endmacro()
