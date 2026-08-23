# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Production source from the mixed Buck config-store target.
set(OPENR_PERSISTENT_STORE_SOURCES openr/config-store/PersistentStore.cpp)

set(OPENR_CONFIG_STORE_EXPECTED_SOURCE_COUNT 1)

# Create the production Persistent Store library.
macro(openr_add_config_store_library)
  # Buck2 target production subset: //openr/config-store:config-store
  openr_add_library(
    NAME openr_persistent_store
    SOURCES ${OPENR_PERSISTENT_STORE_SOURCES}
    PRIVATE_DEPENDENCIES
      openr_util
      glog::glog
    PUBLIC_DEPENDENCIES
      openr_constants
      openr_common
      openr_config
      Folly::folly
      FBThrift::thriftcpp2
  )
  add_library(OpenR::persistent_store ALIAS openr_persistent_store)
endmacro()
