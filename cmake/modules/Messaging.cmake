# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Sources owned by the Buck //openr/messaging:messaging_stats target.
set(OPENR_MESSAGING_STATS_SOURCES openr/messaging/MessagingStats.cpp)

set(OPENR_MESSAGING_STATS_EXPECTED_SOURCE_COUNT 1)

# Model the header-only Buck //openr/messaging:messaging target, which
# re-exports :messaging_stats.
#
# Keeping the queue primitives as a leaf avoids artificial dependencies
# between Monitor and future consumers.
macro(openr_add_messaging_library)
  # Buck2 target: //openr/messaging:messaging_stats
  openr_add_library(
    NAME openr_messaging_stats
    SOURCES ${OPENR_MESSAGING_STATS_SOURCES}
    PRIVATE_DEPENDENCIES
      fmt::fmt
      Folly::folly
    PUBLIC_DEPENDENCIES
      fb303::fb303
  )
  add_library(OpenR::messaging_stats ALIAS openr_messaging_stats)

  add_library(openr_messaging INTERFACE)
  # Messaging is header-only, so consumers must inherit the language level
  # required by the current Folly headers directly from this target.
  target_compile_features(openr_messaging INTERFACE cxx_std_20)
  target_link_libraries(
    openr_messaging INTERFACE OpenR::messaging_stats Folly::folly)
  add_library(OpenR::messaging ALIAS openr_messaging)
endmacro()
