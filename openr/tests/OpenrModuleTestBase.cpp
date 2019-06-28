/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/tests/OpenrModuleTestBase.h"

namespace openr {

void
OpenrModuleTestBase::startOpenrCtrlHandler(
    const std::string& nodeName,
    const std::unordered_set<std::string>& acceptablePeerNames,
    MonitorSubmitUrl const& monitorSubmitUrl,
    KvStoreLocalPubUrl const& kvStoreLocalPubUrl,
    fbzmq::Context& context) {
  LOG(INFO) << "Start openr-ctrl handler";

  // Create main-event-loop
  mainEvlThread_ = std::thread([&]() { mainEvl_.run(); });

  tm_ = apache::thrift::concurrency::ThreadManager::newSimpleThreadManager(
      1, false);
  tm_->threadFactory(
      std::make_shared<apache::thrift::concurrency::PosixThreadFactory>());
  tm_->start();

  // create openrCtrlHandler
  openrCtrlHandler_ = std::make_shared<OpenrCtrlHandler>(
      nodeName,
      acceptablePeerNames,
      moduleTypeToEvl_,
      monitorSubmitUrl,
      kvStoreLocalPubUrl,
      mainEvl_,
      context);
  openrCtrlHandler_->setThreadManager(tm_.get());

  LOG(INFO) << "Successfully started openr-ctrl handler";
}

void
OpenrModuleTestBase::stopOpenrCtrlHandler() {
  LOG(INFO) << "Stop openr-ctrl handler";

  // ATTN: moduleTypeToEvl maintains <shared_ptr> of OpenrEventLoop.
  //       Must cleanup. Otherwise, there will be additional ref count and
  //       cause OpenrEventLoop binding to the existing addr.
  moduleTypeToEvl_.clear();
  mainEvl_.stop();
  mainEvlThread_.join();
  openrCtrlHandler_.reset();
  tm_->join();

  LOG(INFO) << "Successfully stop openr-ctrl handler";
}

} // namespace openr
