# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Source owned by the Buck //openr/platform:netlink-fib-handler target.
set(OPENR_NETLINK_FIB_HANDLER_SOURCES
  openr/platform/NetlinkFibHandler.cpp
)

set(OPENR_PLATFORM_EXPECTED_SOURCE_COUNT 1)

# Create the Linux netlink FIB service implementation. Its public API
# exposes the generated Platform service, Open/R route types, and the raw
# netlink adapter, so those dependencies propagate to direct consumers.
macro(openr_add_platform_library)
  # Buck2 target: //openr/platform:netlink-fib-handler
  openr_add_library(
    NAME openr_netlink_fib_handler
    SOURCES ${OPENR_NETLINK_FIB_HANDLER_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_lsdb_util
      glog::glog
    PUBLIC_DEPENDENCIES
      fb303::fb303
      openr_mpls_util
      openr_network_util
      openr_fbnl
      platform_cpp2
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::netlink_fib_handler ALIAS openr_netlink_fib_handler)
endmacro()
