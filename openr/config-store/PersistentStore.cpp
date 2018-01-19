/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PersistentStore.h"

#include <chrono>

#include <folly/FileUtil.h>
#include <folly/io/IOBuf.h>

namespace openr {

using namespace std::chrono_literals;

PersistentStore::PersistentStore(
    const std::string& storageFilePath,
    const PersistentStoreUrl& socketUrl,
    fbzmq::Context& context)
    : storageFilePath_(storageFilePath),
      repSocket_(
          context, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      saveDbTimerBackoff_(100ms, 5s) {
  // Bind rep socket
  VLOG(3) << "PersistentStore: Binding server socket on url "
          << static_cast<std::string>(socketUrl);
  auto bindRet = repSocket_.bind(fbzmq::SocketUrl(socketUrl));
  if (bindRet.hasError()) {
    LOG(FATAL) << "Error binding socket url "
               << static_cast<std::string>(socketUrl);
  }

  eventLoop_.addSocket(
      fbzmq::RawZmqSocketPtr{*repSocket_},
      ZMQ_POLLIN,
      [this](int /* revents */) noexcept { processRequest(); });

  saveDbTimer_ = fbzmq::ZmqTimeout::make(&eventLoop_, [this]() noexcept {
    if (saveDatabaseToDisk()) {
      saveDbTimerBackoff_.reportSuccess();
    } else {
      // Report error and schedule next-try
      saveDbTimerBackoff_.reportError();
      saveDbTimer_->scheduleTimeout(
          saveDbTimerBackoff_.getTimeRemainingUntilRetry());
    }
  });

  // Load initial database. On failure we will just report error and continue
  // with empty database
  if (not loadDatabaseFromDisk()) {
    LOG(ERROR) << "Failed to load config-database from file: "
               << storageFilePath_;
  }
}

PersistentStore::~PersistentStore() {
  if (eventLoop_.isRunning()) {
    stop();
  }
  saveDatabaseToDisk();
}

void
PersistentStore::run() {
  eventLoop_.run();
}

void
PersistentStore::stop() {
  eventLoop_.stop();
  eventLoop_.waitUntilStopped();
}

void
PersistentStore::processRequest() {
  thrift::StoreResponse response;
  auto request = repSocket_.recvThriftObj<thrift::StoreRequest>(serializer_);
  if (request.hasError()) {
    LOG(ERROR) << "Error while reading request " << request.error();
    response.success = false;
    auto ret = repSocket_.sendThriftObj(response, serializer_);
    if (ret.hasError()) {
      LOG(ERROR) << "Error while sending response " << ret.error();
    }
    return;
  }

  // Generate response
  response.key = request->key;
  switch (request->requestType) {
  case thrift::StoreRequestType::STORE: {
    // Override previous value if any
    database_.keyVals[request->key] = request->data;
    response.success = true;
    break;
  }
  case thrift::StoreRequestType::LOAD: {
    auto it = database_.keyVals.find(request->key);
    const bool success = it != database_.keyVals.end();
    response.success = success;
    response.data = success ? it->second : "";
    break;
  }
  case thrift::StoreRequestType::ERASE: {
    response.success = database_.keyVals.erase(request->key) > 0;
    break;
  }
  default: {
    LOG(ERROR) << "Got unknown request.";
    response.success = false;
    break;
  }
  }

  // Send response
  auto ret = repSocket_.sendThriftObj(response, serializer_);
  if (ret.hasError()) {
    LOG(ERROR) << "Error while sending response " << ret.error();
  }

  // Schedule database save
  if (response.success and not saveDbTimer_->isScheduled()) {
    saveDbTimer_->scheduleTimeout(
        saveDbTimerBackoff_.getTimeRemainingUntilRetry());
  }
}

bool
PersistentStore::saveDatabaseToDisk() noexcept {
  // Write `databse_` to ioBuf
  auto queue = folly::IOBufQueue();
  serializer_.serialize(database_, &queue);
  auto ioBuf = queue.move();
  ioBuf->coalesce();

  // Write ioBuf to disk
  auto fileData = ioBuf->moveToFbString();
  if (not folly::writeFile(fileData, storageFilePath_.c_str())) {
    LOG(ERROR) << "Failed to write data to file '" << storageFilePath_
               << "'. Error (" << errno << "): " << strerror(errno);
    return false;
  }

  return true;
}

bool
PersistentStore::loadDatabaseFromDisk() noexcept {
  // Read data from file
  std::string fileData{""};
  if (not folly::readFile(storageFilePath_.c_str(), fileData)) {
    VLOG(1) << "Failed to read file contents from '" << storageFilePath_
               << "'. Error (" << errno << "): " << strerror(errno);
    return false;
  }

  // Parse data into `database_`
  try {
    auto ioBuf = folly::IOBuf::wrapBuffer(fileData.c_str(), fileData.size());
    thrift::StoreDatabase newDatabase;
    serializer_.deserialize(ioBuf.get(), newDatabase);
    database_ = std::move(newDatabase);
    return true;
  } catch (std::exception const& e) {
    LOG(ERROR) << "Failed to decode file content into StoreDatabase."
               << ". Error: " << folly::exceptionStr(e);
    return false;
  }
}

} // namespace openr
