# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# OSS implementation of the Buck //openr/plugin:plugin API.
set(OPENR_PLUGIN_SOURCES openr/plugin/Plugin.cpp)

set(OPENR_PLUGIN_EXPECTED_SOURCE_COUNT 1)

# Create the OSS no-op plugin adapter. Internal builds provide VIP plugin
# hooks separately, so the OSS and internal implementations must not be
# linked together.
macro(openr_add_plugin_library)
  # Buck2 API target: //openr/plugin:plugin
  openr_add_library(
    NAME openr_plugin
    SOURCES ${OPENR_PLUGIN_SOURCES}
    PUBLIC_DEPENDENCIES
      openr_lsdb_util
      openr_config
      openr_decision_structs
      openr_messaging
      types_cpp2
      FBThrift::thriftcpp2
      wangle::wangle_ssl_ssl_context_config
  )
  add_library(OpenR::plugin ALIAS openr_plugin)
endmacro()
