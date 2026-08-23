# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sources grouped by their Buck test-support ownership. PersistentStore's
# wrapper is separated from Buck's mixed production-and-test target here so
# production consumers do not inherit the shared test-utility dependency.
set(OPENR_PREFIX_GENERATOR_SOURCES
  openr/tests/mocks/PrefixGenerator.cpp
)
set(OPENR_MOCK_NETLINK_PROTOCOL_SOCKET_SOURCES
  openr/tests/mocks/MockNetlinkProtocolSocket.cpp
)
set(OPENR_TEST_UTILS_SOURCES openr/tests/utils/Utils.cpp)
set(OPENR_PERSISTENT_STORE_WRAPPER_SOURCES
  openr/config-store/PersistentStoreWrapper.cpp
)
set(OPENR_DECISION_TEST_UTILS_SOURCES
  openr/decision/tests/DecisionTestUtils.cpp
)
set(OPENR_ROUTING_BENCHMARK_UTILS_SOURCES
  openr/decision/tests/RoutingBenchmarkUtils.cpp
)

set(OPENR_TEST_SUPPORT_EXPECTED_SOURCE_COUNT 6)

# Create test-support libraries from leaf mocks toward composite helpers.
#
# These targets remain part of the physical openrlib compatibility archive,
# but tests can link only the support they use. Keeping the libraries
# separate prevents a change to one test helper from invalidating every
# Open/R test and benchmark.
macro(openr_add_test_support_libraries)
  # Buck2 target: //openr/tests/mocks:prefix_generator
  openr_add_library(
    NAME openr_prefix_generator
    SOURCES ${OPENR_PREFIX_GENERATOR_SOURCES}
    PRIVATE_DEPENDENCIES
      fmt::fmt
      Folly::folly
    PUBLIC_DEPENDENCIES
      openr_lsdb_util
      openr_network_util
  )
  add_library(OpenR::prefix_generator ALIAS openr_prefix_generator)

  # Buck2 target: //openr/tests/mocks:mock_netlink_protocol_socket
  openr_add_library(
    NAME openr_mock_netlink_protocol_socket
    SOURCES ${OPENR_MOCK_NETLINK_PROTOCOL_SOCKET_SOURCES}
    PRIVATE_DEPENDENCIES fb303::fb303
    PUBLIC_DEPENDENCIES
      openr_fbnl
      Folly::folly
  )
  add_library(
    OpenR::mock_netlink_protocol_socket
    ALIAS openr_mock_netlink_protocol_socket
  )

  # Buck2 target: //openr/tests/utils:utils
  openr_add_library(
    NAME openr_test_utils
    SOURCES ${OPENR_TEST_UTILS_SOURCES}
    PUBLIC_DEPENDENCIES
      openr_constants
      openr_common
      openr_mpls_util
      openr_util
      openr_config
      openr_decision_structs
      openr_kvstore_wrapper
      openr_messaging
      openr_prefix_generator
      kv_store_cpp2
      types_cpp2
      fmt::fmt
      Folly::folly
      FBThrift::thriftcpp2
      glog::glog
      Threads::Threads
  )
  add_library(OpenR::test_utils ALIAS openr_test_utils)

  # Buck2 target: test-support half of //openr/config-store:config-store
  openr_add_library(
    NAME openr_persistent_store_wrapper
    SOURCES ${OPENR_PERSISTENT_STORE_WRAPPER_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_test_utils
      fmt::fmt
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_persistent_store
      Folly::folly
      Threads::Threads
  )
  add_library(
    OpenR::persistent_store_wrapper ALIAS openr_persistent_store_wrapper
  )

  # Buck2 target: //openr/decision/tests:decision_test_utils
  openr_add_library(
    NAME openr_decision_test_utils
    SOURCES ${OPENR_DECISION_TEST_UTILS_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_util
      openr_test_utils
      fmt::fmt
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_spf_solver
      types_cpp2
      Folly::folly
  )
  add_library(OpenR::decision_test_utils ALIAS openr_decision_test_utils)

  # Buck2 target: //openr/decision/tests:routing_benchmark_utils
  openr_add_library(
    NAME openr_routing_benchmark_utils
    SOURCES ${OPENR_ROUTING_BENCHMARK_UTILS_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_config_cpp2
      openr_prefix_generator
      fmt::fmt
    PUBLIC_DEPENDENCIES
      openr_constants
      openr_util
      openr_config
      openr_decision
      openr_system_metrics
      openr_test_utils
      types_cpp2
      ${BENCHMARK}
      Folly::folly
      FBThrift::thriftcpp2
      glog::glog
      Threads::Threads
  )
  add_library(
    OpenR::routing_benchmark_utils ALIAS openr_routing_benchmark_utils
  )
endmacro()
