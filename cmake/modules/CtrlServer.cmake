# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Source owned by the Buck //openr/ctrl-server:openr-ctrl-handler target.
set(OPENR_CTRL_HANDLER_SOURCES
  openr/ctrl-server/OpenrCtrlHandler.cpp
)

set(OPENR_CTRL_SERVER_EXPECTED_SOURCE_COUNT 1)

# Create the Thrift control-plane handler after all module implementations
# it dispatches to are available. The broad public dependency surface
# follows the module pointers and generated service bases in its header.
macro(openr_add_ctrl_server_library)
  # Buck2 target: //openr/ctrl-server:openr-ctrl-handler
  openr_add_library(
    NAME openr_ctrl_handler
    SOURCES ${OPENR_CTRL_HANDLER_SOURCES}
    PRIVATE_DEPENDENCIES
      fmt::fmt
      ${RE2}
      openr_constants
      openr_lsdb_util
      openr_profiler
      openr_util
      openr_kvstore_util
      openr_log_sample
      glog::glog
    PUBLIC_DEPENDENCIES
      fb303::fb303
      openr_common
      openr_config
      openr_persistent_store
      openr_decision
      openr_dispatcher
      openr_fib
      openr_kvstore
      openr_link_monitor
      openr_monitor
      openr_prefix_manager
      openr_spark
      openr_ctrl_cpp2
      openr_ctrl_cpp_cpp2
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::ctrl_handler ALIAS openr_ctrl_handler)
endmacro()
