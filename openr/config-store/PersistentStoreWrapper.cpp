/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/config-store/PersistentStoreWrapper.h"

namespace openr {

PersistentStoreWrapper::PersistentStoreWrapper(const unsigned long tid)
    : filePath(folly::sformat("/tmp/aq_persistent_store_test_{}", tid)) {
  VLOG(1) << "PersistentStoreWrapper: Creating PersistentStore.";
  store_ = std::make_unique<PersistentStore>(filePath);
}

void
PersistentStoreWrapper::run() noexcept {
  storeThread_ = std::thread([this]() { store_->run(); });
  store_->waitUntilRunning();
}

void
PersistentStoreWrapper::stop() {
  // Return immediately if not running
  if (!store_->isRunning()) {
    return;
  }

  // Destroy socket for communicating with kvstore
  store_->stop();
  storeThread_.join();
}
} // namespace openr
