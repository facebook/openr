cmake_minimum_required(VERSION 3.2)
project(openr)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-noexcept-type")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-maybe-uninitialized")

include_directories(${CMAKE_SOURCE_DIR}/..)

find_library(ASYNC async PATHS)
find_library(CONCURRENCY concurrency PATHS)
find_library(DOUBLE-CONVERSION double-conversion)
find_library(FBZMQ fbzmq)
find_library(FOLLY folly PATHS)
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
find_library(SIGAR sigar)
find_library(ZSTD zstd)

find_path(LIBNL3-HEADERS libnl3/netlink/netlink.h)

# Include Thrift
find_program(THRIFT1 thrift1)
find_path(THRIFT_COMPILER_INCLUDE thrift/templates)
set(THRIFT_TEMPLATES ${THRIFT_COMPILER_INCLUDE}/thrift/templates)
include(${THRIFT_COMPILER_INCLUDE}/thrift/ThriftLibrary.cmake)

set(THRIFT_PREFIX "openr/if")
set(THRIFT_DIR ${CMAKE_BINARY_DIR}/thrift-out/${THRIFT_PREFIX})
file(MAKE_DIRECTORY ${THRIFT_DIR})

include_directories(${CMAKE_BINARY_DIR}/thrift-out)

thrift_object(
  "PersistentStore" #file_name
  "" #services
  "cpp2" #language
  "optionals" #options
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
)

thrift_object(
  "KvStore" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
)

thrift_object(
  "Lsdb" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
)

add_dependencies(
  Lsdb-cpp2-obj
  IpPrefix-cpp2-obj
)

thrift_object(
  "IpPrefix" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
)

thrift_object(
  "Fib" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
)

add_dependencies(
  Fib-cpp2-obj
  IpPrefix-cpp2-obj
  Lsdb-cpp2-obj
)

thrift_object(
  "HealthChecker" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
)

add_dependencies(
  HealthChecker-cpp2-obj
  IpPrefix-cpp2-obj
)

thrift_object(
  "LinkMonitor" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
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
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
)

add_dependencies(
  Platform-cpp2-obj
  IpPrefix-cpp2-obj
)

thrift_object(
  "Spark" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
)

add_dependencies(
  Spark-cpp2-obj
  IpPrefix-cpp2-obj
)

thrift_object(
  "AllocPrefix" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
)

add_dependencies(
  AllocPrefix-cpp2-obj
  IpPrefix-cpp2-obj
)

thrift_object(
  "PrefixManager" #file_name
  "" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
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
  "${CMAKE_SOURCE_DIR}/if" #file_path
  "${THRIFT_DIR}" #output_path
  "${THRIFT_PREFIX}" #include_prefix
)

add_dependencies(
  Decision-cpp2-obj
  Fib-cpp2-obj
  Lsdb-cpp2-obj
)

install(FILES
  ${CMAKE_SOURCE_DIR}/if/Lsdb.thrift
  ${CMAKE_SOURCE_DIR}/if/IpPrefix.thrift
  ${CMAKE_SOURCE_DIR}/if/Fib.thrift
  ${CMAKE_SOURCE_DIR}/if/Platform.thrift
  ${CMAKE_SOURCE_DIR}/if/Decision.thrift
  DESTINATION include/openr/if
)

add_library(openrlib
  allocators/PrefixAllocator.cpp
  common/BuildInfo.cpp
  common/ExponentialBackoff.cpp
  common/Util.cpp
  common/Constants.cpp
  config-store/PersistentStore.cpp
  config-store/PersistentStoreClient.cpp
  decision/Decision.cpp
  decision-old/DecisionOld.cpp
  fib/Fib.cpp
  health-checker/HealthChecker.cpp
  kvstore/KvStoreClient.cpp
  kvstore/KvStore.cpp
  kvstore/KvStoreWrapper.cpp
  link-monitor/LinkMonitor.cpp
  nl/NetlinkRouteSocket.cpp
  nl/NetlinkSocket.cpp
  nl/NetlinkSubscriber.cpp
  nl/NetlinkTypes.cpp
  platform/NetlinkFibHandler.cpp
  platform/NetlinkSystemHandler.cpp
  platform/PlatformPublisher.cpp
  prefix-manager/PrefixManager.cpp
  prefix-manager/PrefixManagerClient.cpp
  spark/IoProvider.cpp
  spark/SparkWrapper.cpp
  spark/Spark.cpp
  watchdog/Watchdog.cpp
  $<TARGET_OBJECTS:AllocPrefix-cpp2-obj>
  $<TARGET_OBJECTS:Decision-cpp2-obj>
  $<TARGET_OBJECTS:Fib-cpp2-obj>
  $<TARGET_OBJECTS:HealthChecker-cpp2-obj>
  $<TARGET_OBJECTS:IpPrefix-cpp2-obj>
  $<TARGET_OBJECTS:KvStore-cpp2-obj>
  $<TARGET_OBJECTS:LinkMonitor-cpp2-obj>
  $<TARGET_OBJECTS:Lsdb-cpp2-obj>
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
  ${SODIUM}
  ${SIGAR}
  ${RE2}
  ${SIGAR}
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
  Main.cpp
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
  platform/LinuxPlatformMain.cpp
  platform/NetlinkFibHandler.cpp
  platform/NetlinkSystemHandler.cpp
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

# intstall the openr startup script
install(PROGRAMS
  ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_openr.sh
  DESTINATION sbin
)

# install all the headers

install(FILES
  allocators/PrefixAllocator.h
  allocators/RangeAllocator.h
  allocators/RangeAllocator-inl.h
  DESTINATION include/openr/allocators
)

install(FILES
  common/AddressUtil.h
  common/BuildInfo.h
  common/Constants.h
  common/ExponentialBackoff.h
  common/StepDetector.h
  common/Types.h
  common/Util.h
  DESTINATION include/openr/common
)

install(FILES
  config-store/PersistentStore.h
  config-store/PersistentStoreClient.h
  DESTINATION include/openr/config-store
)

install(FILES
  decision/Decision.h
  DESTINATION include/openr/decision
)

install(FILES
  decision-old/DecisionOld.h
  DESTINATION include/openr/decision-old
)

install(FILES
  health-checker/HealthChecker.h
  DESTINATION include/openr/health-checker
)

install(FILES
  kvstore/KvStoreWrapper.h
  kvstore/KvStoreClient-inl.h
  kvstore/KvStoreClient.h
  kvstore/KvStore.h
  DESTINATION include/openr/kvstore
)

install(FILES
  link-monitor/LinkMonitor.h
  DESTINATION include/openr/link-monitor
)

install(FILES
  nl/NetlinkSubscriber.h
  nl/NetlinkException.h
  nl/NetlinkRouteSocket.h
  nl/NetlinkSocket.h
  nl/NetlinkTypes.h
  DESTINATION include/openr/nl
)

install(FILES
  platform/NetlinkSystemHandler.h
  platform/PlatformPublisher.h
  platform/NetlinkFibHandler.h
  DESTINATION include/openr/platform
)

install(FILES
  prefix-manager/PrefixManager.h
  prefix-manager/PrefixManagerClient.h
  DESTINATION include/openr/prefix-manager
)

install(FILES
  spark/IoProvider.h
  spark/SparkWrapper.h
  spark/Spark.h
  DESTINATION include/openr/spark
)

install(FILES
  watchdog/Watchdog.h
  DESTINATION include/openr/watchdog
)

install(FILES
  fib/Fib.h
  DESTINATION include/openr/fib
)

# Install the generated thrift headers
install(DIRECTORY ${THRIFT_DIR}/gen-cpp2 DESTINATION include/openr/if
        FILES_MATCHING PATTERN "*.h")
install(DIRECTORY ${THRIFT_DIR}/gen-cpp2 DESTINATION include/openr/if
        FILES_MATCHING PATTERN "*.tcc")


option(BUILD_TESTS "BUILD_TESTS" ON)
option(ADD_ROOT_TESTS "ADD_ROOT_TESTS" ON)

if(BUILD_TESTS)

  enable_testing()

  find_library(GMOCK gmock)
  find_library(GMOCK_MAIN gmock_main)
  find_library(GTEST gtest)
  find_library(GTEST_MAIN gtest_main)

  add_executable(netlink_socket_sample
    nl/examples/NetlinkSocketSample.cpp
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
    -lpthread
    ${NLROUTE3}
    ${NL3}
  )

  install(TARGETS
    netlink_socket_sample
    DESTINATION sbin
  )

  add_executable(netlink_route_sample
    nl/examples/NetlinkRouteSample.cpp
  )

  target_include_directories(netlink_route_sample
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(netlink_route_sample
    openrlib
    ${GLOG}
    ${GFLAGS}
    ${FOLLY}
    -lpthread
    ${NLROUTE3}
    ${NL3}
  )

  install(TARGETS
    netlink_route_sample
    DESTINATION sbin
  )

  add_executable(netlink_subscriber_sample
    nl/examples/NetlinkSubscriberSample.cpp
  )

  target_include_directories(netlink_subscriber_sample
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(netlink_subscriber_sample
    openrlib
    ${GLOG}
    ${GFLAGS}
    ${FOLLY}
    -lpthread
    ${NLROUTE3}
    ${NL3}
  )

  install(TARGETS
    netlink_subscriber_sample
    DESTINATION sbin
  )

  add_executable(openr_system_test
    tests/OpenrSystemTest.cpp
    tests/OpenrWrapper.cpp
    spark/tests/MockIoProvider.cpp
    tests/MockSystemHandler.cpp
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
    allocators/tests/PrefixAllocatorTest.cpp
    allocators/tests/MockSystemServiceHandler.cpp
  )

  target_include_directories(prefix_allocator_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  add_executable(range_allocator_test
    allocators/tests/RangeAllocatorTest.cpp
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
    common/tests/ExponentialBackoffTest.cpp
  )
  add_executable(util_test
    common/tests/UtilTest.cpp
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
    config-store/tests/PersistentStoreTest.cpp
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
    decision/tests/DecisionTest.cpp
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

  add_executable(health_checker_test
    health-checker/tests/HealthCheckerTest.cpp
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
    kvstore/tests/KvStoreTest.cpp
  )
  add_executable(kvstore_client_test
    kvstore/tests/KvStoreClientTest.cpp
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
    link-monitor/tests/LinkMonitorTest.cpp
    link-monitor/tests/MockNetlinkSystemHandler.cpp
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
    fib/tests/FibTest.cpp
    fib/tests/MockNetlinkFibHandler.cpp
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

  install(TARGETS
    fib_test
    DESTINATION sbin/tests/openr/fib
  )

  add_executable(netlink_route_socket_test
    nl/tests/NetlinkRouteSocketTest.cpp
  )
  add_executable(netlink_subscriber_test
    nl/tests/NetlinkSubscriberTest.cpp
  )
  add_executable(netlink_types_test
    nl/tests/NetlinkTypesTest.cpp
  )
  add_executable(netlink_socket_test
    nl/tests/NetlinkSocketTest.cpp
  )
  add_executable(netlink_socket_subscribe_test
    nl/tests/NetlinkSocketSubscribeTest.cpp
  )

  target_include_directories(netlink_route_socket_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )
  target_include_directories(netlink_subscriber_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )
  target_include_directories(netlink_types_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )
  target_include_directories(netlink_socket_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )
  target_include_directories(netlink_socket_subscribe_test
    PRIVATE
    ${LIBNL3-HEADERS}/libnl3
  )

  target_link_libraries(netlink_route_socket_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )
  target_link_libraries(netlink_subscriber_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
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
  target_link_libraries(netlink_socket_subscribe_test
    openrlib
    ${GMOCK}
    ${GMOCK_MAIN}
    ${GTEST}
    ${GTEST_MAIN}
  )

  if(ADD_ROOT_TESTS)
    # This test fails under Travis, so adding it as an exception
    add_test(FibTest fib_test)
    # these tests must be run by root user
    add_test(NetlinkRouteSocketTest netlink_route_socket_test)
    add_test(NetlinkSubscriberTest netlink_subscriber_test)
    add_test(NetlinkTypesTest netlink_types_test)
    add_test(NetlinkSocketTest netlink_socket_test)
    add_test(NetlinkSocketTest netlink_socket_subscribe_test)
  endif()

  install(TARGETS
    netlink_route_socket_test
    netlink_subscriber_test
    netlink_types_test
    DESTINATION sbin/tests/openr/nl
  )

  add_executable(prefix_manager_test
    prefix-manager/tests/PrefixManagerTest.cpp
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
    spark/tests/SparkTest.cpp
    spark/tests/MockIoProvider.cpp
  )
  add_executable(mock_io_provider_test
    spark/tests/MockIoProviderTest.cpp
    spark/tests/MockIoProvider.cpp
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
