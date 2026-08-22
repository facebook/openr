# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

foreach(required_variable
    OPENR_SOURCE_ROOT
    OPENR_SOURCE_GROUP_FILE
    OPENR_SOURCE_VARIABLES
    OPENR_EXPECTED_SOURCE_COUNT)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

include("${OPENR_SOURCE_ROOT}/${OPENR_SOURCE_GROUP_FILE}")

set(openr_sources)
foreach(source_variable IN LISTS OPENR_SOURCE_VARIABLES)
  list(APPEND openr_sources ${${source_variable}})
endforeach()

list(LENGTH openr_sources openr_source_count)
if(NOT openr_source_count EQUAL OPENR_EXPECTED_SOURCE_COUNT)
  message(
    FATAL_ERROR
    "Expected ${OPENR_EXPECTED_SOURCE_COUNT} sources, found ${openr_source_count}"
  )
endif()

set(unique_openr_sources ${openr_sources})
list(REMOVE_DUPLICATES unique_openr_sources)
list(LENGTH unique_openr_sources unique_openr_source_count)
if(NOT unique_openr_source_count EQUAL openr_source_count)
  message(FATAL_ERROR "An Open/R source belongs to more than one source group")
endif()

foreach(openr_source IN LISTS openr_sources)
  if(NOT EXISTS "${OPENR_SOURCE_ROOT}/${openr_source}")
    message(FATAL_ERROR "Open/R source does not exist: ${openr_source}")
  endif()
endforeach()
