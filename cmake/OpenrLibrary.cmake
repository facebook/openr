# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(OPENR_CMAKE_HELPER_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Define a granular Open/R library.
#
# Each source is compiled once in an OBJECT library. The small STATIC target
# gives consumers a narrow link dependency. Dependency lists are applied to
# both targets because consuming $<TARGET_OBJECTS:...> does not propagate
# usage requirements.
function(openr_add_library)
  set(one_value_args NAME)
  set(multi_value_args SOURCES PRIVATE_DEPENDENCIES PUBLIC_DEPENDENCIES)
  fb_cmake_parse_args(
    ARG
    ""
    "${one_value_args}"
    "${multi_value_args}"
    "${ARGN}"
  )

  if("${ARG_NAME}" STREQUAL "")
    message(FATAL_ERROR "openr_add_library requires NAME")
  endif()
  if("${ARG_SOURCES}" STREQUAL "")
    message(FATAL_ERROR "openr_add_library(${ARG_NAME}) requires SOURCES")
  endif()

  get_property(
    registered_sources GLOBAL PROPERTY OPENR_COMPONENT_SOURCES
  )
  foreach(source IN LISTS ARG_SOURCES)
    list(FIND registered_sources "${source}" source_index)
    if(NOT source_index EQUAL -1)
      message(
        FATAL_ERROR
        "Open/R source belongs to more than one component library: ${source}"
      )
    endif()
    list(APPEND registered_sources "${source}")
  endforeach()
  set_property(
    GLOBAL PROPERTY OPENR_COMPONENT_SOURCES "${registered_sources}"
  )

  set(object_target "${ARG_NAME}_objects")
  add_library(${object_target} OBJECT ${ARG_SOURCES})

  # Preserve the effective C++20 mode inherited by the legacy build through
  # generated Thrift and mvfst usage requirements, and publish that same mode
  # to granular-library consumers.
  target_compile_features(${object_target} PRIVATE cxx_std_20)
  if(BUILD_SHARED_LIBS)
    set_target_properties(
      ${object_target}
      PROPERTIES POSITION_INDEPENDENT_CODE ON
    )
  endif()
  target_link_libraries(
    ${object_target}
    PRIVATE
      # Both dependency sets provide usage requirements while compiling the
      # OBJECT target. Only the STATIC wrapper below publishes public usage
      # requirements to consumers.
      ${ARG_PRIVATE_DEPENDENCIES}
      ${ARG_PUBLIC_DEPENDENCIES}
  )

  add_library(${ARG_NAME} STATIC $<TARGET_OBJECTS:${object_target}>)
  target_compile_features(${ARG_NAME} PUBLIC cxx_std_20)
  target_link_libraries(
    ${ARG_NAME}
    PRIVATE
      ${ARG_PRIVATE_DEPENDENCIES}
    PUBLIC
      ${ARG_PUBLIC_DEPENDENCIES}
  )
endfunction()

# Register a CTest that verifies the installed Open/R CMake contract.
function(openr_add_install_contract_test)
  add_test(
    NAME OpenrInstallContractTest
    COMMAND
      ${CMAKE_COMMAND}
      -DOPENR_BINARY_DIR=${CMAKE_BINARY_DIR}
      -DOPENR_COMPONENT_TARGET_EXISTS=$<TARGET_EXISTS:openr_common>
      -DOPENR_AGGREGATE_TARGET_EXISTS=$<TARGET_EXISTS:openrlib>
      -P ${OPENR_CMAKE_HELPER_DIR}/tests/VerifyOpenrInstallContract.cmake
  )
  set_tests_properties(OpenrInstallContractTest PROPERTIES TIMEOUT 30)
endfunction()

# Register a CTest that validates one module's declared source ownership.
#
# SOURCE_VARIABLES names the lists in the module file rather than repeating
# source paths in the root CMakeLists file.
function(openr_add_source_group_test)
  set(one_value_args NAME SOURCE_GROUP_FILE EXPECTED_SOURCE_COUNT)
  set(multi_value_args SOURCE_VARIABLES)
  fb_cmake_parse_args(
    ARG
    ""
    "${one_value_args}"
    "${multi_value_args}"
    "${ARGN}"
  )

  if("${ARG_NAME}" STREQUAL "")
    message(FATAL_ERROR "openr_add_source_group_test requires NAME")
  endif()
  if("${ARG_SOURCE_GROUP_FILE}" STREQUAL "")
    message(
      FATAL_ERROR
      "openr_add_source_group_test(${ARG_NAME}) requires SOURCE_GROUP_FILE"
    )
  endif()
  if("${ARG_SOURCE_VARIABLES}" STREQUAL "")
    message(
      FATAL_ERROR
      "openr_add_source_group_test(${ARG_NAME}) requires SOURCE_VARIABLES"
    )
  endif()
  if("${ARG_EXPECTED_SOURCE_COUNT}" STREQUAL "")
    message(
      FATAL_ERROR
      "openr_add_source_group_test(${ARG_NAME}) requires EXPECTED_SOURCE_COUNT"
    )
  endif()

  string(REPLACE ";" "\\;" source_variables "${ARG_SOURCE_VARIABLES}")
  add_test(
    NAME ${ARG_NAME}
    COMMAND
      ${CMAKE_COMMAND}
      -DOPENR_SOURCE_ROOT=${CMAKE_SOURCE_DIR}
      -DOPENR_SOURCE_GROUP_FILE=${ARG_SOURCE_GROUP_FILE}
      -DOPENR_SOURCE_VARIABLES=${source_variables}
      -DOPENR_EXPECTED_SOURCE_COUNT=${ARG_EXPECTED_SOURCE_COUNT}
      -P ${OPENR_CMAKE_HELPER_DIR}/tests/VerifyOpenrSourceGroup.cmake
  )
endfunction()

# Create a test executable, register it with CTest, and optionally install it
# for getdeps/TPX discovery. Tests name their Open/R and generated Thrift
# dependencies explicitly through LIBRARIES.
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
    ${GTEST_BOTH_LIBRARIES}
    ${LIBGMOCK_LIBRARIES}
    ${ARG_LIBRARIES}
  )
  add_test(${TEST_NAME} ${BIN_NAME})
  set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 300)
  if(NOT "${ARG_DESTINATION}" STREQUAL "")
    install(TARGETS
      ${BIN_NAME}
      DESTINATION ${ARG_DESTINATION}
    )
  endif()
endfunction()
