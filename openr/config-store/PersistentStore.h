/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#if __has_include("filesystem")
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif
#include <string>

#include <folly/futures/Future.h>
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
  std::optional<std::string> data;
};

/*
 * PersistentStore provides functionality of storing `Key-Values` with arbitrary
 * values which persists across restarts.
 *
 * `storageFilePath`: Describe the path of file in file system where data will
 * be stored/retrieved from (in binary format).
 */
class PersistentStore : public OpenrEventBase {
 public:
  PersistentStore(
      const std::string& storageFilePath,
      bool dryrun = false,
      bool periodicallySaveToDisk = true);

  // Destructor will try to save DB to disk before destroying the object
  ~PersistentStore() override;

  uint64_t
  getNumOfDbWritesToDisk() const {
    return numOfWritesToDisk_;
  }

  /**
   * Encode/Decode a PersistentObject, this can be private method, but for unit
   * test, we make it public
   */
  static folly::Expected<std::unique_ptr<folly::IOBuf>, std::string>
  encodePersistentObject(const PersistentObject& pObject) noexcept;
  static folly::Expected<std::optional<PersistentObject>, std::string>
  decodePersistentObject(folly::io::Cursor& cursor) noexcept;

  //
  // Public API
  //

  // Store key-value
  folly::SemiFuture<folly::Unit> store(std::string key, std::string value);

  // Get value for a key. `nullptr` will be returned if key doesn't exists
  folly::SemiFuture<std::optional<std::string>> load(std::string key);

  // Erase config
  folly::SemiFuture<bool> erase(std::string key);

  // Utility function to store thrift objects
  template <typename ThriftType>
  folly::SemiFuture<folly::Unit>
  storeThriftObj(std::string key, ThriftType const& value) noexcept {
    apache::thrift::CompactSerializer serializer;
    std::string result;
    serializer.serialize(value, &result);
    return store(std::move(key), std::move(result));
  }

  // Utility function to load thrift objects
  template <typename ThriftType>
  folly::SemiFuture<folly::Expected<ThriftType, folly::Unit>>
  loadThriftObj(std::string key) noexcept {
    auto futureVal = load(std::move(key));
    return std::move(futureVal).defer(
        [](folly::Try<std::optional<std::string>>&& value)
            -> folly::Expected<ThriftType, folly::Unit> {
          if (value.hasException() or not value->has_value()) {
            return folly::makeUnexpected(folly::Unit());
          }
          apache::thrift::CompactSerializer serializer;
          ThriftType obj;
          serializer.deserialize(*value.value(), obj);
          return obj;
        });
  }

 private:
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
  const fs::path storageFilePath_;

  // Dryrun to avoid disk writes in UTs
  bool dryrun_{false};

  // Timer for saving database to disk
  std::unique_ptr<folly::AsyncTimeout> saveDbTimer_;
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
