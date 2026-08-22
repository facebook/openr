# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Model the header-only Buck //openr/messaging:messaging target.
#
# Keeping the queue primitives as a leaf avoids artificial dependencies
# between Monitor and future consumers.
macro(openr_add_messaging_library)
  add_library(openr_messaging INTERFACE)
  # Messaging is header-only, so consumers must inherit the language level
  # required by the current Folly headers directly from this target.
  target_compile_features(openr_messaging INTERFACE cxx_std_20)
  target_link_libraries(openr_messaging INTERFACE Folly::folly)
  add_library(OpenR::messaging ALIAS openr_messaging)
endmacro()
