# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sources owned by the Buck //openr/config:config target.
set(
  OPENR_CONFIG_SOURCES
  openr/config/Config.cpp
)

set(OPENR_CONFIG_EXPECTED_SOURCE_COUNT 1)

# Create the Config library after generated Thrift targets exist.
macro(openr_add_config_library)
  # Buck2 target: //openr/config:config
  openr_add_library(
    NAME openr_config
    SOURCES ${OPENR_CONFIG_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_constants
      fb303::fb303
      fmt::fmt
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_file_util
      openr_mpls_util
      kv_store_cpp2
      openr_config_cpp2
      Folly::folly
      FBThrift::thriftcpp2
      ${RE2}
      stdc++fs
  )
  add_library(OpenR::config ALIAS openr_config)
endmacro()
