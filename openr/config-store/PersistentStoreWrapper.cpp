/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/logging/xlog.h>

#include <openr/config-store/PersistentStoreWrapper.h>
#include <openr/tests/utils/Utils.h>

namespace openr {

PersistentStoreWrapper::PersistentStoreWrapper(const unsigned long tid)
    : filePath(folly::sformat("/tmp/openr_persistent_store_test_{}", tid)) {
  XLOG(DBG1) << "PersistentStoreWrapper: Creating PersistentStore.";
  auto tConfig = getBasicOpenrConfig();
  tConfig.persistent_config_store_path_ref() = filePath;
  auto config = std::make_shared<Config>(tConfig);
  store_ = std::make_unique<PersistentStore>(config);
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
