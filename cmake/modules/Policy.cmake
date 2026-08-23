# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# OSS source corresponding to the Buck //openr/policy:policy target.
set(OPENR_POLICY_SOURCES openr/policy/PolicyManager.cpp)

set(OPENR_POLICY_EXPECTED_SOURCE_COUNT 1)

# Create the OSS policy library. Internal Buck builds provide their policy
# implementation separately, so the two implementations must not be mixed.
macro(openr_add_policy_library)
  # Buck2 target: //openr/policy:policy
  openr_add_library(
    NAME openr_policy
    SOURCES ${OPENR_POLICY_SOURCES}
    PUBLIC_DEPENDENCIES
      openr_policy_struct
      routing_policy_cpp2
      types_cpp2
      FBThrift::thriftcpp2
  )
  add_library(OpenR::policy ALIAS openr_policy)
endmacro()
