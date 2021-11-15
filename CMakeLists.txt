# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.2)
project(openr)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Disable all warnings. Warnings can vary across toolchains. We rely on
# internaly configured CI (lint and compile) to raise the warnings we find
# useful
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")

# Optionally build with LTO
option(BUILD_WITH_LTO "BUILD_WITH_LTO" OFF)
if (BUILD_WITH_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto")
endif()

include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_BINARY_DIR})

set(
  CMAKE_MODULE_PATH
  "${CMAKE_SOURCE_DIR}/build/fbcode_builder/CMake"
  "${CMAKE_SOURCE_DIR}"
  ${CMAKE_MODULE_PATH}
)
include(FBThriftCppLibrary)
include(FBGenCMakeBuildInfo)

set(REQ_BOOST_COMPONENTS ${REQ_BOOST_COMPONENTS} system thread context filesystem program_options regex)

find_library(ASYNC async PATHS)
find_package(Boost REQUIRED COMPONENTS ${REQ_BOOST_COMPONENTS})
find_library(CONCURRENCY concurrency PATHS)
find_library(DOUBLE-CONVERSION double-conversion)
find_package(fbzmq REQUIRED)
find_package(folly REQUIRED)
find_package(fb303 CONFIG REQUIRED)
find_package(fizz REQUIRED)
find_package(fmt REQUIRED)
find_package(Gflags REQUIRED)
find_package(Glog REQUIRED)
find_library(RE2 re2)
find_library(SODIUM sodium)
find_package(FBThrift REQUIRED)
find_package(wangle REQUIRED)
find_package(Threads REQUIRED)
find_library(ZMQ zmq)
find_library(ZSTD zstd)
find_library(BENCHMARK follybenchmark PATHS)
find_package(range-v3 REQUIRED)

find_path(RE2_INCLUDE_DIR re2/re2.h)
find_path(ZMQ_INCLUDE_DIR zmq.h)

set(FOLLY_EXCEPTION_TRACER)
if (TARGET Folly::folly_exception_tracer)
  set(FOLLY_EXCEPTION_TRACER Folly::folly_exception_tracer)
endif()


add_compile_definitions(NO_FOLLY_EXCEPTION_TRACER)

include_directories(
  ${Boost_INCLUDE_DIR}
  ${FB303_INCLUDE_DIR}
  ${FBTHRIFT_INCLUDE_DIR}
  ${FOLLY_INCLUDE_DIR}
  ${RE2_INCLUDE_DIR}
  ${ZMQ_INCLUDE_DIR}
)

#
# Build thrift libs
#

SET(OPENR_THRIFT_LIBS)

add_fbthrift_cpp_library(
  bgp_config_cpp2
  openr/if/BgpConfig.thrift
  OPTIONS
    json
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} bgp_config_cpp2)

add_fbthrift_cpp_library(
  routing_policy_cpp2
  configerator/structs/neteng/config/routing_policy.thrift
  OPTIONS
    json
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} routing_policy_cpp2)

add_fbthrift_cpp_library(
  vip_service_config_cpp2
  configerator/structs/neteng/config/vip_service_config.thrift
  OPTIONS
    json
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} vip_service_config_cpp2)

add_fbthrift_cpp_library(
  openr_config_cpp2
  openr/if/OpenrConfig.thrift
  OPTIONS
    json
  DEPENDS
    bgp_config_cpp2
    routing_policy_cpp2
    vip_service_config_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} openr_config_cpp2)

add_fbthrift_cpp_library(
  network_cpp2
  openr/if/Network.thrift
  OPTIONS
    json
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} network_cpp2)

add_fbthrift_cpp_library(
  platform_cpp2
  openr/if/Platform.thrift
  SERVICES
    FibService
    NeighborListenerClientForFibagent
  OPTIONS
    json
  DEPENDS
    fb303::fb303_thrift_cpp
    network_cpp2
    types_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} platform_cpp2)

add_fbthrift_cpp_library(
  kv_store_cpp2
  openr/if/KvStore.thrift
  OPTIONS
    json
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} kv_store_cpp2)

add_fbthrift_cpp_library(
  types_cpp2
  openr/if/Types.thrift
  OPTIONS
    json
  DEPENDS
    network_cpp2
    openr_config_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} types_cpp2)

add_fbthrift_cpp_library(
  openr_ctrl_cpp2
  openr/if/OpenrCtrl.thrift
  SERVICES
    OpenrCtrl
  OPTIONS
    json
  DEPENDS
    openr_config_cpp2
    kv_store_cpp2
    types_cpp2
    network_cpp2
    fb303::fb303_thrift_cpp
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} openr_ctrl_cpp2)

add_fbthrift_cpp_library(
  openr_ctrl_cpp_cpp2
  openr/if/OpenrCtrlCpp.thrift
  SERVICES
    OpenrCtrlCpp
  OPTIONS
    json
    stream
    server_stream
  DEPENDS
    openr_ctrl_cpp2
    kv_store_cpp2
    types_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} openr_ctrl_cpp_cpp2)

add_build_info(build_info)

install(TARGETS
  ${OPENR_THRIFT_LIBS}
  DESTINATION lib
)

add_library(openrlib
  openr/allocators/PrefixAllocator.cpp
  openr/common/AsyncThrottle.cpp
  openr/common/BuildInfo.cpp
  openr/common/Constants.cpp
  openr/common/ExponentialBackoff.cpp
  openr/common/Flags.cpp
  openr/common/NetworkUtil.cpp
  openr/common/OpenrEventBase.cpp
  openr/common/OpenrThriftCtrlServer.cpp
  openr/common/PrependLabelAllocator.cpp
  openr/common/Types.cpp
  openr/common/Util.cpp
  openr/config/Config.cpp
  openr/config-store/PersistentStore.cpp
  openr/config-store/PersistentStoreWrapper.cpp
  openr/ctrl-server/OpenrCtrlHandler.cpp
  openr/decision/Decision.cpp
  openr/decision/LinkState.cpp
  openr/decision/PrefixState.cpp
  openr/decision/RibPolicy.cpp
  openr/decision/SpfSolver.cpp
  openr/decision/SrPolicy.cpp
  openr/decision/tests/DecisionTestUtils.cpp
  openr/decision/tests/RoutingBenchmarkUtils.cpp
  openr/kvstore/Dual.cpp
  openr/fib/Fib.cpp
  openr/kvstore/KvStoreClientInternal.cpp
  openr/kvstore/KvStore.cpp
  openr/kvstore/KvStorePublisher.cpp
  openr/kvstore/KvStoreUtil.cpp
  openr/kvstore/KvStoreWrapper.cpp
  openr/link-monitor/LinkMonitor.cpp
  openr/link-monitor/InterfaceEntry.cpp
  openr/nl/NetlinkAddrMessage.cpp
  openr/nl/NetlinkLinkMessage.cpp
  openr/nl/NetlinkNeighborMessage.cpp
  openr/nl/NetlinkRouteMessage.cpp
  openr/nl/NetlinkRuleMessage.cpp
  openr/nl/NetlinkMessageBase.cpp
  openr/nl/NetlinkProtocolSocket.cpp
  openr/nl/NetlinkTypes.cpp
  openr/monitor/LogSample.cpp
  openr/monitor/Monitor.cpp
  openr/monitor/MonitorBase.cpp
  openr/monitor/SystemMetrics.cpp
  openr/platform/NetlinkFibHandler.cpp
  openr/plugin/Plugin.cpp
  openr/policy/PolicyManager.cpp
  openr/prefix-manager/PrefixManager.cpp
  openr/spark/IoProvider.cpp
  openr/spark/SparkWrapper.cpp
  openr/spark/Spark.cpp
  openr/tests/mocks/MockNetlinkProtocolSocket.cpp
  openr/tests/mocks/PrefixGenerator.cpp
  openr/tests/OpenrThriftServerWrapper.cpp
  openr/tests/utils/Utils.cpp
  openr/watchdog/Watchdog.cpp
)

if (BUILD_SHARED_LIBS)
  set_target_properties(openrlib PROPERTIES VERSION 1.0.0 SOVERSION 1)
endif()

target_link_libraries(openrlib
  build_info
  ${OPENR_THRIFT_LIBS}
  fbzmq::fbzmq
  ${DOUBLE-CONVERSION}
  ${ZMQ}
  glog::glog
  gflags
  FBThrift::thriftcpp2
  fb303::fb303
  ${ASYNC}
  ${CONCURRENCY}
  ${ZSTD}
  Folly::folly
  ${FOLLY_EXCEPTION_TRACER}
  ${SODIUM}
  ${RE2}
  ${Boost_LIBRARIES}
  -lpthread
  -lcrypto
  stdc++fs
  range-v3
)

install(TARGETS
  openrlib
  DESTINATION lib
)

add_executable(openr_bin
  openr/Main.cpp
  openr/common/Flags.cpp
)

target_link_libraries(openr_bin
  openrlib
  ${OPENR_THRIFT_LIBS}
  fbzmq::fbzmq
  ${ZMQ}
  ${GLOG}
  ${GFLAGS}
  ${THRIFT}
  ${ZSTD}
  FBThrift::thriftcpp2
  ${ASYNC}
  ${CONCURRENCY}
  Folly::folly
  ${FOLLY_EXCEPTION_TRACER}
  ${SODIUM}
  ${Boost_LIBRARIES}
  -lpthread
  -lcrypto
)

install(TARGETS
  openr_bin
  DESTINATION sbin
  RENAME openr
)

option(BUILD_TOOLS "BUILD_TOOLS" ON)

if(BUILD_TOOLS)
  add_executable(openr_kvstore_snooper
    openr/kvstore/tools/KvStoreSnooper.cpp
  )

  target_link_libraries(openr_kvstore_snooper
    openrlib
    ${GLOG}
    ${GFLAGS}
    ${THRIFT}
    ${ZSTD}
    ${THRIFTCPP2}
    ${ASYNC}
    ${TRANSPORT}
    ${CONCURRENCY}
    ${THRIFTPROTOCOL}
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${SODIUM}
    ${Boost_LIBRARIES}
    -lpthread
    -lcrypto
  )

  install(TARGETS
    openr_kvstore_snooper
    DESTINATION sbin
  )
endif()

add_executable(platform_linux
  openr/platform/LinuxPlatformMain.cpp
  openr/platform/NetlinkFibHandler.cpp
)

target_link_libraries(platform_linux
  openrlib
  ${OPENR_THRIFT_LIBS}
  ${FBZMQ}
  ${GLOG}
  ${GFLAGS}
  Folly::folly
  ${FOLLY_EXCEPTION_TRACER}
  FBThrift::thriftcpp2
  ${ZSTD}
  ${ASYNC}
  ${CONCURRENCY}
  ${SODIUM}
  ${ZMQ}
  ${Boost_LIBRARIES}
  -lpthread
)

install(TARGETS
  platform_linux
  DESTINATION sbin
)

#
# Install files
#

# headers
install(
  DIRECTORY ${CMAKE_SOURCE_DIR}/openr
  DESTINATION include
  FILES_MATCHING PATTERN "*.h"
)
install(
  DIRECTORY ${CMAKE_SOURCE_DIR}/common
  DESTINATION include
  FILES_MATCHING PATTERN "*.h"
)

# thrift files
install(
  DIRECTORY ${CMAKE_SOURCE_DIR}/openr
  DESTINATION include
  FILES_MATCHING PATTERN "*.thrift"
)
install(
  DIRECTORY ${CMAKE_SOURCE_DIR}/common
  DESTINATION include
  FILES_MATCHING PATTERN "*.thrift"
)

#
# Tests
#

option(BUILD_TESTS "BUILD_TESTS" ON)
option(ADD_ROOT_TESTS "ADD_ROOT_TESTS" ON)

if(BUILD_TESTS)

  enable_testing()

  find_package(GMock MODULE REQUIRED)
  find_package(GTest MODULE REQUIRED)

  include_directories(
    ${GTEST_INCLUDE_DIRS}
    ${GMOCK_INCLUDE_DIRS}
  )

  # function that adds tests, typical usage example:
  # add_openr_test(TestName test_bin_name
  #   SOURCES Feature1Test.cpp Feature2Test.cpp
  #   LIBRARIES additional_library1 -lcrypto
  #   DESTINATION folder/where/to/place/binary
  # )
  function(add_openr_test TEST_NAME BIN_NAME)
    set(one_value_args DESTINATION)
    set(multi_value_args SOURCES LIBRARIES)
    fb_cmake_parse_args(
      ARG
      ""
      "${one_value_args}"
      "${multi_value_args}"
      "${ARGN}"
    )

    add_executable(
      ${BIN_NAME}
      ${ARG_SOURCES}
    )
    target_link_libraries(${BIN_NAME}
      openrlib
      ${OPENR_THRIFT_LIBS}
      ${GTEST_BOTH_LIBRARIES}
      ${LIBGMOCK_LIBRARIES}
      ${ARG_LIBRARIES}
    )
    add_test(${TEST_NAME} ${BIN_NAME})
    if(NOT "${ARG_DESTINATION}" STREQUAL "")
      install(TARGETS
        ${BIN_NAME}
        DESTINATION ${ARG_DESTINATION}
      )
    endif()
  endfunction()

  add_openr_test(OpenrSystemTest openr_system_test
    SOURCES
      openr/tests/OpenrSystemTest.cpp
      openr/tests/OpenrWrapper.cpp
      openr/tests/mocks/NetlinkEventsInjector.cpp
      openr/tests/mocks/MockIoProvider.cpp
      openr/tests/mocks/MockIoProviderUtils.cpp
    LIBRARIES
      fbzmq::fbzmq
      ${ZMQ}
      ${GLOG}
      ${GFLAGS}
      FBThrift::thriftcpp2
      ${ZSTD}
      Folly::folly
      ${FOLLY_EXCEPTION_TRACER}
      ${SODIUM}
      ${Boost_LIBRARIES}
      -lpthread
      -lcrypto
    DESTINATION sbin/tests/openr
  )

  add_openr_test(PrefixAllocatorTest prefix_allocator_test
    SOURCES
      openr/allocators/tests/PrefixAllocatorTest.cpp
      openr/tests/mocks/NetlinkEventsInjector.cpp
    DESTINATION sbin/tests/openr/allocators
  )

  add_openr_test(RangeAllocatorTest range_allocator_test
    SOURCES
      openr/allocators/tests/RangeAllocatorTest.cpp
    DESTINATION sbin/tests/openr/allocators
  )

  add_openr_test(OpenrCtrlHandlerTest openr_ctrl_handler_test
    SOURCES
      openr/ctrl-server/tests/OpenrCtrlHandlerTest.cpp
      openr/tests/mocks/NetlinkEventsInjector.cpp
    DESTINATION sbin/tests/openr/ctrl-server
  )

  add_openr_test(OpenrCtrlLongPollTest openr_ctrl_longpoll_test
    SOURCES
      openr/ctrl-server/tests/OpenrCtrlLongPollTest.cpp
    DESTINATION sbin/tests/openr/ctrl-server
  )

  add_openr_test(AsyncDebounceTest async_debounce_test
    SOURCES
      openr/common/tests/AsyncDebounceTest.cpp
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(AsyncThrottleTest async_throttle_test
    SOURCES
      openr/common/tests/AsyncThrottleTest.cpp
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(ExponentialBackoffTest exp_backoff_test
    SOURCES
      openr/common/tests/ExponentialBackoffTest.cpp
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(OpenrEventBaseTest openr_event_base_test
    SOURCES
      openr/common/tests/OpenrEventBaseTest.cpp
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(UtilTest util_test
    SOURCES
      openr/common/tests/UtilTest.cpp
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(PersistentStoreTest config_store_test
    SOURCES
      openr/config-store/tests/PersistentStoreTest.cpp
    DESTINATION sbin/tests/openr/config-store
  )

  add_openr_test(DecisionTest decision_test
    SOURCES
      openr/decision/tests/DecisionTest.cpp
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(DualTest dual_test
    SOURCES
      openr/kvstore/tests/DualTest.cpp
    DESTINATION sbin/tests/openr/kvstore
  )

  add_openr_test(LinkStateTest link_state_test
    SOURCES
      openr/decision/tests/LinkStateTest.cpp
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(PrefixStateTest prefix_state_test
    SOURCES
      openr/decision/tests/PrefixStateTest.cpp
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(RibPolicyTest rib_policy_test
    SOURCES
      openr/decision/tests/RibPolicyTest.cpp
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(RibEntryTest rib_entry_test
    SOURCES
      openr/decision/tests/RibEntryTest.cpp
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(KvStoreTest kvstore_test
    SOURCES
      openr/kvstore/tests/KvStoreTest.cpp
    DESTINATION sbin/tests/openr/kvstore
  )

  add_openr_test(KvStoreTtlTest kvstore_ttl_test
    SOURCES
      openr/kvstore/tests/KvStoreTtlTest.cpp
    DESTINATION sbin/tests/openr/kvstore
  )

  add_openr_test(KvStoreSelfOriginatedKeyTest kvstore_self_originated_key_test
    SOURCES
      openr/kvstore/tests/KvStoreSelfOriginatedKeyTest.cpp
    DESTINATION sbin/tests/openr/kvstore
  )

  add_openr_test(KvStoreClientInternalTest kvstore_client_internal_test
    SOURCES
      openr/kvstore/tests/KvStoreClientInternalTest.cpp
    DESTINATION sbin/tests/openr/kvstore
  )

  add_openr_test(KvStoreUtilTest kvstore_util_test
    SOURCES
      openr/kvstore/tests/KvStoreUtilTest.cpp
    DESTINATION sbin/tests/openr/kvstore
  )

 add_openr_test(LinkMonitorTest link_monitor_test
    SOURCES
      openr/link-monitor/tests/LinkMonitorTest.cpp
      openr/tests/mocks/NetlinkEventsInjector.cpp
    DESTINATION sbin/tests/openr/link-monitor
  )

  if(ADD_ROOT_TESTS)
    # This test fails under Travis, so adding it as an exception
    add_openr_test(FibTest fib_test
      SOURCES
        openr/fib/tests/FibTest.cpp
        openr/tests/mocks/MockNetlinkFibHandler.cpp
      DESTINATION sbin/tests/openr/fib
    )
  endif()

  add_openr_test(NetlinkTypesTest netlink_types_test
    SOURCES
      openr/nl/tests/NetlinkTypesTest.cpp
    DESTINATION sbin/tests/openr/nl
  )

  if(ADD_ROOT_TESTS)
    # these tests must be run by root user
    add_openr_test(NetlinkProtocolSocketTest netlink_message_test
      SOURCES
        openr/nl/tests/NetlinkProtocolSocketTest.cpp
      DESTINATION sbin/tests/openr/nl
    )
  endif()

  add_openr_test(PrefixManagerTest prefix_manager_test
    SOURCES
      openr/prefix-manager/tests/PrefixManagerTest.cpp
    DESTINATION sbin/tests/openr/prefix-manager
  )

  add_openr_test(SparkTest spark_test
    SOURCES
      openr/spark/tests/SparkTest.cpp
      openr/tests/mocks/MockIoProvider.cpp
      openr/tests/mocks/MockIoProviderUtils.cpp
    DESTINATION sbin/tests/openr/spark
  )

  add_openr_test(MockIoProviderTest mock_io_provider_test
    SOURCES
      openr/tests/MockIoProviderTest.cpp
      openr/tests/mocks/MockIoProvider.cpp
      openr/tests/mocks/MockIoProviderUtils.cpp
    DESTINATION sbin/tests/openr/spark
  )

  add_openr_test(MessagingQueueTest queue_test
    SOURCES
      openr/messaging/tests/QueueTest.cpp
    LIBRARIES
      Folly::folly
    DESTINATION sbin/tests/openr/messaging
  )

  add_openr_test(MessagingReplicateQueueTest replicate_queue_test
    SOURCES
      openr/messaging/tests/ReplicateQueueTest.cpp
    LIBRARIES
      Folly::folly
    DESTINATION sbin/tests/openr/messaging
  )

  add_openr_test(NetlinkFibHandlerTest netlink_fib_handler_test
    SOURCES
      openr/platform/tests/NetlinkFibHandlerTest.cpp
    DESTINATION sbin/tests/openr/platform
  )

  add_openr_test(WatchdogTest watchdog_test
    SOURCES
      openr/watchdog/tests/WatchdogTest.cpp
    DESTINATION sbin/tests/openr/watchdog
  )

  #
  # benchmarks
  #

  add_executable(config_store_benchmark
    openr/config-store/tests/PersistentStoreBenchmark.cpp
  )

  target_link_libraries(config_store_benchmark
    openrlib
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${BENCHMARK}
  )

  install(TARGETS
    config_store_benchmark
    DESTINATION sbin/tests/openr/config-store
  )

  add_executable(fib_benchmark
    openr/fib/tests/FibBenchmark.cpp
    openr/tests/mocks/MockNetlinkFibHandler.cpp
  )

  target_link_libraries(fib_benchmark
    openrlib
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST_BOTH_LIBRARIES}
    ${GTEST_MAIN}
    ${THRIFTCPP2}
    ${BENCHMARK}
  )

  install(TARGETS
    fib_benchmark
    DESTINATION sbin/tests/openr/fib
  )

  add_executable(netlink_fib_handler_benchmark
    openr/platform/tests/NetlinkFibHandlerBenchmark.cpp
  )

  target_link_libraries(netlink_fib_handler_benchmark
    openrlib
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
    ${BENCHMARK}
  )

  install(TARGETS
    netlink_fib_handler_benchmark
    DESTINATION sbin/tests/openr/platform
  )

  add_executable(decision_benchmark
    openr/decision/tests/DecisionBenchmark.cpp
  )

  target_link_libraries(decision_benchmark
    openrlib
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST_BOTH_LIBRARIES}
    ${GTEST_MAIN}
    ${THRIFTCPP2}
    ${BENCHMARK}
  )

  install(TARGETS
    decision_benchmark
    DESTINATION sbin/tests/openr/decision
  )

  add_executable(kvstore_benchmark
    openr/kvstore/tests/KvStoreBenchmark.cpp
  )

  target_link_libraries(kvstore_benchmark
    openrlib
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${BENCHMARK}
  )

  install(TARGETS
    kvstore_benchmark
    DESTINATION sbin/tests/openr/kvstore
  )

endif()
