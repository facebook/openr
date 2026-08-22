# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# These source groups mirror //openr/common:Constants and
# //openr/common:common. Higher-level common-directory targets such as
# lsdb_util and main_util remain separate because they depend on Decision,
# Fib, Policy, and Watchdog.
set(
  OPENR_CONSTANTS_SOURCES
  openr/common/Constants.cpp
)

set(
  OPENR_COMMON_SOURCES
  openr/common/AsyncThrottle.cpp
  openr/common/ExponentialBackoff.cpp
  openr/common/OpenrEventBase.cpp
  openr/common/Types.cpp
)

# Keep this independent from the lists so ownership changes require an
# explicit review of both the source group and its expected size.
set(OPENR_COMMON_EXPECTED_SOURCE_COUNT 5)

# Create the two Buck-aligned libraries after generated Thrift targets exist.
macro(openr_add_common_libraries)
  # Buck2 target: //openr/common:Constants
  openr_add_library(
    NAME openr_constants
    SOURCES ${OPENR_CONSTANTS_SOURCES}
    PUBLIC_DEPENDENCIES Folly::folly
  )
  add_library(OpenR::constants ALIAS openr_constants)

  # Buck2 target: //openr/common:common
  openr_add_library(
    NAME openr_common
    SOURCES ${OPENR_COMMON_SOURCES}
    PRIVATE_DEPENDENCIES glog::glog
    PUBLIC_DEPENDENCIES
      openr_constants
      kv_store_cpp2
      fmt::fmt
      Folly::folly
      ${RE2}
      ${Boost_LIBRARIES}
  )
  add_library(OpenR::common ALIAS openr_common)
endmacro()
