/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>

#include <fbzmq/async/Runnable.h>
#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/ExponentialBackoff.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/PersistentStore_types.h>

namespace openr {

/**
 * PersistentStore provides functionality of storing `Key-Values` with arbitrary
 * values which persists across restarts.
 *
 * `storageFilePath`: Describe the path of file in file system where data will
 * be stored/retrieved from (in binary format).
 *
 * You can interact with this module via ZMQ-Socket APIs described in
 * PersistentStore.thrift file via `REP` socket.
 *
 * To facilitate the easier communication and avoid boilerplate code to interact
 * with PersistentStore use PersistentStoreClient which allows you to
 * load/save/erase entries with different value types like thrift-objects,
 * primitive types and strings.
 */
class PersistentStore : public fbzmq::Runnable {
 public:
  PersistentStore(
      const std::string& storageFilePath,
      const PersistentStoreUrl& socketUrl,
      fbzmq::Context& context);

  // Destructor will try to save DB to disk before destroying the object
  ~PersistentStore() override;

  //
  // Implementation of Runnable interface
  //

  void run() override;
  void stop() override;

  bool
  isRunning() const override {
    return eventLoop_.isRunning();
  }

  void
  waitUntilRunning() const override {
    eventLoop_.waitUntilRunning();
  }

  void
  waitUntilStopped() const override {
    eventLoop_.waitUntilStopped();
  }

 private:
  // Function to process pending request on reqSocket_
  void processRequest();

  // Function to save/load `database_` to local disk. Returns true on success
  // else false. Doesn't throw exception.
  bool saveDatabaseToDisk() noexcept;
  bool loadDatabaseFromDisk() noexcept;

  // Location on disk where data will be synced up. A file will be created
  // if doesn't exists.
  const std::string storageFilePath_;

  // Server(REP) socket which accepts requests from clients for load/store
  // of config data.
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> repSocket_;

  // Timer for saving database to disk
  std::unique_ptr<fbzmq::ZmqTimeout> saveDbTimer_;
  ExponentialBackoff<std::chrono::milliseconds> saveDbTimerBackoff_;

  // Database to store config data. It is synced up on a persistent storage
  // layer (disk) in a file.
  thrift::StoreDatabase database_;

  // Internal loop in which all events are processed sequentially
  fbzmq::ZmqEventLoop eventLoop_;

  // Serializer for encoding/decoding of thrift objects
  apache::thrift::CompactSerializer serializer_;
};

} // namespace openr
