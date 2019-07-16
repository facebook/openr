/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/config-store/PersistentStoreWrapper.h"

namespace openr {

PersistentStoreWrapper::PersistentStoreWrapper(
    fbzmq::Context& context,
    const unsigned long tid,
    // persistent store DB saving backoffs
    std::chrono::milliseconds saveInitialBackoff,
    std::chrono::milliseconds saveMaxBackoff)
    : nodeName(folly::sformat("1-{}", tid)),
      filePath(folly::sformat("/tmp/aq_persistent_store_test_{}", tid)) {
  VLOG(1) << "PersistentStoreWrapper: Creating PersistentStore.";
  store_ = std::make_shared<PersistentStore>(
      nodeName, filePath, context, saveInitialBackoff, saveMaxBackoff);

  // set sockUrl as inproc socketUrl
  sockUrl = store_->inprocCmdUrl;
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
