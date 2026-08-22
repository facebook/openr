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
set(
  OPENR_BUILD_INFO_SOURCES
  openr/common/BuildInfo.cpp
)

set(
  OPENR_FLAGS_SOURCES
  openr/common/Flags.cpp
)

set(
  OPENR_FILE_UTIL_SOURCES
  openr/common/FileUtil.cpp
)

set(
  OPENR_NETWORK_UTIL_SOURCES
  openr/common/NetworkUtil.cpp
)

set(
  OPENR_PROFILER_SOURCES
  openr/common/OpenrProfiler.cpp
)

set(
  OPENR_UTIL_SOURCES
  openr/common/Util.cpp
)

set(OPENR_COMMON_EXPECTED_SOURCE_COUNT 11)

# Create the Buck-aligned common libraries after generated Thrift targets exist.
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

  # The remaining compiled targets in common/ stay separate because their
  # Buck ownership and dependency surfaces are independent. In particular,
  # build_info is distinct from the generated build_info target above.
  # CMake uses the OSS BuildInfo.cpp implementation at this target boundary;
  # Buck uses the corresponding facebook/BuildInfo.cpp implementation.
  # Buck2 target: //openr/common:build_info
  openr_add_library(
    NAME openr_build_info
    SOURCES ${OPENR_BUILD_INFO_SOURCES}
    PRIVATE_DEPENDENCIES build_info fmt::fmt
  )
  add_library(OpenR::build_info ALIAS openr_build_info)

  # Buck2 target: //openr/common:flags
  openr_add_library(
    NAME openr_flags
    SOURCES ${OPENR_FLAGS_SOURCES}
    PUBLIC_DEPENDENCIES gflags
  )
  add_library(OpenR::flags ALIAS openr_flags)

  # Buck2 target: //openr/common:file_util
  openr_add_library(
    NAME openr_file_util
    SOURCES ${OPENR_FILE_UTIL_SOURCES}
    PRIVATE_DEPENDENCIES glog::glog
    PUBLIC_DEPENDENCIES Folly::folly
  )
  add_library(OpenR::file_util ALIAS openr_file_util)

  # MplsUtil is header-only, so its INTERFACE target carries generated
  # Thrift ordering and link requirements without adding an archive.
  add_library(openr_mpls_util INTERFACE)
  target_compile_features(openr_mpls_util INTERFACE cxx_std_20)
  target_link_libraries(
    openr_mpls_util
    INTERFACE
      network_cpp2
      glog::glog
  )
  add_library(OpenR::mpls_util ALIAS openr_mpls_util)

  # Buck2 target: //openr/common:network_util
  openr_add_library(
    NAME openr_network_util
    SOURCES ${OPENR_NETWORK_UTIL_SOURCES}
    PUBLIC_DEPENDENCIES
      openr_constants
      network_cpp2
      openr_ctrl_cpp2
      types_cpp2
      fmt::fmt
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::network_util ALIAS openr_network_util)

  # Buck2 target: //openr/common:openr_profiler
  openr_add_library(
    NAME openr_profiler
    SOURCES ${OPENR_PROFILER_SOURCES}
    PRIVATE_DEPENDENCIES fb303::fb303 glog::glog
    PUBLIC_DEPENDENCIES
      Folly::folly
      gflags
      ${RE2}
  )
  add_library(OpenR::profiler ALIAS openr_profiler)

  # Buck2 target: //openr/common:util
  openr_add_library(
    NAME openr_util
    SOURCES ${OPENR_UTIL_SOURCES}
    PUBLIC_DEPENDENCIES
      openr_constants
      openr_common
      kv_store_cpp2
      fb303::fb303
      Folly::folly
      FBThrift::thriftcpp2
      ${Boost_LIBRARIES}
  )
  add_library(OpenR::util ALIAS openr_util)
endmacro()
