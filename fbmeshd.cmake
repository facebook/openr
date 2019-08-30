#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

cmake_minimum_required (VERSION 3.9)
project (fbmeshd)
enable_testing()

find_library(NL3 nl-3)
find_library(FBZMQ fbzmq)
find_library(FOLLY folly PATHS)
find_library(THRIFT thrift PATHS)
find_library(THRIFTCPP2 thriftcpp2 PATHS)
find_library(GLOG glog)
find_library(GFLAGS gflags)
find_library(SODIUM sodium)
find_library(ZMQ zmq)
find_library(THRIFTPROTOCOL thriftprotocol PATHS)
find_library(PROTOCOL protocol PATHS)
find_library(TRANSPORT transport PATHS)
find_library(ASYNC async PATHS)
find_library(CONCURRENCY concurrency PATHS)

find_path(LIBNL3-HEADERS libnl3/netlink/netlink.h)

# Include Thrift
set(
  THRIFT_GENERATE_THRIFT_INCLUDE_DIRECTORIES
  ${CMAKE_SOURCE_DIR}
  CACHE PATH "thrift include directory"
)
find_program(THRIFT1 thrift1)
find_path(THRIFT_COMPILER_INCLUDE thrift/templates)
set(THRIFT_TEMPLATES ${THRIFT_COMPILER_INCLUDE}/thrift/templates)
include(${CMAKE_SOURCE_DIR}/ThriftLibrary.cmake)

set(THRIFT_DIR ${CMAKE_BINARY_DIR}/thrift-out)
include_directories(${THRIFT_DIR})

file(MAKE_DIRECTORY ${THRIFT_DIR}/openr/fbmeshd/if)

thrift_object(
  "fbmeshd" #file_name
  "MeshService" #services
  "cpp2" #language
  "json,optionals" #options
  "${CMAKE_SOURCE_DIR}/openr/fbmeshd/if" #file_path
  "${THRIFT_DIR}/openr/fbmeshd/if" #output_path
  "openr/fbmeshd/if" #include_prefix
)

option(ENABLE_SYSTEMD_NOTIFY "ENABLE_SYSTEMD_NOTIFY" ON)

if(ENABLE_SYSTEMD_NOTIFY)
  add_definitions(-DENABLE_SYSTEMD_NOTIFY)
  find_library(SYSTEMD_LIBRARY NAMES systemd)
else()
  set(SYSTEMD_LIBRARY "")
endif()

# authsae requires the LINUX flag to use the appropriate includes
add_definitions(-DLINUX)
find_library(SAE_LIBRARY NAMES sae)

add_executable(fbmeshd
    openr/fbmeshd/main.cpp
    openr/fbmeshd/802.11s/AuthsaeCallbackHelpers.cpp
    openr/fbmeshd/802.11s/AuthsaeConfigHelpers.cpp
    openr/fbmeshd/802.11s/NetInterface.cpp
    openr/fbmeshd/802.11s/Nl80211Handler.cpp
    openr/fbmeshd/common/Constants.cpp
    openr/fbmeshd/gateway-11s-root-route-programmer/Gateway11sRootRouteProgrammer.cpp
    openr/fbmeshd/gateway-connectivity-monitor/GatewayConnectivityMonitor.cpp
    openr/fbmeshd/gateway-connectivity-monitor/RouteDampener.cpp
    openr/fbmeshd/gateway-connectivity-monitor/Socket.cpp
    openr/fbmeshd/gateway-connectivity-monitor/StatsClient.cpp
    openr/fbmeshd/nl/GenericNetlinkFamily.cpp
    openr/fbmeshd/route-update-monitor/RouteUpdateMonitor.cpp
    openr/fbmeshd/routing/MetricManager80211s.cpp
    openr/fbmeshd/routing/PeriodicPinger.cpp
    openr/fbmeshd/routing/Routing.cpp
    openr/fbmeshd/routing/SyncRoutes80211s.cpp
    openr/fbmeshd/routing/UDPRoutingPacketTransport.cpp
    $<TARGET_OBJECTS:fbmeshd-cpp2-obj>
)

# override with -DCMAKE_BUILD_TYPE=Release
SET(CMAKE_BUILD_TYPE Debug)

target_include_directories(fbmeshd
  PRIVATE
  ${LIBNL3-HEADERS}/libnl3
)

target_link_libraries(fbmeshd
  openrlib
  ${FBZMQ}
  ${THRIFT}
  ${THRIFTCPP2}
  ${GLOG}
  ${GFLAGS}
  ${SODIUM}
  ${ZMQ}
  ${THRIFTPROTOCOL}
  ${PROTOCOL}
  ${TRANSPORT}
  ${ASYNC}
  ${CONCURRENCY}
  ${NL3}
  ${SAE_LIBRARY}
  -lboost_program_options
  -lpthread
  -lcrypto
  -lnl-genl-3
  ${SYSTEMD_LIBRARY}
)
install(TARGETS fbmeshd
        RUNTIME DESTINATION sbin)

# install the fbmeshd startup script
install(PROGRAMS
  ${CMAKE_CURRENT_SOURCE_DIR}/openr/fbmeshd/scripts/run_fbmeshd.sh
  DESTINATION sbin
)

# ========================================================================
option(BUILD_TESTS "BUILD_TESTS" ON)

if(BUILD_TESTS)

  # add google test (only used by unit tests)
  find_package(GTest REQUIRED)
  include_directories(${GTEST_INCLUDE_DIRS})
  set(LIBS ${LIBS} ${GTEST_LIBRARIES})

  project (UnitTests)

  include_directories(${CMAKE_SOURCE_DIR}/../..)

  add_executable(Nl80211HandlerTest
      openr/fbmeshd/802.11s/AuthsaeCallbackHelpers.cpp
      openr/fbmeshd/802.11s/AuthsaeConfigHelpers.cpp
      openr/fbmeshd/802.11s/NetInterface.cpp
      openr/fbmeshd/802.11s/Nl80211Handler.cpp
      openr/fbmeshd/common/Constants.cpp
      openr/fbmeshd/nl/GenericNetlinkFamily.cpp
      openr/fbmeshd/tests/Nl80211HandlerTest.cpp
  )
  target_link_libraries(Nl80211HandlerTest ${LIBS})
  add_test(unittest-Nl80211Handler Nl80211HandlerTest)

  install(TARGETS Nl80211HandlerTest
          RUNTIME DESTINATION bin)

endif()
