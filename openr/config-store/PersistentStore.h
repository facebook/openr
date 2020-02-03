/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>

#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/PersistentStore_types.h>

namespace {
constexpr folly::StringPiece kTlvFormatMarker{"TlvFormatMarker"};
enum WriteType { APPEND = 1, WRITE = 2 };

} // anonymous namespace

namespace openr {

enum ActionType {
  ADD = 1,
  DEL = 2,
};

struct PersistentObject {
  ActionType type;
  std::string key;
  folly::Optional<std::string> data;
};
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
class PersistentStore : public OpenrEventBase {
 public:
  PersistentStore(
      const std::string& nodeName,
      const std::string& storageFilePath,
      fbzmq::Context& context,
      // persistent store DB saving backoffs
      std::chrono::milliseconds saveInitialBackoff =
          Constants::kPersistentStoreInitialBackoff,
      std::chrono::milliseconds saveMaxBackoff =
          Constants::kPersistentStoreMaxBackoff,
      bool dryrun = false);

  // Destructor will try to save DB to disk before destroying the object
  ~PersistentStore() override;

  uint64_t
  getNumOfDbWritesToDisk() const {
    return numOfWritesToDisk_;
  }

  // Encode a PersistentObject, this can be private method, but for unit test,
  // we make it public
  static folly::Expected<std::unique_ptr<folly::IOBuf>, std::string>
  encodePersistentObject(const PersistentObject& pObject) noexcept;
  // Decode a PersistentObject, this can be private method, but for test,
  // we make it public
  folly::Expected<folly::Optional<PersistentObject>, std::string>
  decodePersistentObject(folly::io::Cursor& cursor) noexcept;

  // Public API
  folly::SemiFuture<folly::Unit> setConfigKey(
      std::string key, std::string value);
  folly::SemiFuture<folly::Unit> eraseConfigKey(std::string key);
  folly::SemiFuture<std::unique_ptr<std::string>> getConfigKey(std::string key);

 private:
  // Function to process pending request on reqSocket_
  folly::Expected<fbzmq::Message, fbzmq::Error> processRequestMsg(
      fbzmq::Message&& request) override;

  // Function to save/load `database_` to local disk. Returns true on success
  // else false. Doesn't throw exception.
  bool saveDatabaseToDisk() noexcept;
  bool loadDatabaseFromDisk() noexcept;

  // Load old format file from disk, this is for compatible with the old version
  folly::Expected<folly::Unit, std::string> loadDatabaseOldFormat(
      const std::unique_ptr<folly::IOBuf>& ioBuf) noexcept;

  // Load TlvFormat from disk
  folly::Expected<folly::Unit, std::string> loadDatabaseTlvFormat(
      const std::unique_ptr<folly::IOBuf>& ioBuf) noexcept;

  // Wrapper function to save persistent object to disk immediately or later
  void maybeSaveObjectToDisk() noexcept;

  // Function to save Persistent Object to local disk.
  bool savePersistentObjectToDisk() noexcept;

  // Write IoBuf ro local disk
  folly::Expected<folly::Unit, std::string> writeIoBufToDisk(
      const std::unique_ptr<folly::IOBuf>& ioBuf, WriteType writeType) noexcept;

  // Function to create a PersistentObject.
  PersistentObject toPersistentObject(
      const ActionType type, const std::string& key, const std::string& data);

  // Keeps track of number of writes of Database to disk
  std::atomic<std::uint64_t> numOfWritesToDisk_{0};

  // Keeps track of number of writes of PersistentObject to disk
  std::atomic<std::uint64_t> numOfNewWritesToDisk_{0};

  // Location on disk where data will be synced up. A file will be created
  // if doesn't exists.
  const std::string storageFilePath_;

  // Dryrun to avoid disk writes in UTs
  bool dryrun_{false};

  // Timer for saving database to disk
  std::unique_ptr<fbzmq::ZmqTimeout> saveDbTimer_;
  std::unique_ptr<ExponentialBackoff<std::chrono::milliseconds>>
      saveDbTimerBackoff_;

  // Database to store config data. It is synced up on a persistent storage
  // layer (disk) in a file.
  thrift::StoreDatabase database_;

  // Serializer for encoding/decoding of thrift objects
  apache::thrift::CompactSerializer serializer_;

  // Define a persistent object
  std::vector<PersistentObject> pObjects_;
};

} // namespace openr
