# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sources owned by the Buck //openr/nl:fbnl target.
set(
  OPENR_FBNL_SOURCES
  openr/nl/NetlinkAddrMessage.cpp
  openr/nl/NetlinkLinkMessage.cpp
  openr/nl/NetlinkMessageBase.cpp
  openr/nl/NetlinkNeighborMessage.cpp
  openr/nl/NetlinkProtocolSocket.cpp
  openr/nl/NetlinkRouteMessage.cpp
  openr/nl/NetlinkRuleMessage.cpp
  openr/nl/NetlinkTypes.cpp
)

set(OPENR_NETLINK_EXPECTED_SOURCE_COUNT 8)

# Create the Linux Netlink library after generated Thrift targets exist.
macro(openr_add_netlink_library)
  # Buck2 target: //openr/nl:fbnl
  openr_add_library(
    NAME openr_fbnl
    SOURCES ${OPENR_FBNL_SOURCES}
    PRIVATE_DEPENDENCIES fb303::fb303
    PUBLIC_DEPENDENCIES
      openr_messaging
      network_cpp2
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
      glog::glog
  )
  add_library(OpenR::fbnl ALIAS openr_fbnl)
endmacro()
