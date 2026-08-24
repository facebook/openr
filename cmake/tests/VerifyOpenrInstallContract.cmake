# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

foreach(required_variable
    OPENR_BINARY_DIR
    OPENR_COMPONENT_TARGET_EXISTS
    OPENR_AGGREGATE_TARGET_EXISTS)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

if(NOT OPENR_COMPONENT_TARGET_EXISTS)
  message(FATAL_ERROR "The granular openr_common target is missing")
endif()

if(OPENR_AGGREGATE_TARGET_EXISTS)
  message(FATAL_ERROR "The removed openrlib target is still generated")
endif()

set(install_script "${OPENR_BINARY_DIR}/cmake_install.cmake")
if(NOT EXISTS "${install_script}")
  message(FATAL_ERROR "Open/R install script is missing: ${install_script}")
endif()

file(READ "${install_script}" install_rules)
string(FIND "${install_rules}" "libopenrlib" aggregate_install_index)
if(NOT aggregate_install_index EQUAL -1)
  message(FATAL_ERROR "The removed openrlib archive is still installed")
endif()
