cmake_minimum_required (VERSION 3.1)
project (fbmeshd)
enable_testing()

include_directories("${PROJECT_BINARY_DIR}")
include_directories($ENV{STAGING_INCDIR}/fbmeshd-if)

# add our own cmake modules
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules/")

# we only use modern c++
set (CMAKE_CXX_STANDARD 14)
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-long-long -pedantic -fno-omit-frame-pointer")

option(BUILD_WITH_LTO "BUILD_WITH_LTO" ON)
if (BUILD_WITH_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto -fno-fat-lto-objects")
endif()

# libnl
find_package(NL REQUIRED)
include_directories(${NL_INCLUDE_DIRS})
set(LIBS ${LIBS} ${NL_LIBRARIES})

# fbzeromq
find_package(FBZMQ REQUIRED)
include_directories(${FBZMQ_INCLUDE_DIRS})
set(LIBS ${LIBS} ${FBZMQ_LIBRARIES})

# folly
find_package(FOLLY REQUIRED)
include_directories(${FOLLY_INCLUDE_DIRS})
set(LIBS ${LIBS} ${FOLLY_LIBRARIES})

# thrift
find_package(THRIFT REQUIRED)
include_directories(${THRIFT_INCLUDE_DIRS})
set(LIBS ${LIBS} ${THRIFT_LIBRARIES})

# thriftcpp2
find_package(THRIFTCPP2 REQUIRED)
include_directories(${THRIFTCPP2_INCLUDE_DIRS})
set(LIBS ${LIBS} ${THRIFTCPP2_LIBRARIES})

# google log
find_package(GLOG REQUIRED)
include_directories(${GLOG_INCLUDE_DIRS})
set(LIBS ${LIBS} ${GLOG_LIBRARIES})

# google flags
find_package(GFLAGS REQUIRED)
include_directories(${GFLAGS_INCLUDE_DIRS})
set(LIBS ${LIBS} ${GFLAGS_LIBRARIES})

# sodium
find_package(SODIUM REQUIRED)
include_directories(${SODIUM_INCLUDE_DIRS})
set(LIBS ${LIBS} ${SODIUM_LIBRARIES})

# zeromq
find_package(ZMQ REQUIRED)
include_directories(${ZMQ_INCLUDE_DIRS})
set(LIBS ${LIBS} ${ZMQ_LIBRARIES})

# boost
find_package(Boost REQUIRED program_options)
include_directories(${Boost_INCLUDE_DIRS})
set(LIBS ${LIBS} ${Boost_LIBRARIES})

# pthreads
find_package (Threads)
set(LIBS ${LIBS} ${CMAKE_THREAD_LIBS_INIT})

find_library(FBMESH_THRIFT_LIBRARY NAMES fbmeshd-if)
set(LIBS ${LIBS} ${FBMESH_THRIFT_LIBRARY})

find_library(THRIFT_LIBRARY NAMES thrift)
set(LIBS ${LIBS} ${THRIFT_LIBRARY})

find_library(THRIFT_PROTOCOL_LIBRARY NAMES thriftprotocol)
set(LIBS ${LIBS} ${THRIFT_PROTOCOL_LIBRARY})

find_library(PROTOCOL_LIBRARY NAMES protocol)
set(LIBS ${LIBS} ${PROTOCOL_LIBRARY})

find_library(TRANSPORT_LIBRARY NAMES transport)
set(LIBS ${LIBS} ${TRANSPORT_LIBRARY})

find_library(ASYNC_LIBRARY NAMES async)
set(LIBS ${LIBS} ${ASYNC_LIBRARY})

find_library(CONCURRENCY_LIBRARY NAMES concurrency)
set(LIBS ${LIBS} ${CONCURRENCY_LIBRARY})

find_library(OPENRLIB NAMES openrlib)
set(LIBS ${LIBS} ${OPENRLIB})

option(ENABLE_SYSTEMD_NOTIFY "ENABLE_SYSTEMD_NOTIFY" ON)

if(ENABLE_SYSTEMD_NOTIFY)
  add_definitions(-DENABLE_SYSTEMD_NOTIFY)
  find_library(SYSTEMD_LIBRARY NAMES systemd)
  set(LIBS ${LIBS} ${SYSTEMD_LIBRARY})
endif()

# authsae requires the LINUX flag to use the appropriate includes
add_definitions(-DLINUX)
find_library(SAE_LIBRARY NAMES sae)
set(LIBS ${LIBS} ${SAE_LIBRARY})

find_library(CRYPTO_LIBRARY NAMES crypto)
set(LIBS ${LIBS} ${CRYPTO_LIBRARY})
include_directories(${LIBCRYPTO_INCLUDE_DIRS})

find_library(CONFIG_LIBRARY NAMES config)
set(LIBS ${LIBS} ${CONFIG_LIBRARY})

add_executable(fbmeshd
    ${PROJECT_SOURCE_DIR}/main.cpp
    ${PROJECT_SOURCE_DIR}/802.11s/AuthsaeCallbackHelpers.cpp
    ${PROJECT_SOURCE_DIR}/802.11s/AuthsaeConfigHelpers.cpp
    ${PROJECT_SOURCE_DIR}/802.11s/NetInterface.cpp
    ${PROJECT_SOURCE_DIR}/802.11s/Nl80211Handler.cpp
    ${PROJECT_SOURCE_DIR}/802.11s/PeerSelector.cpp
    ${PROJECT_SOURCE_DIR}/common/Constants.cpp
    ${PROJECT_SOURCE_DIR}/common/Util.cpp
    ${PROJECT_SOURCE_DIR}/gateway-11s-root-route-programmer/Gateway11sRootRouteProgrammer.cpp
    ${PROJECT_SOURCE_DIR}/gateway-connectivity-monitor/GatewayConnectivityMonitor.cpp
    ${PROJECT_SOURCE_DIR}/gateway-connectivity-monitor/MonitorClient.cpp
    ${PROJECT_SOURCE_DIR}/gateway-connectivity-monitor/RouteDampener.cpp
    ${PROJECT_SOURCE_DIR}/gateway-connectivity-monitor/Socket.cpp
    ${PROJECT_SOURCE_DIR}/mesh-spark/MeshSpark.cpp
    ${PROJECT_SOURCE_DIR}/nl/GenericNetlinkFamily.cpp
    ${PROJECT_SOURCE_DIR}/openr-metric-manager/OpenRMetricManager.cpp
    ${PROJECT_SOURCE_DIR}/pinger/PeerPinger.cpp
    ${PROJECT_SOURCE_DIR}/route-update-monitor/RouteUpdateMonitor.cpp
    ${PROJECT_SOURCE_DIR}/routing/PeriodicPinger.cpp
    ${PROJECT_SOURCE_DIR}/routing/Routing.cpp
    ${PROJECT_SOURCE_DIR}/separa/Separa.cpp
)

# override with -DCMAKE_BUILD_TYPE=Release
SET(CMAKE_BUILD_TYPE Debug)

target_link_libraries(fbmeshd ${LIBS})
install(TARGETS fbmeshd
        RUNTIME DESTINATION sbin)

# install the fbmeshd startup script
install(PROGRAMS
  ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_fbmeshd.sh
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
      ${PROJECT_SOURCE_DIR}/802.11s/AuthsaeCallbackHelpers.cpp
      ${PROJECT_SOURCE_DIR}/802.11s/AuthsaeConfigHelpers.cpp
      ${PROJECT_SOURCE_DIR}/802.11s/NetInterface.cpp
      ${PROJECT_SOURCE_DIR}/802.11s/Nl80211Handler.cpp
      ${PROJECT_SOURCE_DIR}/802.11s/PeerSelector.cpp
      ${PROJECT_SOURCE_DIR}/common/Constants.cpp
      ${PROJECT_SOURCE_DIR}/nl/GenericNetlinkFamily.cpp
      ${PROJECT_SOURCE_DIR}/tests/Nl80211HandlerTest.cpp
  )
  target_link_libraries(Nl80211HandlerTest ${LIBS})
  add_test(unittest-Nl80211Handler Nl80211HandlerTest)

  install(TARGETS Nl80211HandlerTest
          RUNTIME DESTINATION bin)

endif()
