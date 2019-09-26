#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

cmake_minimum_required(VERSION 3.2)
project(openr)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-noexcept-type")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-maybe-uninitialized")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-declarations")

# Enable LTO by default
option(BUILD_WITH_LTO "BUILD_WITH_LTO" ON)
if (BUILD_WITH_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto")
endif()

include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_BINARY_DIR})

set(
  CMAKE_MODULE_PATH
  "${CMAKE_SOURCE_DIR}/build/fbcode_builder/CMake"
  ${CMAKE_MODULE_PATH}
)
include(FBThriftCppLibrary)

find_library(ASYNC async PATHS)
find_library(CONCURRENCY concurrency PATHS)
find_library(DOUBLE-CONVERSION double-conversion)
find_package(fbzmq REQUIRED)
find_package(folly REQUIRED)
find_package(fizz REQUIRED)
find_package(fmt REQUIRED)
find_package(Gflags REQUIRED)
find_package(Glog REQUIRED)
find_library(NL3 nl-3)
find_library(NLROUTE3 nl-route-3)
find_library(PROTOCOL protocol PATHS)
find_library(RE2 re2)
find_library(SODIUM sodium)
find_library(SIGAR sigar)
find_package(FBThrift REQUIRED)
find_package(wangle REQUIRED)
find_package(yarpl REQUIRED)
find_package(rsocket REQUIRED)
find_package(Threads REQUIRED)
find_library(ZMQ zmq)
find_library(ZSTD zstd)
find_library(BENCHMARK follybenchmark PATHS)

find_path(LIBNL3-HEADERS libnl3/netlink/netlink.h)
find_path(RE2_INCLUDE_DIR re2/re2.h)
find_path(SIGAR_INCLUDE_DIR sigar.h)
find_path(ZMQ_INCLUDE_DIR zmq.h)

set(FOLLY_EXCEPTION_TRACER)
if (TARGET Folly::folly_exception_tracer)
  set(FOLLY_EXCEPTION_TRACER Folly::folly_exception_tracer)
endif()

include_directories(
  ${Boost_INCLUDE_DIRS}
  ${FBTHRIFT_INCLUDE_DIR}
  ${FOLLY_INCLUDE_DIR}
  ${RE2_INCLUDE_DIR}
  ${SIGAR_INCLUDE_DIR}
  ${ZMQ_INCLUDE_DIR}
)

SET(OPENR_THRIFT_LIBS)

add_fbthrift_cpp_library(
  dual_cpp2
  openr/if/Dual.thrift
  OPTIONS
    json
    optionals
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} dual_cpp2)


add_fbthrift_cpp_library(
  network_cpp2
  openr/if/Network.thrift
  OPTIONS
    json
    optionals
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} network_cpp2)

add_fbthrift_cpp_library(
  fb303_cpp2
  common/fb303/if/fb303.thrift
  SERVICES
    FacebookService
  OPTIONS
    optionals
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} fb303_cpp2)

add_fbthrift_cpp_library(
  persistent_store_cpp2
  openr/if/PersistentStore.thrift
  OPTIONS
    json
    optionals
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} persistent_store_cpp2)

add_fbthrift_cpp_library(
  kv_store_cpp2
  openr/if/KvStore.thrift
  OPTIONS
    json
    optionals
  DEPENDS
    dual_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} kv_store_cpp2)

add_fbthrift_cpp_library(
  lsdb_cpp2
  openr/if/Lsdb.thrift
  OPTIONS
    json
    optionals
  DEPENDS
    network_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} lsdb_cpp2)

add_fbthrift_cpp_library(
  fib_cpp2
  openr/if/Fib.thrift
  OPTIONS
    json
    optionals
  DEPENDS
    network_cpp2
    lsdb_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} fib_cpp2)

add_fbthrift_cpp_library(
  health_checker_cpp2
  openr/if/HealthChecker.thrift
  OPTIONS
    json
    optionals
  DEPENDS
    network_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} health_checker_cpp2)

add_fbthrift_cpp_library(
  spark_cpp2
  openr/if/Spark.thrift
  OPTIONS
    json
    optionals
  DEPENDS
    network_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} spark_cpp2)

add_fbthrift_cpp_library(
  link_monitor_cpp2
  openr/if/LinkMonitor.thrift
  OPTIONS
    json
    optionals
  DEPENDS
    lsdb_cpp2
    spark_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} link_monitor_cpp2)

add_fbthrift_cpp_library(
  platform_cpp2
  openr/if/Platform.thrift
  SERVICES
    FibService
    SystemService
    NeighborListenerClientForFibagent
  OPTIONS
    json
    optionals
  DEPENDS
    fb303_cpp2
    network_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} platform_cpp2)

add_fbthrift_cpp_library(
  alloc_prefix_cpp2
  openr/if/AllocPrefix.thrift
  OPTIONS
    json
    optionals
  DEPENDS
    network_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} alloc_prefix_cpp2)

add_fbthrift_cpp_library(
  prefix_manager_cpp2
  openr/if/PrefixManager.thrift
  OPTIONS
    json
    optionals
  DEPENDS
    lsdb_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} prefix_manager_cpp2)

add_fbthrift_cpp_library(
  decision_cpp2
  openr/if/Decision.thrift
  OPTIONS
    json
    optionals
  DEPENDS
    lsdb_cpp2
    fib_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} decision_cpp2)

add_fbthrift_cpp_library(
  openr_ctrl_cpp2
  openr/if/OpenrCtrl.thrift
  SERVICES
    OpenrCtrl
  OPTIONS
    json
    optionals
  DEPENDS
    decision_cpp2
    fb303_cpp2
    dual_cpp2
    fib_cpp2
    health_checker_cpp2
    kv_store_cpp2
    link_monitor_cpp2
    lsdb_cpp2
    network_cpp2
    fbzmq::monitor_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} openr_ctrl_cpp2)

add_fbthrift_cpp_library(
  openr_ctrl_cpp_cpp2
  openr/if/OpenrCtrlCpp.thrift
  SERVICES
    OpenrCtrlCpp
  OPTIONS
    json
    optionals
    stream
  DEPENDS
    openr_ctrl_cpp2
    kv_store_cpp2
    fbzmq::monitor_cpp2
)
SET(OPENR_THRIFT_LIBS ${OPENR_THRIFT_LIBS} openr_ctrl_cpp_cpp2)

install(TARGETS
  ${OPENR_THRIFT_LIBS}
  DESTINATION lib
)

add_library(openrlib
  common/fb303/cpp/FacebookBase2.cpp
  openr/allocators/PrefixAllocator.cpp
  openr/common/BuildInfo.cpp
  openr/common/ExponentialBackoff.cpp
  openr/common/NetworkUtil.cpp
  openr/common/OpenrEventLoop.cpp
  openr/common/Util.cpp
  openr/common/Constants.cpp
  openr/common/ThriftUtil.cpp
  openr/config-store/PersistentStore.cpp
  openr/config-store/PersistentStoreClient.cpp
  openr/config-store/PersistentStoreWrapper.cpp
  openr/ctrl-server/OpenrCtrlHandler.cpp
  openr/decision/Decision.cpp
  openr/decision/LinkState.cpp
  openr/decision/PrefixState.cpp
  openr/dual/Dual.cpp
  openr/fib/Fib.cpp
  openr/health-checker/HealthChecker.cpp
  openr/kvstore/KvStoreClient.cpp
  openr/kvstore/KvStore.cpp
  openr/kvstore/KvStoreWrapper.cpp
  openr/link-monitor/LinkMonitor.cpp
  openr/link-monitor/InterfaceEntry.cpp
  openr/nl/NetlinkMessage.cpp
  openr/nl/NetlinkRoute.cpp
  openr/nl/NetlinkSocket.cpp
  openr/nl/NetlinkTypes.cpp
  openr/platform/NetlinkFibHandler.cpp
  openr/platform/NetlinkSystemHandler.cpp
  openr/platform/PlatformPublisher.cpp
  openr/plugin/Plugin.cpp
  openr/prefix-manager/PrefixManager.cpp
  openr/prefix-manager/PrefixManagerClient.cpp
  openr/spark/IoProvider.cpp
  openr/spark/SparkWrapper.cpp
  openr/spark/Spark.cpp
  openr/fib/tests/PrefixGenerator.cpp
  openr/tests/OpenrThriftServerWrapper.cpp
  openr/watchdog/Watchdog.cpp
)

if (BUILD_SHARED_LIBS)
  set_target_properties(openrlib PROPERTIES VERSION 1.0.0 SOVERSION 1)
endif()

target_include_directories(openrlib
  PRIVATE
  ${LIBNL3-HEADERS}/libnl3
)

target_link_libraries(openrlib
  ${OPENR_THRIFT_LIBS}
  fbzmq::fbzmq
  fbzmq::monitor_cpp2
  ${DOUBLE-CONVERSION}
  ${ZMQ}
  glog::glog
  gflags
  FBThrift::thriftcpp2
  ${ASYNC}
  ${PROTOCOL}
  ${CONCURRENCY}
  ${ZSTD}
  Folly::folly
  ${FOLLY_EXCEPTION_TRACER}
  ${SODIUM}
  ${SIGAR}
  ${RE2}
  -lboost_system
  -lboost_context
  -lpthread
  ${NLROUTE3}
  ${NL3}
  -lcrypto
)

install(TARGETS
  openrlib
  DESTINATION lib
)

add_executable(openr_bin
  openr/Main.cpp
  openr/common/Flags.cpp
)

target_include_directories(openr_bin
  PRIVATE
  ${LIBNL3-HEADERS}/libnl3
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
  ${PROTOCOL}
  ${CONCURRENCY}
  Folly::folly
  ${FOLLY_EXCEPTION_TRACER}
  ${SODIUM}
  ${SIGAR}
  -lboost_system
  -lboost_context
  -lpthread
  ${NLROUTE3}
  ${NL3}
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
    ${PROTOCOL}
    ${TRANSPORT}
    ${CONCURRENCY}
    ${THRIFTPROTOCOL}
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${SODIUM}
    ${SIGAR}
    -lboost_system
    -lboost_context
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
  openr/platform/NetlinkSystemHandler.cpp
)

target_include_directories(platform_linux
  PRIVATE
  ${LIBNL3-HEADERS}/libnl3
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
  ${PROTOCOL}
  ${CONCURRENCY}
  ${SODIUM}
  ${SIGAR}
  ${ZMQ}
  ${SIGAR}
  -lboost_system
  -lboost_context
  -lpthread
  ${NLROUTE3}
  ${NL3}
)

install(TARGETS
  platform_linux
  DESTINATION sbin
)

#
# Install files
#

# scripts
install(PROGRAMS
  ${CMAKE_CURRENT_SOURCE_DIR}/openr/scripts/run_openr.sh
  DESTINATION sbin
)

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

  include_directories(
    ${GTEST_INCLUDE_DIRS}
    ${GMOCK_INCLUDE_DIRS}
  )

  add_executable(netlink_socket_sample
    openr/nl/examples/NetlinkSocketSample.cpp
  )

  target_include_directories(netlink_socket_sample
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(netlink_socket_sample
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${GLOG}
    ${GFLAGS}
    Folly::folly
    ${FOLLY_EXCEPTION_TRACER}
    -lpthread
    ${NLROUTE3}
    ${NL3}
  )

  install(TARGETS
    netlink_socket_sample
    DESTINATION sbin
  )

  add_executable(openr_system_test
    openr/tests/OpenrSystemTest.cpp
    openr/tests/OpenrWrapper.cpp
    openr/spark/tests/MockIoProvider.cpp
    openr/tests/MockSystemHandler.cpp
  )

  add_library(openr_module_test_base
    openr/tests/OpenrModuleTestBase.cpp
  )

  target_link_libraries(openr_module_test_base
    openrlib
    fbzmq::fbzmq
    FBThrift::thriftcpp2
  )

  target_include_directories(openr_system_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(openr_system_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    fbzmq::fbzmq
    ${ZMQ}
    ${GLOG}
    ${GFLAGS}
    ${PROTOCOL}
    FBThrift::thriftcpp2
    ${ZSTD}
    Folly::folly
    ${FOLLY_EXCEPTION_TRACER}
    ${SODIUM}
    ${SIGAR}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
    ${SIGAR}
    -lboost_system
    -lboost_context
    -lpthread
    ${NLROUTE3}
    ${NL3}
    -lcrypto
  )

  add_test(OpenrSystemTest openr_system_test)

  install(TARGETS
    openr_system_test
    DESTINATION sbin/tests/openr
  )

  add_executable(prefix_allocator_test
    openr/allocators/tests/PrefixAllocatorTest.cpp
    openr/allocators/tests/MockSystemServiceHandler.cpp
  )

  target_include_directories(prefix_allocator_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  add_executable(range_allocator_test
    openr/allocators/tests/RangeAllocatorTest.cpp
  )

  target_link_libraries(prefix_allocator_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )
  target_link_libraries(range_allocator_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  if(ADD_ROOT_TESTS)
    # this test needs many file descriptors, must increase limit from default
    # on most systems to run successfully
    add_test(PrefixAllocatorTest prefix_allocator_test)
  endif()

  add_test(RangeAllocatorTest range_allocator_test)

  install(TARGETS
    prefix_allocator_test
    range_allocator_test
    DESTINATION sbin/tests/openr/allocators
  )

  add_executable(openr_ctrl_handler_test
    openr/ctrl-server/tests/OpenrCtrlHandlerTest.cpp
    openr/link-monitor/tests/MockNetlinkSystemHandler.cpp
  )

  target_include_directories(openr_ctrl_handler_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(openr_ctrl_handler_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  add_test(ExponentialBackoffTest openr_ctrl_handler_test)

  install(TARGETS
    openr_ctrl_handler_test
    DESTINATION sbin/tests/openr/ctrl-server
  )


  add_executable(exp_backoff_test
    openr/common/tests/ExponentialBackoffTest.cpp
  )
  add_executable(util_test
    openr/common/tests/UtilTest.cpp
  )

  target_link_libraries(exp_backoff_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )
  target_link_libraries(util_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  add_test(ExponentialBackoffTest exp_backoff_test)
  add_test(UtilTest util_test)

  install(TARGETS
    exp_backoff_test
    util_test
    DESTINATION sbin/tests/openr/common
  )

  add_executable(config_store_test
    openr/config-store/tests/PersistentStoreTest.cpp
  )

  target_link_libraries(config_store_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  add_test(PersistentStoreTest config_store_test)

  install(TARGETS
    config_store_test
    DESTINATION sbin/tests/openr/config-store
  )

  add_executable(decision_test
    openr/decision/tests/DecisionTest.cpp
  )

  target_link_libraries(decision_test
    openrlib
    openr_module_test_base
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  add_test(DecisionTest decision_test)

  install(TARGETS
    decision_test
    DESTINATION sbin/tests/openr/decision
  )

  add_executable(dual_test
    openr/dual/tests/DualTest.cpp
  )

  target_link_libraries(dual_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  add_test(DualTest dual_test)

  install(TARGETS
    dual_test
    DESTINATION sbin/tests/openr/dual
  )

  add_executable(link_state_test
    openr/decision/tests/LinkStateTest.cpp
  )

  target_link_libraries(link_state_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  add_test(LinkStateTest link_state_test)

  install(TARGETS
    link_state_test
    DESTINATION sbin/tests/openr/decision
  )

  add_executable(prefix_state_test
    openr/decision/tests/PrefixStateTest.cpp
  )

  target_link_libraries(prefix_state_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )

  add_test(PrefixStateTest prefix_state_test)

  install(TARGETS
    prefix_state_test
    DESTINATION sbin/tests/openr/decision
  )

  add_executable(health_checker_test
    openr/health-checker/tests/HealthCheckerTest.cpp
  )

  target_link_libraries(health_checker_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  add_test(HealthCheckerTest health_checker_test)

  install(TARGETS
    health_checker_test
    DESTINATION sbin/tests/openr/health-checker
  )

  add_executable(kvstore_test
    openr/kvstore/tests/KvStoreTest.cpp
  )
  add_executable(kvstore_client_test
    openr/kvstore/tests/KvStoreClientTest.cpp
  )
  add_executable(kvstore_thrift_client_test
    openr/kvstore/tests/KvStoreThriftClientTest.cpp
  )

  target_link_libraries(kvstore_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )
  target_link_libraries(kvstore_client_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )
  target_link_libraries(kvstore_thrift_client_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )

  add_test(KvStoreTest kvstore_test)
  add_test(KvStoreClientTest kvstore_client_test)
  add_test(KvStoreThriftClientTest kvstore_thrift_client_test)

  install(TARGETS
    kvstore_test
    kvstore_client_test
    kvstore_thrift_client_test
    DESTINATION sbin/tests/openr/kvstore
  )

  add_executable(link_monitor_test
    openr/link-monitor/tests/LinkMonitorTest.cpp
    openr/link-monitor/tests/MockNetlinkSystemHandler.cpp
  )

  target_include_directories(link_monitor_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(link_monitor_test
    openrlib
    openr_module_test_base
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  add_test(LinkMonitorTest link_monitor_test)

  install(TARGETS
    link_monitor_test
    DESTINATION sbin/tests/openr/link-monitor
  )

  add_executable(fib_test
    openr/fib/tests/FibTest.cpp
    openr/fib/tests/MockNetlinkFibHandler.cpp
  )

  target_include_directories(fib_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(fib_test
    openrlib
    openr_module_test_base
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  if(ADD_ROOT_TESTS)
    # This test fails under Travis, so adding it as an exception
    add_test(FibTest fib_test)
  endif()

  install(TARGETS
    fib_test
    DESTINATION sbin/tests/openr/fib
  )

  add_executable(netlink_message_test
    openr/nl/tests/NetlinkMessageTest.cpp
  )
  add_executable(netlink_types_test
    openr/nl/tests/NetlinkTypesTest.cpp
  )
  add_executable(netlink_socket_test
    openr/nl/tests/NetlinkSocketTest.cpp
  )
  add_executable(netlink_socket_subscribe_test
    openr/nl/tests/NetlinkSocketSubscribeTest.cpp
  )

  target_include_directories(netlink_types_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )
  target_include_directories(netlink_socket_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )
  target_include_directories(netlink_message_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )
  target_include_directories(netlink_socket_subscribe_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(netlink_types_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )
  target_link_libraries(netlink_socket_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )
  target_link_libraries(netlink_message_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )
  target_link_libraries(netlink_socket_subscribe_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  add_test(NetlinkTypesTest netlink_types_test)
  if(ADD_ROOT_TESTS)
    # these tests must be run by root user
    add_test(NetlinkSocketTest netlink_socket_test)
    add_test(NetlinkMessageTest netlink_message_test)
    add_test(NetlinkSocketSubscribeTest netlink_socket_subscribe_test)
  endif()

  install(TARGETS
    netlink_types_test
    netlink_socket_test
    netlink_socket_subscribe_test
    DESTINATION sbin/tests/openr/nl
  )

  add_executable(prefix_manager_test
    openr/prefix-manager/tests/PrefixManagerTest.cpp
  )

  target_link_libraries(prefix_manager_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  add_test(PrefixManagerTest prefix_manager_test)

  install(TARGETS
    prefix_manager_test
    DESTINATION sbin/tests/openr/prefix-manager
  )

  add_executable(spark_test
    openr/spark/tests/SparkTest.cpp
    openr/spark/tests/MockIoProvider.cpp
  )
  add_executable(mock_io_provider_test
    openr/spark/tests/MockIoProviderTest.cpp
    openr/spark/tests/MockIoProvider.cpp
  )

  target_link_libraries(spark_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )
  target_link_libraries(mock_io_provider_test
    openrlib
    ${OPENR_THRIFT_LIBS}
    ${LIBGMOCK_LIBRARIES}
    ${GTEST_BOTH_LIBRARIES}
  )

  add_test(SparkTest spark_test)
  add_test(MockIoProviderTest mock_io_provider_test)

  install(TARGETS
    spark_test
    mock_io_provider_test
    DESTINATION sbin/tests/openr/spark
  )

  #
  # benchmarks
  #

  add_executable(config_store_benchmark
    openr/config-store/tests/PersistentStoreBenchmark.cpp
  )

  target_include_directories(config_store_benchmark
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
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
    openr/fib/tests/MockNetlinkFibHandler.cpp
  )

  target_include_directories(fib_benchmark
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(fib_benchmark
    openrlib
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
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

  target_include_directories(netlink_fib_handler_benchmark
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(netlink_fib_handler_benchmark
    openrlib
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
    ${BENCHMARK}
  )

  install(TARGETS
    netlink_fib_handler_benchmark
    DESTINATION sbin/tests/openr/platform
  )

  add_executable(decision_benchmark
    openr/decision/tests/DecisionBenchmark.cpp
  )

  target_include_directories(decision_benchmark
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(decision_benchmark
    openrlib
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${BENCHMARK}
  )

  install(TARGETS
    decision_benchmark
    DESTINATION sbin/tests/openr/decision
  )

  add_executable(kvstore_benchmark
    openr/kvstore/tests/KvStoreBenchmark.cpp
  )

  target_include_directories(kvstore_benchmark
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
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
