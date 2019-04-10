cmake_minimum_required(VERSION 3.2)
project(openr)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")
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

find_library(ASYNC async PATHS)
find_library(CONCURRENCY concurrency PATHS)
find_library(DOUBLE-CONVERSION double-conversion)
find_library(FBZMQ fbzmq)
find_library(FOLLY folly PATHS)
find_library(FOLLY_EXCEPTION_TRACER folly_exception_tracer PATHS)
find_library(FIZZ fizz PATHS)
find_library(GFLAGS gflags)
find_library(GLOG glog)
find_library(NL3 nl-3)
find_library(NLROUTE3 nl-route-3)
find_library(PROTOCOL protocol PATHS)
find_library(RE2 re2)
find_library(SODIUM sodium)
find_library(SIGAR sigar)
find_library(THRIFT thrift PATHS)
find_library(THRIFTCPP2 thriftcpp2 PATHS)
find_library(THRIFTPROTOCOL thriftprotocol PATHS)
find_library(TRANSPORT transport PATHS)
find_library(ZMQ zmq)
find_library(ZSTD zstd)

# FOLLY_EXCEPTION_TRACER may not be found
if(NOT FOLLY_EXCEPTION_TRACER)
  set(FOLLY_EXCEPTION_TRACER "")
endif()

find_path(LIBNL3-HEADERS libnl3/netlink/netlink.h)

# Include Thrift
find_program(THRIFT1 thrift1)
find_path(THRIFT_COMPILER_INCLUDE thrift/templates)
set(THRIFT_TEMPLATES ${THRIFT_COMPILER_INCLUDE}/thrift/templates)
include(${THRIFT_COMPILER_INCLUDE}/thrift/ThriftLibrary.cmake)

set(THRIFT_DIR ${CMAKE_BINARY_DIR}/thrift-out)
include_directories(${THRIFT_DIR})

file(MAKE_DIRECTORY ${THRIFT_DIR}/openr/if)

thrift_object(
  "PersistentStore" #file_name
  "" #services
  "cpp2" #language
  "optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

thrift_object(
  "KvStore" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

add_dependencies(
  KvStore-cpp2-obj
  Dual-cpp2-obj
)

thrift_object(
  "Lsdb" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

thrift_object(
  "Dual" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

thrift_object(
  "OpenrCtrl" #file_name
  "OpenrCtrl" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

add_dependencies(
  Lsdb-cpp2-obj
  Network-cpp2-obj
)

thrift_object(
  "Network" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

thrift_object(
  "Fib" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

add_dependencies(
  Fib-cpp2-obj
  Network-cpp2-obj
  Lsdb-cpp2-obj
)

thrift_object(
  "HealthChecker" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

add_dependencies(
  HealthChecker-cpp2-obj
  Network-cpp2-obj
)

thrift_object(
  "LinkMonitor" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

add_dependencies(
  LinkMonitor-cpp2-obj
  Lsdb-cpp2-obj
  Spark-cpp2-obj
)

thrift_object(
  "Platform" #file_name
  "FibService;SystemService" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

add_dependencies(
  Platform-cpp2-obj
  Network-cpp2-obj
)

thrift_object(
  "Spark" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

add_dependencies(
  Spark-cpp2-obj
  Network-cpp2-obj
)

thrift_object(
  "AllocPrefix" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

add_dependencies(
  AllocPrefix-cpp2-obj
  Network-cpp2-obj
)

thrift_object(
  "PrefixManager" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

add_dependencies(
  PrefixManager-cpp2-obj
  Lsdb-cpp2-obj
)

thrift_object(
  "Decision" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/if" #file_path
  "${THRIFT_DIR}/openr/if" #output_path
  "openr/if" #include_prefix
)

add_dependencies(
  Decision-cpp2-obj
  Fib-cpp2-obj
  Lsdb-cpp2-obj
)

add_library(openrlib
  openr/allocators/PrefixAllocator.cpp
  openr/common/BuildInfo.cpp
  openr/common/ExponentialBackoff.cpp
  openr/common/NetworkUtil.cpp
  openr/common/OpenrEventLoop.cpp
  openr/common/Util.cpp
  openr/common/Constants.cpp
  openr/config-store/PersistentStore.cpp
  openr/config-store/PersistentStoreClient.cpp
  openr/ctrl-server/OpenrCtrlHandler.cpp
  openr/decision/Decision.cpp
  openr/decision/LinkState.cpp
  openr/decision-old/DecisionOld.cpp
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
  openr/watchdog/Watchdog.cpp
  $<TARGET_OBJECTS:AllocPrefix-cpp2-obj>
  $<TARGET_OBJECTS:Decision-cpp2-obj>
  $<TARGET_OBJECTS:Fib-cpp2-obj>
  $<TARGET_OBJECTS:HealthChecker-cpp2-obj>
  $<TARGET_OBJECTS:KvStore-cpp2-obj>
  $<TARGET_OBJECTS:LinkMonitor-cpp2-obj>
  $<TARGET_OBJECTS:Lsdb-cpp2-obj>
  $<TARGET_OBJECTS:Dual-cpp2-obj>
  $<TARGET_OBJECTS:Network-cpp2-obj>
  $<TARGET_OBJECTS:OpenrCtrl-cpp2-obj>
  $<TARGET_OBJECTS:PersistentStore-cpp2-obj>
  $<TARGET_OBJECTS:Platform-cpp2-obj>
  $<TARGET_OBJECTS:PrefixManager-cpp2-obj>
  $<TARGET_OBJECTS:Spark-cpp2-obj>
)

if (BUILD_SHARED_LIBS)
  set_target_properties(openrlib PROPERTIES VERSION 1.0.0 SOVERSION 1)
endif()

target_include_directories(openrlib
  PRIVATE
  ${LIBNL3-HEADERS}/libnl3
)

target_link_libraries(openrlib
  ${FBZMQ}
  ${DOUBLE-CONVERSION}
  ${ZMQ}
  ${GLOG}
  ${GFLAGS}
  ${THRIFT}
  ${THRIFTPROTOCOL}
  ${THRIFTCPP2}
  ${ASYNC}
  ${PROTOCOL}
  ${TRANSPORT}
  ${CONCURRENCY}
  ${ZSTD}
  ${FOLLY}
  ${FOLLY_EXCEPTION_TRACER}
  ${SODIUM}
  ${SIGAR}
  ${RE2}
  -lboost_system
  -lpthread
  ${NLROUTE3}
  ${NL3}
  -lcrypto
)

install(TARGETS
  openrlib
  DESTINATION lib
)

add_executable(openr
  openr/Main.cpp
  openr/common/Flags.cpp
)

target_include_directories(openr
  PRIVATE
  ${LIBNL3-HEADERS}/libnl3
)

target_link_libraries(openr
  openrlib
  ${FBZMQ}
  ${ZMQ}
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
  -lpthread
  ${NLROUTE3}
  ${NL3}
  -lcrypto
)

install(TARGETS
  openr
  DESTINATION sbin
)

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
  ${GLOG}
  ${GFLAGS}
  ${FOLLY}
  ${FOLLY_EXCEPTION_TRACER}
  ${THRIFT}
  ${ZSTD}
  ${THRIFTCPP2}
  ${ASYNC}
  ${PROTOCOL}
  ${TRANSPORT}
  ${CONCURRENCY}
  ${THRIFTPROTOCOL}
  ${SODIUM}
  ${SIGAR}
  ${ZMQ}
  ${SIGAR}
  -lboost_system
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

# thrift files
install(
  DIRECTORY ${CMAKE_SOURCE_DIR}/openr
  DESTINATION include
  FILES_MATCHING PATTERN "*.thrift"
)

# generated thrift headers
install(
  DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/thrift-out/openr
  DESTINATION include
  FILES_MATCHING PATTERN "*.h"
)
install(
  DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/thrift-out/openr
  DESTINATION include
  FILES_MATCHING PATTERN "*.tcc"
)

#
# Tests
#

option(BUILD_TESTS "BUILD_TESTS" ON)
option(ADD_ROOT_TESTS "ADD_ROOT_TESTS" ON)

if(BUILD_TESTS)

  enable_testing()

  find_library(GMOCK gmock)
  find_library(GMOCK_MAIN gmock_main)
  find_library(GTEST gtest)
  find_library(GTEST_MAIN gtest_main)

  add_executable(netlink_socket_sample
    openr/nl/examples/NetlinkSocketSample.cpp
  )

  target_include_directories(netlink_socket_sample
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(netlink_socket_sample
    openrlib
    ${GLOG}
    ${GFLAGS}
    ${FOLLY}
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

  target_include_directories(openr_system_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(openr_system_test
    openrlib
    ${FBZMQ}
    ${ZMQ}
    ${GLOG}
    ${GFLAGS}
    ${THRIFT}
    ${THRIFTCPP2}
    ${PROTOCOL}
    ${THRIFTPROTOCOL}
    ${ZSTD}
    ${FOLLY}
    ${FOLLY_EXCEPTION_TRACER}
    ${SODIUM}
    ${SIGAR}
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
    ${SIGAR}
    -lboost_system
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
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )
  target_link_libraries(range_allocator_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
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

  add_executable(exp_backoff_test
    openr/common/tests/ExponentialBackoffTest.cpp
  )
  add_executable(util_test
    openr/common/tests/UtilTest.cpp
  )

  target_link_libraries(exp_backoff_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )
  target_link_libraries(util_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
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
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
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
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
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
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
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
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )

  add_test(LinkStateTest link_state_test)

  install(TARGETS
    link_state_test
    DESTINATION sbin/tests/openr/decision
  )

  add_executable(health_checker_test
    openr/health-checker/tests/HealthCheckerTest.cpp
  )

  target_link_libraries(health_checker_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
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

  target_link_libraries(kvstore_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )
  target_link_libraries(kvstore_client_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )

  add_test(KvStoreTest kvstore_test)
  add_test(KvStoreClientTest kvstore_client_test)

  install(TARGETS
    kvstore_test
    kvstore_client_test
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
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
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
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
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
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )
  target_link_libraries(netlink_socket_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )
  target_link_libraries(netlink_message_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )
  target_link_libraries(netlink_socket_subscribe_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )

  add_test(NetlinkTypesTest netlink_types_test)
  if(ADD_ROOT_TESTS)
    # these tests must be run by root user
    add_test(NetlinkSocketTest netlink_socket_test)
    add_test(NetlinkMessageTest netlink_message_test)
    add_test(NetlinkSocketTest netlink_socket_subscribe_test)
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
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
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
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )
  target_link_libraries(mock_io_provider_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )

  add_test(SparkTest spark_test)
  add_test(MockIoProviderTest mock_io_provider_test)

  install(TARGETS
    spark_test
    mock_io_provider_test
    DESTINATION sbin/tests/openr/spark
  )

endif()
