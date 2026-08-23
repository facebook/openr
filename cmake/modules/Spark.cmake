# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sources owned by the three compiled targets in //openr/spark.
set(OPENR_IO_PROVIDER_SOURCES
  openr/spark/IoProvider.cpp
)

set(OPENR_SPARK_SOURCES
  openr/spark/Spark.cpp
)

set(OPENR_SPARK_WRAPPER_SOURCES
  openr/spark/SparkWrapper.cpp
)

set(OPENR_SPARK_EXPECTED_SOURCE_COUNT 3)

# Create the socket abstraction, neighbor-discovery engine, and its test
# control wrapper in dependency order. Keeping these targets separate lets
# non-Spark socket tests avoid the neighbor-discovery implementation.
macro(openr_add_spark_libraries)
  # Buck2 target: //openr/spark:io_provider
  openr_add_library(
    NAME openr_io_provider
    SOURCES ${OPENR_IO_PROVIDER_SOURCES}
    PRIVATE_DEPENDENCIES
      fmt::fmt
      glog::glog
    PUBLIC_DEPENDENCIES Folly::folly
  )
  add_library(OpenR::io_provider ALIAS openr_io_provider)

  # Buck2 target: //openr/spark:spark
  openr_add_library(
    NAME openr_spark
    SOURCES ${OPENR_SPARK_SOURCES}
    PRIVATE_DEPENDENCIES
      fb303::fb303
      openr_network_util
      glog::glog
    PUBLIC_DEPENDENCIES
      fmt::fmt
      openr_io_provider
      openr_constants
      openr_common
      openr_lsdb_util
      openr_step_detector
      openr_config
      openr_messaging
      types_cpp2
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::spark ALIAS openr_spark)

  # SparkWrapper is test support, but it is an independent Buck target and
  # has a narrow public API that can be validated without starting Spark.
  # Buck2 target: //openr/spark:spark_wrapper
  openr_add_library(
    NAME openr_spark_wrapper
    SOURCES ${OPENR_SPARK_WRAPPER_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_network_util
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_spark
      openr_constants
      openr_common
      openr_config
      openr_messaging
  )
  add_library(OpenR::spark_wrapper ALIAS openr_spark_wrapper)
endmacro()
