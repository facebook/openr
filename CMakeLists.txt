# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.13)
project(openr)

set(CMAKE_CXX_STANDARD 20)
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
find_library(ZSTD zstd)
find_package(Snappy REQUIRED)
find_library(BENCHMARK follybenchmark PATHS)
find_package(range-v3 REQUIRED)
find_package(Xxhash REQUIRED)

find_path(RE2_INCLUDE_DIR re2/re2.h)

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
  ${Xxhash_INCLUDE_DIR}
)

#
# Build thrift libs
#

SET(OPENR_THRIFT_LIBS)

add_fbthrift_cpp_library(
  routing_policy_cpp2
  configerator/structs/neteng/config/routing_policy.thrift
  OPTIONS
    json
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} routing_policy_cpp2)

add_fbthrift_cpp_library(
  openr_config_cpp2
  openr/if/OpenrConfig.thrift
  OPTIONS
    json
  DEPENDS
    routing_policy_cpp2
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
  dual_cpp2
  openr/if/Dual.thrift
  OPTIONS
    json
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} dual_cpp2)

add_fbthrift_cpp_library(
  kv_store_cpp2
  openr/if/KvStore.thrift
  OPTIONS
    json
  DEPENDS
    fb303::fb303_thrift_cpp
    types_cpp2
  SERVICES
    KvStoreService
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

include(${CMAKE_CURRENT_LIST_DIR}/cmake/OpenrLibrary.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Common.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/ConfigStore.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Lsdb.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Policy.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Messaging.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/NeighborMonitor.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Decision.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/PrefixManager.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Spark.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Dispatcher.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Monitor.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Watchdog.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Fib.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/KvStore.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Netlink.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/Platform.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/LinkMonitor.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/CtrlServer.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/MainUtil.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/modules/TestSupport.cmake)
openr_add_common_libraries()
openr_add_config_library()
openr_add_config_store_library()
openr_add_lsdb_library()
openr_add_policy_library()
openr_add_messaging_library()
openr_add_neighbor_monitor_library()
openr_add_decision_libraries()
openr_add_prefix_manager_library()
openr_add_spark_libraries()
openr_add_dispatcher_libraries()
openr_add_monitor_libraries()
openr_add_watchdog_library()
openr_add_fib_library()
openr_add_kvstore_libraries()
openr_add_netlink_library()
openr_add_platform_library()
openr_add_link_monitor_libraries()
openr_add_ctrl_server_library()
openr_add_main_util_library()
openr_add_test_support_libraries()

add_executable(openr_bin
  openr/Main.cpp
)

target_link_libraries(openr_bin
  openr_build_info
  openr_flags
  openr_main_util
  openr_common
  openr_util
  openr_config
  openr_persistent_store
  openr_ctrl_handler
  openr_decision
  openr_decision_structs
  openr_dispatcher
  openr_fib
  openr_kvstore
  openr_link_monitor
  openr_messaging
  openr_monitor
  openr_neighbor_monitor
  openr_fbnl
  openr_netlink_fib_handler
  openr_prefix_manager
  openr_io_provider
  openr_spark
  openr_watchdog
  ${OPENR_THRIFT_LIBS}
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
    openr_client_util
    openr_kvstore
    openr_kvstore_util
    openr_ctrl_cpp_cpp2
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
)

target_link_libraries(platform_linux
  openr_netlink_fib_handler
  openr_common
  openr_fbnl
  glog::glog
  gflags
  Folly::folly
  FBThrift::thriftcpp2
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

  openr_add_install_contract_test()

  # Validate source-list integrity separately from compilation so duplicate,
  # missing, or stale entries fail with a focused diagnostic.
  openr_add_source_group_test(
    NAME OpenrCommonSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Common.cmake
    SOURCE_VARIABLES
      OPENR_CONSTANTS_SOURCES
      OPENR_COMMON_SOURCES
      OPENR_BUILD_INFO_SOURCES
      OPENR_FLAGS_SOURCES
      OPENR_FILE_UTIL_SOURCES
      OPENR_NETWORK_UTIL_SOURCES
      OPENR_PROFILER_SOURCES
      OPENR_UTIL_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_COMMON_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrConfigSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Config.cmake
    SOURCE_VARIABLES OPENR_CONFIG_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_CONFIG_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrConfigStoreSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/ConfigStore.cmake
    SOURCE_VARIABLES OPENR_PERSISTENT_STORE_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_CONFIG_STORE_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrLsdbSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Lsdb.cmake
    SOURCE_VARIABLES OPENR_LSDB_UTIL_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_LSDB_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrPolicySourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Policy.cmake
    SOURCE_VARIABLES OPENR_POLICY_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_POLICY_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrNeighborMonitorSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/NeighborMonitor.cmake
    SOURCE_VARIABLES OPENR_NEIGHBOR_MONITOR_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_NEIGHBOR_MONITOR_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrDispatcherSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Dispatcher.cmake
    SOURCE_VARIABLES
      OPENR_DISPATCHER_QUEUE_SOURCES
      OPENR_DISPATCHER_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_DISPATCHER_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrDecisionSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Decision.cmake
    SOURCE_VARIABLES
      OPENR_LINK_SOURCES
      OPENR_DECISION_STRUCTS_SOURCES
      OPENR_FABRIC_HELPER_SOURCES
      OPENR_SPF_SOLVER_SOURCES
      OPENR_DECISION_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_DECISION_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrPrefixManagerSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/PrefixManager.cmake
    SOURCE_VARIABLES OPENR_PREFIX_MANAGER_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_PREFIX_MANAGER_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrSparkSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Spark.cmake
    SOURCE_VARIABLES
      OPENR_IO_PROVIDER_SOURCES
      OPENR_SPARK_SOURCES
      OPENR_SPARK_WRAPPER_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_SPARK_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrMonitorSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Monitor.cmake
    SOURCE_VARIABLES
      OPENR_LOG_SAMPLE_SOURCES
      OPENR_SYSTEM_METRICS_SOURCES
      OPENR_MONITOR_BASE_SOURCES
      OPENR_MONITOR_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_MONITOR_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrWatchdogSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Watchdog.cmake
    SOURCE_VARIABLES OPENR_WATCHDOG_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_WATCHDOG_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrFibSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Fib.cmake
    SOURCE_VARIABLES OPENR_FIB_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_FIB_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrKvStoreSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/KvStore.cmake
    SOURCE_VARIABLES
      OPENR_KVSTORE_SOURCES
      OPENR_KVSTORE_UTIL_SOURCES
      OPENR_KVSTORE_WRAPPER_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_KVSTORE_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrNetlinkSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Netlink.cmake
    SOURCE_VARIABLES OPENR_FBNL_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_NETLINK_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrPlatformSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/Platform.cmake
    SOURCE_VARIABLES OPENR_NETLINK_FIB_HANDLER_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_PLATFORM_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrLinkMonitorSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/LinkMonitor.cmake
    SOURCE_VARIABLES
      OPENR_ADJACENCY_ENTRY_SOURCES
      OPENR_LINK_MONITOR_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_LINK_MONITOR_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrCtrlServerSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/CtrlServer.cmake
    SOURCE_VARIABLES OPENR_CTRL_HANDLER_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_CTRL_SERVER_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrMainUtilSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/MainUtil.cmake
    SOURCE_VARIABLES OPENR_MAIN_UTIL_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_MAIN_UTIL_EXPECTED_SOURCE_COUNT}
  )

  openr_add_source_group_test(
    NAME OpenrTestSupportSourceGroupTest
    SOURCE_GROUP_FILE cmake/modules/TestSupport.cmake
    SOURCE_VARIABLES
      OPENR_PREFIX_GENERATOR_SOURCES
      OPENR_MOCK_NETLINK_PROTOCOL_SOCKET_SOURCES
      OPENR_TEST_UTILS_SOURCES
      OPENR_PERSISTENT_STORE_WRAPPER_SOURCES
      OPENR_DECISION_TEST_UTILS_SOURCES
      OPENR_ROUTING_BENCHMARK_UTILS_SOURCES
    EXPECTED_SOURCE_COUNT ${OPENR_TEST_SUPPORT_EXPECTED_SOURCE_COUNT}
  )

  set(
    OPENR_COMMON_TEST_LIBRARIES
    openr_common
    glog::glog
    gflags
  )

  add_openr_test(OpenrSystemTest openr_system_test
    SOURCES
      openr/tests/OpenrSystemTest.cpp
      openr/tests/OpenrWrapper.cpp
      openr/tests/mocks/NetlinkEventsInjector.cpp
      openr/tests/mocks/MockIoProvider.cpp
      openr/tests/mocks/MockIoProviderUtils.cpp
    LIBRARIES
      openr_test_utils
      openr_mock_netlink_protocol_socket
      openr_config
      openr_lsdb_util
      openr_decision
      openr_fib
      openr_kvstore
      openr_link_monitor
      openr_monitor
      openr_prefix_manager
      openr_spark
      openr_watchdog
      openr_io_provider
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

  add_openr_test(OpenrCtrlHandlerTest openr_ctrl_handler_test
    SOURCES
      openr/ctrl-server/tests/OpenrCtrlHandlerTest.cpp
      openr/tests/mocks/NetlinkEventsInjector.cpp
    LIBRARIES
      openr_ctrl_handler
      openr_persistent_store
      openr_kvstore_wrapper
      openr_decision
      openr_fib
      openr_link_monitor
      openr_prefix_manager
      openr_config
      openr_common
      openr_util
      openr_messaging
      openr_test_utils
      openr_mock_netlink_protocol_socket
    DESTINATION sbin/tests/openr/ctrl-server
  )

  add_openr_test(OpenrCtrlLongPollTest openr_ctrl_longpoll_test
    SOURCES
      openr/ctrl-server/tests/OpenrCtrlLongPollTest.cpp
    LIBRARIES
      openr_ctrl_handler
      openr_kvstore_wrapper
      openr_test_utils
    DESTINATION sbin/tests/openr/ctrl-server
  )

  add_openr_test(AsyncDebounceTest async_debounce_test
    SOURCES
      openr/common/tests/AsyncDebounceTest.cpp
    LIBRARIES ${OPENR_COMMON_TEST_LIBRARIES}
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(AsyncThrottleTest async_throttle_test
    SOURCES
      openr/common/tests/AsyncThrottleTest.cpp
    LIBRARIES ${OPENR_COMMON_TEST_LIBRARIES}
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(ExponentialBackoffTest exp_backoff_test
    SOURCES
      openr/common/tests/ExponentialBackoffTest.cpp
    LIBRARIES ${OPENR_COMMON_TEST_LIBRARIES}
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(OpenrEventBaseTest openr_event_base_test
    SOURCES
      openr/common/tests/OpenrEventBaseTest.cpp
    LIBRARIES ${OPENR_COMMON_TEST_LIBRARIES}
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(UtilTest util_test
    SOURCES
      openr/common/tests/UtilTest.cpp
    LIBRARIES
      openr_lsdb_util
      openr_mpls_util
      openr_util
      kv_store_cpp2
      types_cpp2
      fb303::fb303
      glog::glog
      gflags
      Folly::folly
      FBThrift::thriftcpp2
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(TypesTest types_test
    SOURCES
      openr/common/tests/TypesTest.cpp
    LIBRARIES
      openr_lsdb_util
      glog::glog
      gflags
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(FileUtilTest file_util_test
    SOURCES
      openr/common/tests/FileUtilTest.cpp
    LIBRARIES
      openr_file_util
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(OpenrProfilerTest openr_profiler_test
    SOURCES
      openr/common/tests/OpenrProfilerTest.cpp
    LIBRARIES
      openr_profiler
      fmt::fmt
      glog::glog
    DESTINATION sbin/tests/openr/common
  )

  add_openr_test(LogSampleTest log_sample_test
    SOURCES
      openr/monitor/tests/LogSampleTest.cpp
    LIBRARIES
      openr_log_sample
      glog::glog
      gflags
    DESTINATION sbin/tests/openr/monitor
  )

  add_openr_test(SystemMetricsTest system_metrics_test
    SOURCES
      openr/monitor/tests/SystemMetricsTest.cpp
    LIBRARIES
      openr_system_metrics
      glog::glog
    DESTINATION sbin/tests/openr/monitor
  )

  add_openr_test(MonitorBaseTest monitor_base_test
    SOURCES
      openr/monitor/tests/MonitorBaseTest.cpp
    LIBRARIES
      openr_constants
      openr_config
      openr_monitor_base
      glog::glog
    DESTINATION sbin/tests/openr/monitor
  )

  add_openr_test(PersistentStoreTest config_store_test
    SOURCES
      openr/config-store/tests/PersistentStoreTest.cpp
    LIBRARIES
      openr_persistent_store_wrapper
      glog::glog
      gflags
    DESTINATION sbin/tests/openr/config-store
  )

  add_openr_test(DecisionTest decision_test
    SOURCES
      openr/decision/tests/DecisionTest.cpp
    LIBRARIES
      openr_decision_test_utils
      openr_test_utils
      openr_decision
      openr_decision_structs
      openr_network_util
      openr_util
      openr_flags
      openr_mpls_util
      openr_constants
      openr_config_cpp2
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(DispatcherTest dispatcher_test
    SOURCES
      openr/dispatcher/tests/DispatcherTest.cpp
    LIBRARIES
      openr_dispatcher
      openr_dispatcher_queue
      openr_kvstore_util
      openr_test_utils
      openr_lsdb_util
      openr_common
      openr_util
      openr_messaging
    DESTINATION sbin/tests/openr/dispatcher
  )

  add_openr_test(DispatcherQueueTest dispatcher_queue_test
    SOURCES
      openr/dispatcher/tests/DispatcherQueueTest.cpp
    LIBRARIES
      openr_dispatcher_queue
      openr_test_utils
      openr_common
      openr_util
    DESTINATION sbin/tests/openr/dispatcher
  )

  add_openr_test(KvStorePublisherTest kvstore_publisher_test
    SOURCES
      openr/kvstore/tests/KvStorePublisherTest.cpp
    LIBRARIES
      openr_kvstore
      openr_util
      glog::glog
      gflags
      Folly::folly
    DESTINATION sbin/tests/openr/kvstore
  )

  add_openr_test(LinkStateTest link_state_test
    SOURCES
      openr/decision/tests/LinkStateTest.cpp
    LIBRARIES
      openr_decision_test_utils
      openr_test_utils
      openr_spf_solver
      openr_network_util
      openr_util
      openr_constants
      openr_config_cpp2
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(PrefixStateTest prefix_state_test
    SOURCES
      openr/decision/tests/PrefixStateTest.cpp
    LIBRARIES
      openr_spf_solver
      openr_lsdb_util
      openr_network_util
      glog::glog
      Folly::folly
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(RibPolicyTest rib_policy_test
    SOURCES
      openr/decision/tests/RibPolicyTest.cpp
    LIBRARIES
      openr_decision_structs
      openr_lsdb_util
      fb303::fb303
      Folly::folly
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(RibEntryTest rib_entry_test
    SOURCES
      openr/decision/tests/RibEntryTest.cpp
    LIBRARIES
      openr_rib_entry
      openr_lsdb_util
      Folly::folly
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(LinkTest link_test
    SOURCES
      openr/decision/tests/LinkTest.cpp
    LIBRARIES
      openr_link
      openr_lsdb_util
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(RouteUpdateTest route_update_test
    SOURCES
      openr/decision/tests/RouteUpdateTest.cpp
    LIBRARIES
      openr_decision_structs
      openr_lsdb_util
      Folly::folly
    DESTINATION sbin/tests/openr/decision
  )

  add_openr_test(FabricHelperTest fabric_helper_test
    SOURCES
      openr/decision/tests/FabricHelperTest.cpp
    LIBRARIES
      openr_fabric_helper
      openr_spf_solver
      openr_constants
      openr_lsdb_util
      openr_config
      openr_config_cpp2
      Folly::folly
    DESTINATION sbin/tests/openr/decision
  )

  if(ADD_ROOT_TESTS)
    # These tests create multi-node KvStore topologies with thrift peer
    # connections that are unreliable in containerized CI environments.
    add_openr_test(KvStoreTest kvstore_test
      SOURCES
        openr/kvstore/tests/KvStoreTest.cpp
      LIBRARIES
        openr_kvstore_util
        openr_kvstore_wrapper
        openr_config
        openr_util
        fb303::fb303
        Folly::folly
        glog::glog
      DESTINATION sbin/tests/openr/kvstore
    )

    add_openr_test(KvStoreTtlTest kvstore_ttl_test
      SOURCES
        openr/kvstore/tests/KvStoreTtlTest.cpp
      LIBRARIES
        openr_kvstore_wrapper
        Folly::folly
        range-v3
      DESTINATION sbin/tests/openr/kvstore
    )
  endif()

  add_openr_test(KvStoreSelfOriginatedKeyTest kvstore_self_originated_key_test
    SOURCES
      openr/kvstore/tests/KvStoreSelfOriginatedKeyTest.cpp
    LIBRARIES
      openr_kvstore_wrapper
      openr_util
      kv_store_cpp2
      glog::glog
      gflags
      Folly::folly
    DESTINATION sbin/tests/openr/kvstore
  )

  add_openr_test(KvStoreUtilTest kvstore_util_test
    SOURCES
      openr/kvstore/tests/KvStoreUtilTest.cpp
    LIBRARIES
      openr_client_util
      openr_kvstore_util
      openr_kvstore_wrapper
      kv_store_cpp2
      glog::glog
      gflags
      Folly::folly
    DESTINATION sbin/tests/openr/kvstore
  )

  add_openr_test(InterfaceEntryTest interface_entry_test
    SOURCES
      openr/link-monitor/tests/InterfaceEntryTest.cpp
    LIBRARIES
      openr_link_monitor
      openr_common
      openr_network_util
      glog::glog
      gflags
      Folly::folly
      ${RE2}
    DESTINATION sbin/tests/openr/link-monitor
  )

  add_openr_test(LinkMonitorTest link_monitor_test
    SOURCES
      openr/link-monitor/tests/LinkMonitorTest.cpp
      openr/tests/mocks/NetlinkEventsInjector.cpp
    LIBRARIES
      openr_link_monitor
      openr_prefix_manager
      openr_kvstore_wrapper
      openr_config
      openr_network_util
      openr_common
      openr_util
      openr_test_utils
      openr_mock_netlink_protocol_socket
    DESTINATION sbin/tests/openr/link-monitor
  )

  if(ADD_ROOT_TESTS)
    # This test fails under Travis, so adding it as an exception
    add_openr_test(FibTest fib_test
      SOURCES
        openr/fib/tests/FibTest.cpp
        openr/tests/mocks/MockNetlinkFibHandler.cpp
      LIBRARIES
        openr_fib
        openr_ctrl_handler
        openr_rib_entry
        openr_decision_structs
        openr_messaging
        openr_test_utils
        openr_config
        openr_network_util
      DESTINATION sbin/tests/openr/fib
    )
  endif()

  add_openr_test(NetlinkTypesTest netlink_types_test
    SOURCES
      openr/nl/tests/NetlinkTypesTest.cpp
    LIBRARIES
      openr_fbnl
      glog::glog
      gflags
    DESTINATION sbin/tests/openr/nl
  )

  if(ADD_ROOT_TESTS)
    # Build this privileged test for compile coverage. It remains disabled
    # in CTest and must be invoked manually as root.
    add_openr_test(NetlinkProtocolSocketTest netlink_message_test
      SOURCES
        openr/nl/tests/NetlinkProtocolSocketTest.cpp
      LIBRARIES
        openr_fbnl
        openr_util
        platform_cpp2
        network_cpp2
        fb303::fb303
        Folly::folly
        FBThrift::thriftcpp2
        glog::glog
        gflags
      DESTINATION sbin/tests/openr/nl
    )
    set_tests_properties(NetlinkProtocolSocketTest PROPERTIES DISABLED TRUE)
  endif()

  add_openr_test(PrefixManagerTest prefix_manager_test
    SOURCES
      openr/prefix-manager/tests/PrefixManagerTest.cpp
    LIBRARIES
      openr_prefix_manager
      openr_kvstore_wrapper
      openr_test_utils
      openr_config
      openr_rib_entry
      openr_messaging
      openr_network_util
      openr_util
    DESTINATION sbin/tests/openr/prefix-manager
  )

  add_openr_test(SparkTest spark_test
    SOURCES
      openr/spark/tests/SparkTest.cpp
      openr/tests/mocks/MockIoProvider.cpp
      openr/tests/mocks/MockIoProviderUtils.cpp
    LIBRARIES
      openr_spark_wrapper
      openr_test_utils
      openr_config
      openr_mpls_util
      openr_network_util
      openr_constants
    DESTINATION sbin/tests/openr/spark
  )
  set_tests_properties(SparkTest PROPERTIES TIMEOUT 600)

  add_openr_test(MockIoProviderTest mock_io_provider_test
    SOURCES
      openr/tests/MockIoProviderTest.cpp
      openr/tests/mocks/MockIoProvider.cpp
      openr/tests/mocks/MockIoProviderUtils.cpp
    LIBRARIES
      openr_io_provider
      openr_constants
      openr_network_util
      fmt::fmt
      Folly::folly
      glog::glog
      gflags
      Threads::Threads
    DESTINATION sbin/tests/openr/spark
  )

  add_openr_test(MessagingQueueTest queue_test
    SOURCES
      openr/messaging/tests/QueueTest.cpp
    LIBRARIES
      openr_messaging
      Folly::folly
    DESTINATION sbin/tests/openr/messaging
  )

  add_openr_test(MessagingReplicateQueueTest replicate_queue_test
    SOURCES
      openr/messaging/tests/ReplicateQueueTest.cpp
    LIBRARIES
      openr_messaging
      Folly::folly
    DESTINATION sbin/tests/openr/messaging
  )

  add_openr_test(NetlinkFibHandlerTest netlink_fib_handler_test
    SOURCES
      openr/platform/tests/NetlinkFibHandlerTest.cpp
    LIBRARIES
      openr_netlink_fib_handler
      openr_mock_netlink_protocol_socket
      openr_lsdb_util
      openr_network_util
    DESTINATION sbin/tests/openr/platform
  )

  add_openr_test(WatchdogTest watchdog_test
    SOURCES
      openr/watchdog/tests/WatchdogTest.cpp
    LIBRARIES
      openr_watchdog
      openr_test_utils
      openr_common
      openr_messaging
    DESTINATION sbin/tests/openr/watchdog
  )

  #
  # benchmarks
  #

  add_executable(config_store_benchmark
    openr/config-store/tests/PersistentStoreBenchmark.cpp
  )

  target_link_libraries(config_store_benchmark
    openr_persistent_store_wrapper
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
    openr_fib
    openr_ctrl_handler
    openr_config
    openr_messaging
    openr_system_metrics
    openr_test_utils
    openr_prefix_generator
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
    openr_netlink_fib_handler
    openr_mock_netlink_protocol_socket
    openr_prefix_generator
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
    openr_routing_benchmark_utils
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

  add_executable(dispatcher_queue_benchmark
    openr/dispatcher/tests/DispatcherQueueBenchmark.cpp
  )

  target_link_libraries(dispatcher_queue_benchmark
    openr_dispatcher_queue
    openr_util
    kv_store_cpp2
    fmt::fmt
    Folly::folly
    ${FOLLY_EXCEPTION_TRACER}
    ${BENCHMARK}
    glog::glog
  )

  install(TARGETS
    dispatcher_queue_benchmark
    DESTINATION sbin/tests/openr/dispatcher
  )

  add_executable(kvstore_benchmark
    openr/kvstore/tests/KvStoreBenchmark.cpp
  )

  target_link_libraries(kvstore_benchmark
    openr_kvstore
    openr_kvstore_util
    openr_kvstore_wrapper
    openr_system_metrics
    openr_test_utils
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${BENCHMARK}
  )

  install(TARGETS
    kvstore_benchmark
    DESTINATION sbin/tests/openr/kvstore
  )

endif()
