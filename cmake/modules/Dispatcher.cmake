# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sources owned by the Buck dispatcher_queue and dispatcher targets.
set(OPENR_DISPATCHER_QUEUE_SOURCES openr/dispatcher/DispatcherQueue.cpp)
set(OPENR_DISPATCHER_SOURCES openr/dispatcher/Dispatcher.cpp)

set(OPENR_DISPATCHER_EXPECTED_SOURCE_COUNT 2)

# Create the Dispatcher libraries from the queue layer upward.
macro(openr_add_dispatcher_libraries)
  # Buck2 target: //openr/dispatcher:dispatcher_queue
  openr_add_library(
    NAME openr_dispatcher_queue
    SOURCES ${OPENR_DISPATCHER_QUEUE_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_util
      kv_store_cpp2
      Folly::folly
    PUBLIC_DEPENDENCIES
      openr_common
      openr_messaging
  )
  add_library(OpenR::dispatcher_queue ALIAS openr_dispatcher_queue)

  # Buck2 target: //openr/dispatcher:dispatcher
  openr_add_library(
    NAME openr_dispatcher
    SOURCES ${OPENR_DISPATCHER_SOURCES}
    PRIVATE_DEPENDENCIES
      kv_store_cpp2
      Folly::folly
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_dispatcher_queue
      openr_common
      openr_config
  )
  add_library(OpenR::dispatcher ALIAS openr_dispatcher)
endmacro()
