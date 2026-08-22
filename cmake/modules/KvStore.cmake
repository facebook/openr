# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sources corresponding to the compiled Buck targets in openr/kvstore.
set(OPENR_DUAL_SOURCES openr/kvstore/Dual.cpp)
set(OPENR_KVSTORE_SOURCES openr/kvstore/KvStorePublisher.cpp)
set(OPENR_KVSTORE_UTIL_SOURCES openr/kvstore/KvStoreUtil.cpp)
set(OPENR_KVSTORE_WRAPPER_SOURCES openr/kvstore/KvStoreWrapper.cpp)

set(OPENR_KVSTORE_EXPECTED_SOURCE_COUNT 4)

# Create KvStore's header-only and compiled targets in dependency order.
#
# KvStoreWrapper is test infrastructure and deliberately remains separate
# from the core KvStore target, matching the Buck dependency direction.
macro(openr_add_kvstore_libraries)
  add_library(openr_client_util INTERFACE)
  target_link_libraries(
    openr_client_util
    INTERFACE
      openr_constants
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::client_util ALIAS openr_client_util)

  # Buck2 target: //openr/kvstore:dual
  openr_add_library(
    NAME openr_dual
    SOURCES ${OPENR_DUAL_SOURCES}
    PRIVATE_DEPENDENCIES glog::glog
    PUBLIC_DEPENDENCIES
      dual_cpp2
      Folly::folly
  )
  add_library(OpenR::dual ALIAS openr_dual)

  # Buck2 target: //openr/kvstore:kvstore-util
  openr_add_library(
    NAME openr_kvstore_util
    SOURCES ${OPENR_KVSTORE_UTIL_SOURCES}
    PRIVATE_DEPENDENCIES
      fb303::fb303
      ${RE2}
    PUBLIC_DEPENDENCIES
      openr_constants
      openr_common
      openr_client_util
      openr_util
      openr_config
      kv_store_cpp2
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::kvstore_util ALIAS openr_kvstore_util)

  add_library(openr_kvstore_params INTERFACE)
  target_link_libraries(
    openr_kvstore_params
    INTERFACE
      openr_kvstore_util
      openr_config
      openr_messaging
      openr_log_sample
  )
  add_library(OpenR::kvstore_params ALIAS openr_kvstore_params)

  # Buck2 target: //openr/kvstore:kvstore
  openr_add_library(
    NAME openr_kvstore
    SOURCES ${OPENR_KVSTORE_SOURCES}
    PRIVATE_DEPENDENCIES openr_util
    PUBLIC_DEPENDENCIES
      openr_kvstore_util
      openr_kvstore_params
      openr_constants
      openr_common
      openr_lsdb_util
      openr_client_util
      openr_profiler
      openr_messaging
      kv_store_cpp2
      fb303::fb303
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::kvstore ALIAS openr_kvstore)

  add_library(openr_kvstore_service_handler INTERFACE)
  target_link_libraries(
    openr_kvstore_service_handler
    INTERFACE
      openr_kvstore
      kv_store_cpp2
      fb303::fb303
      FBThrift::thriftcpp2
  )
  add_library(
    OpenR::kvstore_service_handler ALIAS openr_kvstore_service_handler
  )

  # Buck2 target: //openr/kvstore:kvstore-wrapper
  openr_add_library(
    NAME openr_kvstore_wrapper
    SOURCES ${OPENR_KVSTORE_WRAPPER_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_constants
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_kvstore
      openr_kvstore_service_handler
      openr_lsdb_util
      openr_messaging
      openr_log_sample
      kv_store_cpp2
      openr_ctrl_cpp_cpp2
      Folly::folly
      FBThrift::thriftcpp2
      wangle::wangle_ssl_ssl_context_config
  )
  add_library(OpenR::kvstore_wrapper ALIAS openr_kvstore_wrapper)
endmacro()
