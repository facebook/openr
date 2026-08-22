# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Define a granular Open/R library without changing libopenrlib's contents.
#
# Each source is compiled once in an OBJECT library. The small STATIC target
# gives focused tests a narrow link dependency, while the same object files
# are collected into the historical libopenrlib compatibility archive.
# Dependency lists are applied to both targets because consuming
# $<TARGET_OBJECTS:...> does not propagate usage requirements.
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

  set_property(
    GLOBAL APPEND PROPERTY OPENR_COMPONENT_OBJECT_TARGETS ${object_target}
  )
endfunction()

# Return the registered object expressions for the compatibility archive.
function(openr_get_component_objects output_variable)
  get_property(
    component_targets GLOBAL PROPERTY OPENR_COMPONENT_OBJECT_TARGETS
  )
  set(component_objects)
  foreach(component_target IN LISTS component_targets)
    list(APPEND component_objects $<TARGET_OBJECTS:${component_target}>)
  endforeach()
  set(${output_variable} ${component_objects} PARENT_SCOPE)
endfunction()

# Create a test executable, register it with CTest, and optionally install it
# for getdeps/TPX discovery. Tests use the compatibility openrlib and all
# generated Thrift libraries by default. NO_DEFAULT_OPENR_LIBRARIES removes
# that umbrella dependency so a migrated test can name only its granular
# Open/R libraries through LIBRARIES.
function(add_openr_test TEST_NAME BIN_NAME)
  set(options NO_DEFAULT_OPENR_LIBRARIES)
  set(one_value_args DESTINATION)
  set(multi_value_args SOURCES LIBRARIES)
  fb_cmake_parse_args(
    ARG
    "${options}"
    "${one_value_args}"
    "${multi_value_args}"
    "${ARGN}"
  )

  add_executable(
    ${BIN_NAME}
    ${ARG_SOURCES}
  )
  set(openr_test_default_libraries openrlib ${OPENR_THRIFT_LIBS})
  if(ARG_NO_DEFAULT_OPENR_LIBRARIES)
    set(openr_test_default_libraries)
  endif()
  target_link_libraries(${BIN_NAME}
    ${openr_test_default_libraries}
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
